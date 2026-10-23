#pragma once

#include "application/diff_sides.h"
#include "application/diff_view_protocol.h"
#include "application/git_operation.h"
#include "application/operation_events.h"
#include "domain/diff_file.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cyella {
    // 컨트롤러는 입출력을 하지 않는다. 웹뷰에서 온 동작이 낳는 일은 값으로 돌려주고 호출자가
    // 실행한다. git과 파일을 만지는 일이 logic thread를 막지 않게 하려는 것이다.

    // 만든 patch를 `git apply`로 적용한다. workspace에 `apply_patch_request`로 낸다. 끝나면 호출자가
    // diff를 다시 읽어 `show`로 넘기거나, 실패하면 `action_failed`를 부른다.
    struct apply_patch_effect
    {
        apply_patch_request request {};
        diff_hunk_command command { diff_hunk_command::stage };
        std::size_t hunk { 0 };
        // hunk 전체를 골랐는가(줄 선택이 아니라).
        bool whole_hunk { true };
    };

    // 이미지 비교나 바이너리 카드에 필요한 두 쪽을 꺼낸다. worker thread에서 `diff_git_gateway::extract`를
    // 하고, 결과를 `sides_prepared(ticket, ...)`로 돌려준다.
    struct prepare_sides_effect
    {
        std::uint64_t ticket { 0 };
        diff_side_plan plan {};
        // 이미지는 임시 파일로 꺼내야 하고, 바이너리 카드는 크기만 구하면 된다.
        bool write_files { false };
    };

    // "외부 도구로 열기"다. worker thread에서 `external_diff_opener::open`을 부른다.
    struct open_external_effect
    {
        diff_side_plan plan {};
    };

    // 클립보드에 쓸 텍스트다.
    struct copy_text_effect
    {
        std::string text {};
    };

    using diff_view_effect = std::variant<apply_patch_effect, prepare_sides_effect, open_external_effect, copy_text_effect>;

    struct web_message_result
    {
        // 컨트롤러가 받아들였는가. 상태를 바꿨거나 일을 낳았으면 참이다.
        bool accepted { false };
        // 거절한 까닭이다. 로그용이다.
        std::string reason {};
        std::vector<diff_view_effect> effects {};
    };

    struct diff_view_options
    {
        // 한 번에 웹뷰로 보내는 diff의 줄 수 상한이다. 0이면 자르지 않는다.
        std::size_t line_limit { diff_view_default_line_limit };
        // 뷰어 안의 머리 줄을 보인다.
        bool toolbar { true };
    };

    // diff 뷰어 하나의 상태다. 보고 있는 diff, 화면 상태(layout·theme), 웹뷰로 보낼 다음 메시지를 가진다.
    // logic thread 하나에서만 쓴다.
    //
    // 웹뷰로 가는 길은 `ui_webview::post_revision`·`post_message` 하나다. frame마다 마지막 메시지만 적용되므로
    // `poll()`을 frame마다 한 번 불러, 바뀐 것이 있으면 **하나의 메시지**로 묶는다.
    //  - 내용(diff·이미지·바이너리·비움)이 바뀌었으면 내용 메시지가 layout·theme까지 싣는다.
    //  - 내용은 그대로이고 layout·theme·이미지 모드 중 하나만 바뀌었으면 작은 `set_*` 메시지다.
    //  - 둘 이상이 한꺼번에 바뀌었으면 내용 메시지를 다시 보낸다.
    //  - 페이지가 `ready`를 보내기 전에는 아무것도 보내지 않는다. 페이지는 `ready`가 오면 현재 상태 전체를
    //    다시 받는다(웹뷰가 다시 읽혔을 때도 같다).
    class diff_view_controller
    {
    public:
        explicit diff_view_controller(diff_view_options options = {});

        // ------------------------------------------------------------------ 화면 상태

        void set_layout(diff_layout layout);
        void set_theme(diff_theme theme);
        void set_image_mode(image_compare_mode mode);
        [[nodiscard]] const diff_view_state& state() const noexcept
        {
            return state_;
        }

        // ------------------------------------------------------------------ 내용

        // workspace가 읽은 diff 결과를 보인다. 같은 파일이 다시 오면(stage 뒤 새로 읽음) 뷰어가 스크롤을 유지한다.
        // 바이너리 파일이면 두 쪽을 꺼내는 일을 돌려준다.
        [[nodiscard]] std::vector<diff_view_effect> show(const file_diff& diff, std::u8string repository_path);

        // 보일 diff가 없다.
        void clear();

        // `prepare_sides_effect`의 결과다. 그 사이에 다른 파일로 넘어갔으면(ticket이 다르면) 버린다.
        void sides_prepared(std::uint64_t ticket, const extracted_sides& sides);

        // `apply_patch_effect`가 실패했다. 다음 동작을 다시 받는다.
        void action_failed();

        // ------------------------------------------------------------------ 웹뷰

        // 웹뷰가 보낸 JSON이다(`webview_event_kind::web_message_received`의 message).
        [[nodiscard]] web_message_result handle_web_message(std::string_view message);

        // frame마다 한 번 부른다. 새 메시지를 만들었으면 참이다.
        bool poll();

        // 마지막으로 만든 메시지와 그 번호다. `ui_webview::post_message`·`post_revision`에 그대로 싣는다.
        [[nodiscard]] std::uint64_t post_revision() const noexcept
        {
            return post_revision_;
        }
        [[nodiscard]] const std::string& latest_message() const noexcept
        {
            return latest_message_;
        }

        // 웹뷰 페이지가 다시 읽힐 때 부른다(항해 시작). 새 `ready`가 올 때까지 보내지 않는다.
        void page_reloading() noexcept;

        // ------------------------------------------------------------------ 조회

        [[nodiscard]] bool page_ready() const noexcept
        {
            return page_ready_;
        }
        // 마지막으로 보낸 내용 메시지의 seq다. 동작이 이 값을 되돌려 보내야 받아들인다.
        [[nodiscard]] std::uint64_t content_seq() const noexcept
        {
            return content_seq_;
        }
        [[nodiscard]] diff_content content() const noexcept;
        [[nodiscard]] bool truncated() const noexcept;
        [[nodiscard]] bool can_stage() const noexcept;
        [[nodiscard]] bool can_unstage() const noexcept;
        [[nodiscard]] bool can_discard() const noexcept;
        // 지금 보는 파일에 textconv가 걸려 있으면 그 diff 드라이버 이름이다. 아니면 빈 값이다.
        [[nodiscard]] const std::string& textconv_driver() const noexcept
        {
            return textconv_driver_;
        }

        [[nodiscard]] diff_side_plan make_plan() const;

    private:
        enum class content_kind
        {
            empty,
            text,
            // 이미지·바이너리의 두 쪽을 꺼내는 중이다. 그동안 뷰어는 비운다.
            pending_image,
            pending_binary,
            image,
            binary,
            special,
        };

        [[nodiscard]] std::string build_content_message();
        [[nodiscard]] bool has_actionable_content() const noexcept;
        [[nodiscard]] web_message_result reject(std::string reason) const;
        [[nodiscard]] web_message_result handle_hunk_action(const hunk_action& action);
        [[nodiscard]] web_message_result handle_lines_action(const lines_action& action);
        [[nodiscard]] web_message_result build_patch_effect(diff_hunk_command command, std::size_t hunk, std::optional<std::vector<std::size_t>> lines);
        [[nodiscard]] bool command_allowed(diff_hunk_command command) const noexcept;

        diff_view_options options_ {};
        diff_view_state state_ {};

        content_kind kind_ { content_kind::empty };
        diff_file file_ {};
        diff_view view_ { diff_view::unstaged };
        std::string sha_ {};
        // range 보기의 왼쪽 리비전이다.
        std::string base_ {};
        bool untracked_ { false };
        std::uint32_t context_lines_ { 3 };
        // textconv가 걸린 파일이다. 보이는 diff가 실제 파일 내용과 맞지 않아 hunk·줄 동작을 막는다.
        std::string textconv_driver_ {};
        std::u8string repository_path_ {};
        extracted_sides sides_ {};
        // 줄 수 상한을 풀고 전체를 보낸다.
        bool full_requested_ { false };
        // 패치를 적용하러 보냈고 아직 diff를 다시 받지 못했다. 같은 hunk를 두 번 내지 않게 막는다.
        bool awaiting_refresh_ { false };
        std::uint64_t ticket_ { 0 };

        bool page_ready_ { false };
        bool content_dirty_ { true };
        bool layout_dirty_ { false };
        bool theme_dirty_ { false };
        bool image_mode_dirty_ { false };

        std::uint64_t next_seq_ { 0 };
        std::uint64_t content_seq_ { 0 };
        std::uint64_t post_revision_ { 0 };
        std::string latest_message_ {};
    };
} // namespace cyella
