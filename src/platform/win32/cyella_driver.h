#pragma once

#include "application/app_messages.h"
#include "application/cancellation_registry.h"
#include "application/git_workspace.h"
#include "application/process_runner.h"
#include "application/task_scheduler.h"
#include "application/workspace_files.h"
#include "domain/git_tool.h"
#include "infrastructure/git_operation_runner.h"
#include "platform/win32/askpass_bridge.h"
#include "platform/win32/operation_result_relay.h"

#include "luil/luil.h"

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace cyella::win32 {
    // luil logic thread에 꽂는 cyella 구동기다. 앱 상태를 소유하고 메시지를 처리해
    // 불변 frame을 게시한다. `handle`·`make_frame`은 logic thread에서만 불리고,
    // `make_close_message`·`shutdown_completed`는 UI thread도 부르므로 atomic으로 지킨다.
    //
    // 결과의 길: worker가 `operation_event`를 relay에 넣고 깨우기(`relay_drain_intent`)를 보낸다.
    // `handle`은 첫 줄에서 relay를 비워 workspace에 반영한다. 세션 상태가 정본이고 frame은 그것을
    // 읽어 짓는다.
    class cyella_driver final : public luil::win32::logic_driver
    {
    public:
        // `initial_repository`가 있으면 시작할 때 그 저장소를 연다(명령줄 인자).
        explicit cyella_driver(std::u8string initial_repository = {});
        cyella_driver(const cyella_driver&) = delete;
        cyella_driver& operator=(const cyella_driver&) = delete;
        ~cyella_driver() override;

        void start() override;
        void handle(luil::app_message message) override;
        [[nodiscard]] std::shared_ptr<const luil::win32::ui_frame> make_frame() override;
        [[nodiscard]] luil::app_message make_close_message() override;
        [[nodiscard]] bool shutdown_completed() const override;
        void cancel() noexcept override;
        void stop_workers() override;

        // delegate의 `on_started`가 host를 묶는다.
        [[nodiscard]] operation_result_relay& relay() noexcept
        {
            return relay_;
        }

        // 시작 전이거나 git을 찾지 못했으면 null이다.
        [[nodiscard]] const git_workspace* workspace() const noexcept
        {
            return workspace_.get();
        }

        // 인증(askpass)을 쓸 수 없는 이유다. 도우미나 서버가 없으면 인증이 필요한 작업은 즉시 실패하고,
        // UI는 이 진단을 한 번 보인다. 준비됐거나 시작 전이면 값이 없다.
        [[nodiscard]] const std::optional<diagnostic>& askpass_diagnostic() const noexcept
        {
            return askpass_.startup_diagnostic();
        }

    private:
        void drain_relay();
        void handle_intent(const luil::app_message& message);

        // 멤버 순서가 파괴 순서의 반대다. worker(scheduler)가 가장 먼저 멈춰야 실행기·relay를
        // 붙잡은 job이 남지 않는다.
        operation_result_relay relay_ {};
        // 서버의 연결 스레드가 질문을 relay로 보낸다. relay보다 먼저 파괴되어 스레드가 멈춘 뒤에야
        // relay가 사라진다. workspace가 서버를 가리키므로 workspace보다 나중에 파괴된다.
        askpass_bridge askpass_ { [relay = &relay_](operation_event event) { relay->post(luil::app_message { std::move(event) }); } };
        cancellation_registry registry_ {};
        std::unique_ptr<process_runner> process_runner_ {};
        std::unique_ptr<workspace_files> files_ {};
        git_tool_info git_tool_ {};
        std::unique_ptr<git_operation_runner> executor_ {};
        std::unique_ptr<git_workspace> workspace_ {};
        std::unique_ptr<task_scheduler> scheduler_ {};
        std::u8string initial_repository_ {};
        // 창의 실제 크기·배율이다. 첫 frame(메시지가 오기 전)은 기본값으로 배치한다.
        window_metrics_intent metrics_ {};
        std::atomic<bool> closed_ { false };
    };
} // namespace cyella::win32
