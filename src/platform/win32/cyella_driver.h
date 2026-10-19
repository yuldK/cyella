#pragma once

#include "application/app_messages.h"
#include "application/app_settings_service.h"
#include "application/cancellation_registry.h"
#include "application/file_probe.h"
#include "application/git_workspace.h"
#include "application/process_runner.h"
#include "application/settings_file_system.h"
#include "application/task_scheduler.h"
#include "application/workspace_files.h"
#include "domain/git_tool.h"
#include "infrastructure/git_operation_runner.h"
#include "infrastructure/json_app_settings_store.h"
#include "platform/win32/askpass_bridge.h"
#include "platform/win32/operation_result_relay.h"
#include "presentation/shell_controller.h"

#include "luil/luil.h"

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace cyella::win32 {
    struct cyella_driver_options
    {
        // 시작할 때 여는 저장소다(명령줄 인자).
        std::u8string initial_repository {};
        // 설정 파일 경로다. 비우면 설정을 메모리에만 둔다 (test, 위치를 모를 때).
        std::u8string settings_path {};
    };

    // luil logic thread에 꽂는 cyella 구동기다. 백엔드(workspace·scheduler·relay)와 설정을 조립하고, 화면은
    // `shell_controller`(상태와 화면 메시지)와 `ui::build_ui_tree`(snapshot → tree)에 맡긴다. `handle`·
    // `make_frame`은 logic thread에서만 불리고, `make_close_message`·`shutdown_completed`는 UI thread도
    // 부르므로 atomic으로 지킨다.
    //
    // 메시지의 길: worker 결과는 relay를 거쳐 `handle` 첫 줄에서 workspace에 반영된다. 백엔드 메시지
    // (`app_messages.h`)는 여기서 workspace로, 화면 메시지(`ui_intents.h`)는 controller로 간다. 그 뒤
    // controller의 `after_events`가 끝난 operation의 후속을 처리한다.
    //
    // 다른 구성 요소가 붙는 자리 (docs/ui-architecture.md):
    //  - `askpass_bridge`는 이 driver가 소유하고, `answer_input_intent`·`cancel_input_intent`는
    //    `handle_intent`의 백엔드 분기에서 처리한다.
    //  - `diff_view_controller`는 controller 옆에 두고 `make_frame`에서 `frame->webviews`를 채운다.
    class cyella_driver final : public luil::win32::logic_driver
    {
    public:
        explicit cyella_driver(std::u8string initial_repository = {});
        explicit cyella_driver(cyella_driver_options options);
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

        // 시작 전이면 null이다.
        [[nodiscard]] const shell_controller* controller() const noexcept
        {
            return controller_.get();
        }

    private:
        void drain_relay();
        // 백엔드 메시지면 처리하고 true다.
        bool handle_backend_intent(const luil::app_message& message);

        // 멤버 순서가 파괴 순서의 반대다. worker(scheduler)가 가장 먼저 멈추고, 그다음 workspace·설정을
        // 빌린 controller가 사라져야 실행기·relay를 붙잡은 것이 남지 않는다.
        cyella_driver_options options_ {};
        operation_result_relay relay_ {};
        // 서버의 연결 스레드가 질문을 relay로 보낸다. relay보다 먼저 파괴되어 스레드가 멈춘 뒤에야
        // relay가 사라진다. workspace가 서버를 가리키므로 workspace보다 나중에 파괴된다.
        askpass_bridge askpass_ { [relay = &relay_](operation_event event) { relay->post(luil::app_message { std::move(event) }); } };
        cancellation_registry registry_ {};
        std::unique_ptr<process_runner> process_runner_ {};
        std::unique_ptr<workspace_files> files_ {};
        std::unique_ptr<file_probe> probe_ {};
        std::unique_ptr<settings_file_system> settings_files_ {};
        std::unique_ptr<json_app_settings_store> settings_store_ {};
        std::unique_ptr<app_settings_service> settings_ {};
        git_tool_info git_tool_ {};
        std::unique_ptr<git_operation_runner> executor_ {};
        std::unique_ptr<git_workspace> workspace_ {};
        std::unique_ptr<shell_controller> controller_ {};
        std::unique_ptr<task_scheduler> scheduler_ {};
        // 창의 실제 크기·배율이다. 첫 frame(메시지가 오기 전)은 기본값으로 배치한다.
        window_metrics_intent metrics_ {};
        std::atomic<bool> closed_ { false };
    };
} // namespace cyella::win32
