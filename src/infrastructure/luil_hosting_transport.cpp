#include "infrastructure/luil_hosting_transport.h"

#include <chrono>
#include <utility>

namespace cyella {
    namespace {
        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::string to_text(const std::u8string_view text)
        {
            return std::string { text.begin(), text.end() };
        }

        // 어느 호출이 어떤 표인지 모르는 채 `deliver`가 먼저 올 수 있으므로 표 값은 `send`가 돌려준 뒤에 찾는다.
        std::string describe_error(const luil::net::http_error& error)
        {
            return to_text(error.message);
        }
    } // namespace

    std::unique_ptr<luil_hosting_transport> luil_hosting_transport::create(std::u8string user_agent, const bool use_system_proxy, std::u8string& error)
    {
        std::unique_ptr<luil_hosting_transport> transport { new luil_hosting_transport {} };
        luil_hosting_transport* const raw { transport.get() };
        luil::net::http_client_config config {};
        config.user_agent = std::move(user_agent);
        config.use_system_proxy = use_system_proxy;
        config.deliver = [raw](luil::net::http_response response) { raw->deliver(std::move(response)); };
        transport->client_ = luil::net::http_client::create(std::move(config), error);
        if (transport->client_ == nullptr)
            return nullptr;
        return transport;
    }

    luil_hosting_transport::~luil_hosting_transport()
    {
        stop();
    }

    void luil_hosting_transport::deliver(luil::net::http_response response)
    {
        const std::lock_guard<std::mutex> lock { mutex_ };
        const std::uint64_t ticket { response.ticket.value };
        completed_[ticket] = std::move(response);
        arrived_.notify_all();
    }

    void luil_hosting_transport::cancel_all() noexcept
    {
        if (client_ != nullptr)
            client_->cancel_all();
    }

    void luil_hosting_transport::stop() noexcept
    {
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            stopped_ = true;
        }
        // client를 멈추는 동안 기다리던 호출이 깨어나야 한다. 멈춘 뒤에는 `deliver`가 불리지 않는다.
        arrived_.notify_all();
        if (client_ != nullptr)
            client_->stop();
        arrived_.notify_all();
    }

    hosting_http_response luil_hosting_transport::execute(const hosting_http_request& request, const secret_text& bearer, const process_cancellation_token& cancellation)
    {
        hosting_http_response result {};
        luil::net::http_request prepared {};
        prepared.url = to_u8(request.url);
        prepared.method = request.method == hosting_http_method::post ? luil::net::http_method::post : luil::net::http_method::get;
        for (const hosting_http_header& header : request.headers)
            prepared.headers.push_back({ to_u8(header.name), to_u8(header.value) });
        if (bearer.empty() == false)
            prepared.headers.push_back({ u8"Authorization", to_u8("Bearer " + bearer.value()) });
        if (request.body.empty() == false)
        {
            prepared.body.assign(request.body.begin(), request.body.end());
            prepared.content_type = to_u8(request.content_type.empty() ? "application/json" : request.content_type);
        }
        // 몸을 풀지 않는다: JSON은 nlohmann으로 직접 읽는다.
        prepared.parse.assume_kind = luil::net::http_body_kind::bytes;
        prepared.total_timeout = std::chrono::seconds { 45 };

        std::u8string error {};
        const luil::net::http_ticket ticket { client_ != nullptr ? client_->send(std::move(prepared), error) : luil::net::http_ticket {} };
        if (ticket.value == 0)
        {
            result.transport_error = to_text(error);
            return result;
        }
        // 취소되면 표를 접는다. 접어도 답은 `cancelled`로 온다.
        const process_cancellation_registration link { cancellation.register_callback([this, ticket]() { client_->cancel(ticket); }) };

        luil::net::http_response response {};
        {
            std::unique_lock<std::mutex> lock { mutex_ };
            arrived_.wait(lock, [this, ticket] { return stopped_ || completed_.contains(ticket.value); });
            const auto found { completed_.find(ticket.value) };
            if (found == completed_.end())
            {
                result.cancelled = true;
                result.transport_error = "stopped";
                return result;
            }
            response = std::move(found->second);
            completed_.erase(found);
        }

        result.cancelled = response.error.kind == luil::net::http_error_kind::cancelled || response.error.kind == luil::net::http_error_kind::stopped;
        if (response.error.empty() == false)
        {
            result.transport_error = describe_error(response.error);
            return result;
        }
        result.status_code = response.status_code;
        for (const luil::net::http_header& header : response.headers)
            result.headers.push_back({ to_text(header.name), to_text(header.value) });
        const std::span<const std::uint8_t> bytes { response.body.data() };
        result.body.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return result;
    }
} // namespace cyella
