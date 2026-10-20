#pragma once

#include "application/app_messages.h"
#include "application/app_settings_service.h"
#include "application/cancellation_registry.h"
#include "application/diff_tool_launcher.h"
#include "application/diff_view_coordinator.h"
#include "application/external_diff_opener.h"
#include "application/file_probe.h"
#include "application/git_workspace.h"
#include "application/process_runner.h"
#include "application/repository_watcher.h"
#include "application/settings_file_system.h"
#include "application/task_scheduler.h"
#include "application/watch_messages.h"
#include "application/watch_refresh_policy.h"
#include "application/workspace_files.h"
#include "domain/git_tool.h"
#include "infrastructure/diff_temp_directory.h"
#include "infrastructure/git_diff_gateway.h"
#include "infrastructure/git_operation_runner.h"
#include "infrastructure/json_app_settings_store.h"
#include "infrastructure/ssh_info.h"
#include "platform/win32/askpass_bridge.h"
#include "platform/win32/operation_result_relay.h"
#include "presentation/shell_controller.h"

#include "luil/luil.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace cyella::win32 {
    struct cyella_driver_options
    {
        // 시작할 때 여는 저장소다(명령줄 인자).
        std::u8string initial_repository {};
        // 설정 파일 경로다. 비우면 설정을 메모리에만 둔다 (test, 위치를 모를 때).
        std::u8string settings_path {};
        // diff를 웹뷰 없이 텍스트 대체 보기로만 보인다 (WebView2 Runtime이 없는 PC의 화면을 검증할 때 쓴다).
        bool force_text_diff { false };
        // SSH 키를 읽고 만드는 폴더다. 비우면 `%USERPROFILE%\.ssh`다 (검증할 때 사용자의 키를 건드리지 않으려고 바꾼다).
        std::u8string ssh_directory {};
        // 저장소 파일 감시를 만드는 공장이다. 비우면 Win32 구현(`ReadDirectoryChangesW`)을 쓴다 (test가 가짜를 꽂는다).
        std::shared_ptr<repository_watcher_factory> watcher_factory {};
        // 파일 감시를 쓰지 않는다 (감시가 안 되는 환경을 검증할 때 쓴다). 창 초점으로만 다시 읽는다.
        bool disable_file_watching { false };
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
        [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> next_tick() override;
        void tick(std::chrono::steady_clock::time_point now) override;

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

        // delegate의 `on_started`가 host를 묶는다. 클립보드 요청이 host를 거친다.
        void bind_host(luil::app_host* host) noexcept
        {
            host_.store(host);
        }

        // diff 웹뷰를 쥔 조립부다. git을 쓸 수 없으면 null이다.
        [[nodiscard]] const diff_view_coordinator* diff_view() const noexcept
        {
            return diff_.get();
        }
        // 웹뷰로 diff를 그릴 수 있다 (Runtime·뷰어 파일이 있고, 웹뷰가 실패하지 않았다).
        [[nodiscard]] bool webview_usable() const noexcept
        {
            return webview_capable_ && webview_failed_ == false;
        }

    private:
        void drain_relay();
        // 백엔드 메시지면 처리하고 true다.
        bool handle_backend_intent(const luil::app_message& message);
        // diff 웹뷰의 머리 막대·웹뷰 사건이면 처리하고 true다.
        bool handle_diff_view_intent(const luil::app_message& message);
        void handle_webview_event(const luil::webview_event& event);
        void start_diff_view();
        void copy_to_clipboard(const std::string& text);

        // ---- 파일 감시와 창 초점
        // 열린 저장소와 감시를 맞춘다: 새로 열린 저장소에 감시를 켜고, 닫힌 저장소의 감시를 멈춘다. 메시지를 처리한 뒤마다 부른다.
        void sync_watchers();
        void start_watcher(const std::string& repository_id);
        void on_repository_changed(const repository_changed_intent& changed);
        void on_window_focus();
        // 감시를 쓸 수 없게 된 저장소다 (시작 실패, 폴더가 사라짐). 감시를 접고 한 번 알린 뒤 창 초점에서 다시 읽는다.
        void give_up_watching(const std::string& repository_id, bool lost);
        void run_refresh(const std::string& repository_id, refresh_scope scope);
        // 끝난 operation의 저장소에서 보류해 둔 새로 고침을 낸다.
        void flush_pending_refreshes();
        // 환경 설정 SSH 화면의 agent·키 목록을 worker에게 읽게 한다.
        void request_ssh_info();
        // 웹뷰가 보여야 할 diff를 맞추고, 쌓인 안내를 화면에 올린다.
        void sync_diff_view();

        // 멤버 순서가 파괴 순서의 반대다. worker(scheduler)가 가장 먼저 멈추고, 그다음 workspace·설정을
        // 빌린 controller가 사라져야 실행기·relay를 붙잡은 것이 남지 않는다.
        cyella_driver_options options_ {};
        operation_result_relay relay_ {};
        // 감시 스레드가 relay로 알린다. 멤버는 선언의 반대 순서로 파괴되므로 relay_ 뒤에 선언해야 감시가 먼저 멈춘 뒤 relay가 사라진다.
        std::shared_ptr<repository_watcher_factory> watch_factory_ {};
        std::map<std::string, std::unique_ptr<repository_watcher>> watchers_ {};
        // 감시를 시작하지 못했거나 포기한 저장소다. 같은 저장소를 열어 둔 동안 다시 시도하지 않는다.
        std::set<std::string> unwatched_ {};
        // operation이 끝난 저장소다. 한 번 비운 relay 묶음 뒤에 보류한 새로 고침을 내는 데 쓴다.
        std::set<std::string> finished_repositories_ {};
        watch_refresh_policy refresh_policy_ {};
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
        // diff 뷰어: 두 쪽 꺼내기·외부 도구 열기는 scheduler worker가 쓰므로 scheduler 뒤에 둔다 (stop_workers가 먼저 join한다).
        std::unique_ptr<diff_temp_directory> diff_temp_ {};
        std::unique_ptr<git_diff_gateway> diff_gateway_ {};
        std::unique_ptr<diff_tool_launcher> diff_launcher_ {};
        std::unique_ptr<external_diff_opener> diff_opener_ {};
        std::unique_ptr<diff_view_coordinator> diff_ {};
        // 저장소마다 `file_diff_loaded`가 온 횟수다. 같은 요청을 다시 읽어 왔는지(stage 뒤) 알아내는 데 쓴다.
        std::map<std::string, std::uint64_t> diff_serials_ {};
        std::atomic<luil::app_host*> host_ { nullptr };
        ssh_programs ssh_programs_ {};
        std::filesystem::path ssh_directory_ {};
        // 웹뷰에 보낸 테마와 그 원천(해석한 luil 테마)이다.
        std::optional<luil::color_theme> diff_theme_source_ {};
        diff_theme diff_theme_ {};
        // 환경 설정의 기본 diff 보기를 뷰어에 마지막으로 보낸 값이다.
        std::optional<diff_layout_preference> applied_diff_layout_ {};
        std::u8string viewer_url_ {};
        std::u8string webview_data_folder_ {};
        // 웹뷰를 세울 수 있는 PC인가 (Runtime 설치, 뷰어 파일 있음, 프로필 폴더를 앎).
        bool webview_capable_ { false };
        // 웹뷰가 실패했다 (항해 실패·브라우저 프로세스 종료). 이후 대체 보기를 쓴다.
        bool webview_failed_ { false };
        // 한 번 diff를 연 뒤로는 웹뷰를 frame에 계속 실어 둔다. 웹뷰를 다시 만들지 않고 재사용하려는 것이다.
        bool webview_requested_ { false };
        std::uint64_t posted_revision_ { 0 };
        std::u8string posted_message_ {};
        // 창의 실제 크기·배율이다. 첫 frame(메시지가 오기 전)은 기본값으로 배치한다.
        window_metrics_intent metrics_ {};
        std::atomic<bool> closed_ { false };
    };
} // namespace cyella::win32
