#pragma once

#include "application/cancellation_registry.h"
#include "application/hosting_messages.h"
#include "application/process_request.h"
#include "application/process_runner.h"
#include "application/task_scheduler.h"
#include "infrastructure/git_credential_source.h"
#include "infrastructure/hosting_service.h"
#include "infrastructure/luil_hosting_transport.h"

#include "luil/ui/app_message.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace cyella::win32 {
    // GitHub·GitLab 호출의 조립부다. luil의 비동기 HTTP client 위에 선 전송, `git credential`로 토큰을 받는 자격 증명 어댑터,
    // 그 둘을 쥔 `hosting_service`를 한곳에서 소유한다. 일감(`hosting_job`)은 scheduler의 호스트별 lane에서 워커가 돌리고 결과(
    // `hosting_result_intent`)는 `post`로 logic thread에 돌려보낸다. 토큰은 서비스 안의 메모리에만 있고 종료할 때 지워진다.
    class hosting_bridge
    {
    public:
        struct options
        {
            // `git.exe` 절대 경로다.
            std::u8string git_executable {};
            // 거짓이면 시스템 proxy를 따르지 않는다 (되돌이 주소의 가짜 서버를 쓰는 시험·검증이 끈다).
            bool use_system_proxy { true };
            // 자격 증명 도우미를 부르는 git에 더할 환경 변수다. 시험이 사용자의 helper를 끊는 데 쓴다.
            std::vector<process_environment_override> environment {};
            std::function<void(luil::app_message)> post {};
            // 유닉스 초다. 비면 시스템 시계다.
            std::function<std::int64_t()> clock {};
        };

        hosting_bridge() = default;
        hosting_bridge(const hosting_bridge&) = delete;
        hosting_bridge& operator=(const hosting_bridge&) = delete;
        ~hosting_bridge();

        // 전송을 세운다. 실패하면 false이고 `error`에 이유가 있다 (일감을 내면 실패 결과가 돌아온다).
        [[nodiscard]] bool start(process_runner& runner, task_scheduler* scheduler, cancellation_registry* registry, options value, std::u8string& error);

        // 일감을 호스트별 lane에 올린다. 시작하지 못했거나 scheduler가 닫혔으면 실패 결과를 바로 돌려준다.
        void submit(hosting_job job);
        // 이 실행의 메모리 토큰을 잊는다 (도우미의 저장본은 그대로).
        void sign_out(const hosting_sign_out_request& request);

        // 날아가 있는 요청을 접는다 (종료 0단계).
        void cancel_all() noexcept;
        // 전송을 멈추고 토큰을 모두 지운다. 워커가 모두 멈춘 뒤에 부른다. 멱등이다.
        void stop() noexcept;

        [[nodiscard]] bool available() const noexcept
        {
            return service_ != nullptr;
        }

    private:
        void run(const hosting_job& job, const process_cancellation_token& cancellation) const;
        void post_failure(const hosting_job& job, hosting_error_kind kind, std::string message = {}) const;
        [[nodiscard]] std::int64_t now() const;

        options options_ {};
        // 전송을 세우지 못한 이유다. 일감마다 같은 이유로 돌려준다.
        std::string start_error_ {};
        task_scheduler* scheduler_ { nullptr };
        cancellation_registry* registry_ { nullptr };
        std::unique_ptr<luil_hosting_transport> transport_ {};
        std::unique_ptr<git_credential_source> credentials_ {};
        std::unique_ptr<hosting_service> service_ {};
    };
} // namespace cyella::win32
