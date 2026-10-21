#pragma once

#include "application/app_messages.h"
#include "application/cancellation_registry.h"
#include "application/diff_view_controller.h"
#include "application/external_diff_opener.h"
#include "application/operation_events.h"
#include "application/operation_executor.h"
#include "domain/external_diff_tool.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 웹뷰가 보여야 하는 diff다. 선택된 탭이 연 diff 요청과 그 저장소다.
    struct diff_target
    {
        std::string repository_id {};
        std::u8string repository_path {};
        diff_file_request request {};
    };

    // 화면 안내 한 줄로 올릴 결과다 (외부 도구를 열지 못했다, 적용에 실패했다).
    struct diff_notice
    {
        std::u8string text {};
        bool warning { false };
    };

    // 코디네이터가 바깥 세계에 닿는 길이다. 앱에서는 workspace·scheduler·relay·클립보드가 꽂히고, test에서는
    // 낸 것을 세는 가짜가 꽂힌다.
    struct diff_view_ports
    {
        // 저장소 operation을 낸다 (`git_workspace::submit`). 0이면 받지 못했다.
        std::function<operation_id(const std::string& repository_id, git_operation operation)> submit {};
        // worker에 일을 올린다 (`task_scheduler::submit`). 접수하지 못하면 false다.
        std::function<bool(operation_request request)> schedule {};
        // 클립보드에 쓴다.
        std::function<void(std::string text)> copy_text {};
        // worker의 결과를 logic thread로 돌려보낸다 (relay). 어느 스레드에서나 불린다.
        std::function<void(diff_sides_ready_intent message)> sides_ready {};
        std::function<void(external_open_done_intent message)> external_done {};
        // 지금 설정의 확장자별 외부 도구 표다.
        std::function<std::vector<external_diff_tool>()> tools {};
        // 앱을 닫을 때 진행 중인 일을 함께 취소하려고 취소 source를 맡긴다. 없어도 된다.
        cancellation_registry* registry { nullptr };
        // 아래 둘은 worker가 쓰므로 scheduler보다 오래 살아야 한다.
        diff_git_gateway* gateway { nullptr };
        external_diff_opener* opener { nullptr };
        std::u8string git_executable {};
    };

    // `diff_view_controller`가 값으로 돌려준 일(`diff_view_effect`)을 실제 operation·worker 일·클립보드로 바꾸는
    // 조립부다. 컨트롤러는 입출력을 하지 않으므로, 웹뷰를 프레임에 끼우는 쪽(cyella_driver)이 이것 하나만 부르면 된다.
    //
    //  - hunk·줄 stage/unstage/discard → `apply_patch_request`를 보고 있는 diff의 저장소에 낸다. 끝나면 같은 파일의
    //    `diff_file`을 다시 읽고, 다시 읽은 결과가 오면 `sync`가 다시 보인다. 실패하면 `action_failed`와 안내다.
    //  - 이미지·바이너리의 두 쪽 꺼내기와 외부 도구 열기 → scheduler의 한 줄(lane)에서 취소 token과 함께 돈다.
    //    다른 파일로 넘어가면 앞의 일을 취소한다.
    //  - 복사 → 클립보드 port.
    //
    // logic thread에서만 쓴다. worker는 port의 콜백으로만 돌아온다.
    class diff_view_coordinator
    {
    public:
        explicit diff_view_coordinator(diff_view_ports ports, diff_view_options options = {});
        diff_view_coordinator(const diff_view_coordinator&) = delete;
        diff_view_coordinator& operator=(const diff_view_coordinator&) = delete;
        ~diff_view_coordinator();

        [[nodiscard]] diff_view_controller& controller() noexcept
        {
            return controller_;
        }

        [[nodiscard]] const diff_view_controller& controller() const noexcept
        {
            return controller_;
        }

        // frame마다 부른다. 웹뷰가 보여야 할 diff(`wanted`)와 그 저장소 세션이 마지막으로 읽은 diff(`loaded`)를 맞춘다.
        //  - `loaded`가 `wanted`의 요청과 같으면 보인다. 같은 요청을 다시 읽어 왔으면(`serial`이 올랐으면) 다시 보인다.
        //  - 아직 읽는 중이면 낡은 내용을 지워 둔다 (stage 단추가 엉뚱한 파일에 걸리지 않게).
        //  - `wanted`가 없으면 비운다.
        void sync(const std::optional<diff_target>& wanted, const file_diff* loaded, std::uint64_t serial);

        // 웹뷰가 보낸 JSON이다. 받아들였으면 그 동작이 낳은 일까지 실행한다.
        web_message_result handle_web_message(std::string_view message);

        // 웹뷰 페이지가 다시 읽힌다 (항해 시작).
        void page_reloading() noexcept;

        // worker의 결과다. 두 쪽이 준비됐다.
        void sides_prepared(const diff_sides_ready_intent& message);
        // 외부 도구 열기가 끝났다. 알릴 것이 있으면 안내가 쌓인다.
        void external_opened(const external_open_done_intent& message);
        void open_file_app(external_file_app app);
        // operation이 끝났다. 패치 적용이나 그 뒤의 diff 다시 읽기면 이어서 처리한다.
        void on_operation_finished(operation_id id, const operation_finished& finished);

        // 쌓인 안내를 하나 꺼낸다.
        [[nodiscard]] std::optional<diff_notice> take_notice();

        // 지금 보이는 diff다. 없으면 비어 있다.
        [[nodiscard]] const std::optional<diff_target>& shown() const noexcept
        {
            return shown_target_;
        }

        // 패치를 적용하는 operation과 그 뒤 다시 읽는 operation이 진행 중이다.
        [[nodiscard]] bool busy() const noexcept
        {
            return pending_apply_ != 0 || pending_reload_ != 0;
        }

    private:
        void run_effects(const std::vector<diff_view_effect>& effects);
        void run_apply(const apply_patch_effect& effect);
        void run_prepare_sides(const prepare_sides_effect& effect);
        void run_open_external(const open_external_effect& effect, std::optional<external_file_app> app = std::nullopt);
        void forget_shown();
        void notice(std::u8string text, bool warning);
        [[nodiscard]] std::shared_ptr<process_cancellation_source> replace_source(std::shared_ptr<process_cancellation_source>& slot);

        diff_view_ports ports_ {};
        diff_view_controller controller_;

        std::optional<diff_target> shown_target_ {};
        std::uint64_t shown_serial_ { 0 };
        operation_id pending_apply_ { 0 };
        operation_id pending_reload_ { 0 };
        std::shared_ptr<process_cancellation_source> sides_source_ {};
        std::shared_ptr<process_cancellation_source> external_source_ {};
        std::vector<diff_notice> notices_ {};
    };

    // "외부 도구로 열기" 결과를 사용자에게 보일 한 줄로 바꾼다. 도구를 띄웠으면 짧은 확인이다.
    [[nodiscard]] diff_notice describe_external_open(const external_open_result& result);
} // namespace cyella
