#include "platform/win32/hosting_bridge.h"

#include "domain/text_catalog.h"

#include <chrono>
#include <utility>

namespace cyella::win32 {
    namespace {
        // cyella가 보내는 User-Agent다 (GitHub API는 User-Agent를 요구한다).
        constexpr std::u8string_view user_agent { u8"cyella" };

        template<class value_type>
        hosting_result<value_type> failure_of(const hosting_error_kind kind, std::string message)
        {
            hosting_result<value_type> result {};
            result.error.kind = kind;
            result.error.message = std::move(message);
            return result;
        }

        hosting_payload failure_payload(const hosting_job& job, const hosting_error_kind kind, std::string message)
        {
            if (std::holds_alternative<hosting_sign_in_job>(job.work))
                return failure_of<std::string>(kind, std::move(message));
            if (std::holds_alternative<hosting_list_job>(job.work))
                return failure_of<pull_request_page>(kind, std::move(message));
            if (std::holds_alternative<hosting_create_job>(job.work))
                return failure_of<created_pull_request>(kind, std::move(message));
            return failure_of<ci_status_info>(kind, std::move(message));
        }
    } // namespace

    hosting_bridge::~hosting_bridge()
    {
        stop();
    }

    bool hosting_bridge::start(process_runner& runner, task_scheduler* const scheduler, cancellation_registry* const registry, options value, std::u8string& error)
    {
        options_ = std::move(value);
        scheduler_ = scheduler;
        registry_ = registry;
        transport_ = luil_hosting_transport::create(std::u8string { user_agent }, options_.use_system_proxy, error);
        if (transport_ == nullptr)
        {
            start_error_.assign(error.begin(), error.end());
            return false;
        }
        git_credential_source_options source_options {};
        source_options.git_executable = options_.git_executable;
        source_options.environment = options_.environment;
        credentials_ = std::make_unique<git_credential_source>(runner, std::move(source_options));
        service_ = std::make_unique<hosting_service>(*transport_, *credentials_, [this] { return now(); });
        return true;
    }

    std::int64_t hosting_bridge::now() const
    {
        if (options_.clock != nullptr)
            return options_.clock();
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void hosting_bridge::post_failure(const hosting_job& job, const hosting_error_kind kind, std::string message) const
    {
        if (options_.post == nullptr)
            return;
        hosting_result_intent result {};
        result.serial = job.serial;
        result.repository_id = job.repository_id;
        result.host_key = job.context.repository.host_key;
        result.payload = failure_payload(job, kind, std::move(message));
        result.signed_in = service_ != nullptr && service_->signed_in(result.host_key);
        result.at = now();
        options_.post(luil::app_message { std::move(result) });
    }

    void hosting_bridge::submit(hosting_job job)
    {
        if (service_ == nullptr || scheduler_ == nullptr)
        {
            // 전송을 세우지 못했다 (WinHTTP·client 생성 실패, git 없음). 원격을 모른다는 뜻이 아니므로 연결 오류로 알린다.
            post_failure(job, hosting_error_kind::network, start_error_.empty() ? tr_narrow(u8"hosting_remote.start_failed") : start_error_);
            return;
        }
        auto source { std::make_shared<process_cancellation_source>() };
        if (registry_ != nullptr)
            registry_->add(*source);
        operation_request request {};
        // 같은 호스트의 일감은 차례로 돈다: 자격 증명 도우미의 창이 겹쳐 뜨지 않고 401 뒤 재시도가 서로 어긋나지 않는다.
        request.repository_key = "hosting:" + job.context.repository.host_key;
        request.job = [this, job = std::move(job), source](const operation_context& context) {
            const process_cancellation_registration link {
                context.shutdown.cancellable() ? context.shutdown.register_callback([canceller = source->weak_canceller()]() { static_cast<void>(canceller()); }) : process_cancellation_registration {}
            };
            if (context.shutdown.cancelled())
            {
                post_failure(job, hosting_error_kind::cancelled);
                return;
            }
            run(job, source->token());
        };
        // 접수하지 못하면(종료 중) 일감은 호출되지 않는다. 결과를 기다리는 쪽이 없으니 알릴 것도 없다.
        static_cast<void>(scheduler_->submit(std::move(request)));
    }

    void hosting_bridge::run(const hosting_job& job, const process_cancellation_token& cancellation) const
    {
        hosting_result_intent result {};
        result.serial = job.serial;
        result.repository_id = job.repository_id;
        result.host_key = job.context.repository.host_key;
        if (const auto* const sign_in { std::get_if<hosting_sign_in_job>(&job.work) }; sign_in != nullptr)
            result.payload = service_->sign_in(job.context, cancellation);
        else if (const auto* const list { std::get_if<hosting_list_job>(&job.work) }; list != nullptr)
            result.payload = service_->list_pull_requests(job.context, list->query, cancellation);
        else if (const auto* const create { std::get_if<hosting_create_job>(&job.work) }; create != nullptr)
            result.payload = service_->create_pull_request(job.context, create->params, cancellation);
        else if (const auto* const ci { std::get_if<hosting_ci_job>(&job.work) }; ci != nullptr)
            result.payload = service_->ci_status(job.context, ci->sha, cancellation);
        result.signed_in = service_->signed_in(result.host_key);
        result.user = service_->signed_in_user(result.host_key);
        result.rate_limit = service_->last_rate_limit(result.host_key);
        result.at = now();
        if (options_.post != nullptr)
            options_.post(luil::app_message { std::move(result) });
    }

    void hosting_bridge::sign_out(const hosting_sign_out_request& request)
    {
        if (service_ != nullptr)
            service_->sign_out(request.host_key);
    }

    void hosting_bridge::cancel_all() noexcept
    {
        if (transport_ != nullptr)
            transport_->cancel_all();
    }

    void hosting_bridge::stop() noexcept
    {
        if (transport_ != nullptr)
            transport_->stop();
        if (service_ != nullptr)
            service_->sign_out_all();
    }
} // namespace cyella::win32
