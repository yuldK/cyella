#include "platform/win32/askpass_bridge.h"

#include "domain/text_catalog.h"
#include "platform/win32/win32_askpass_server.h"

#include <charconv>
#include <string>
#include <system_error>
#include <utility>

namespace cyella::win32 {
    namespace {
        // 도우미가 환경 변수로 받은 operation id(10진수)를 숫자로 돌린다. 알아볼 수 없으면 0이다.
        operation_id parse_operation_id(const std::u8string_view text) noexcept
        {
            operation_id value { 0 };
            const char* const first { reinterpret_cast<const char*>(text.data()) };
            const char* const last { first + text.size() };
            const auto [end, error] { std::from_chars(first, last, value) };
            if (error != std::errc {} || end != last)
                return 0;
            return value;
        }

        diagnostic make_diagnostic(const diagnostic_code code, std::u8string message, std::optional<std::uint32_t> native_error = std::nullopt)
        {
            diagnostic value {};
            value.code = code;
            value.severity = diagnostic_severity::warning;
            value.message = std::move(message);
            value.native_error = native_error;
            return value;
        }
    } // namespace

    askpass_bridge::askpass_bridge(operation_event_sink post)
        : post_ { std::move(post) }
    {}

    askpass_bridge::~askpass_bridge()
    {
        shutdown();
    }

    void askpass_bridge::start(std::optional<std::filesystem::path> helper)
    {
        if (started_)
            return;
        started_ = true;

        if (helper.has_value() == false)
        {
            startup_diagnostic_ = make_diagnostic(diagnostic_code::askpass_helper_missing, tr(u8"askpass.helper_missing"));
            return;
        }

        // 서버보다 오래 사는 것만 잡는다. 사건을 보내는 길(post)만 복사해 쥐고 this는 잡지 않는다.
        const auto handler = [post = post_](askpass_prompt_request request, askpass_reply_callback reply) {
            const operation_id id { parse_operation_id(request.operation_id) };
            // 어느 operation의 질문인지 모르면 대화 상자를 띄울 곳이 없다. 매달리지 않게 바로 취소한다.
            if (id == 0 || post == nullptr)
            {
                reply(askpass_reply::cancelled());
                return;
            }
            operation_event event {};
            event.id = id;
            event.payload = operation_input_requested { request.prompt_id, std::move(request.prompt), std::move(reply) };
            post(std::move(event));
        };

        askpass_server_start_result started { start_askpass_server(askpass_prompt_handler { handler }) };
        if (started.server == nullptr)
        {
            startup_diagnostic_ = make_diagnostic(diagnostic_code::askpass_server_failed, tr(u8"askpass.server_failed"), started.native_error);
            return;
        }
        server_ = std::move(started.server);
        helper_path_ = helper->generic_u8string();
    }

    askpass_endpoint askpass_bridge::endpoint() const
    {
        return server_ != nullptr ? server_->endpoint() : askpass_endpoint {};
    }

    void askpass_bridge::shutdown() noexcept
    {
        if (server_ != nullptr)
            server_->shutdown();
    }
} // namespace cyella::win32
