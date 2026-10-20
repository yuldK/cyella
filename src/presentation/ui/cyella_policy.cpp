#include "presentation/ui/cyella_policy.h"

#include "presentation/dialog_intents.h"
#include "presentation/rebase/rebase_editor_intents.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"
#include "presentation/ui_state.h"

namespace cyella::ui {
    std::optional<luil::text_input_target> cyella_policy::text_target_of(const luil::ui_element_kind kind) const
    {
        // 새 텍스트 칸은 이 표에만 더하면 초점·caret·문자·클립보드·IME 배선이 함께 따라온다.
        if (kind == kind_start_search_input)
            return to_luil_target(text_target::start_search);
        if (kind == kind_sidebar_filter_input)
            return to_luil_target(text_target::sidebar_filter);
        if (kind == kind_commit_subject_input)
            return to_luil_target(text_target::commit_subject);
        if (kind == kind_commit_body_input)
            return to_luil_target(text_target::commit_body);
        if (kind == kind_clone_url_input)
            return to_luil_target(text_target::clone_url);
        if (kind == kind_clone_path_input)
            return to_luil_target(text_target::clone_path);
        if (kind == kind_settings_git_path_input)
            return to_luil_target(text_target::settings_git_path);
        if (kind == kind_settings_tool_extensions_input)
            return to_luil_target(text_target::settings_tool_extensions);
        if (kind == kind_settings_tool_executable_input)
            return to_luil_target(text_target::settings_tool_executable);
        if (kind == kind_settings_tool_arguments_input)
            return to_luil_target(text_target::settings_tool_arguments);
        if (kind == kind_settings_key_name_input)
            return to_luil_target(text_target::settings_key_name);
        if (kind == kind_settings_key_comment_input)
            return to_luil_target(text_target::settings_key_comment);
        if (kind == kind_settings_test_host_input)
            return to_luil_target(text_target::settings_test_host);
        if (kind == kind_form_input_0)
            return to_luil_target(text_target::form_field_0);
        if (kind == kind_form_input_1)
            return to_luil_target(text_target::form_field_1);
        if (kind == kind_form_input_2)
            return to_luil_target(text_target::form_field_2);
        if (kind == kind_rebase_subject_input)
            return to_luil_target(text_target::rebase_message_subject);
        if (kind == kind_rebase_body_input)
            return to_luil_target(text_target::rebase_message_body);
        return std::nullopt;
    }

    std::optional<std::vector<luil::input_action>> cyella_policy::rebase_editor_key(const luil::ui_tree* tree, const luil::key_pressed_event& event, const luil::interaction_snapshot& snapshot)
    {
        const luil::ui_element_kind input { snapshot.focused_input.kind };
        // 본문은 여러 줄 칸이다. Enter는 줄바꿈이고 ↑↓는 윗줄·아랫줄로 간다 (Ctrl+Enter는 그대로 앱의 것이다).
        if (input == kind_rebase_body_input)
        {
            if (event.key == luil::key_code::enter && event.control == false && event.alt == false)
            {
                luil::text_edit_request request {};
                request.target = to_luil_target(text_target::rebase_message_body);
                request.command = luil::text::text_edit_command::insert;
                request.text = u8"\n";
                return std::vector<luil::input_action> { message(edit_text_intent { std::move(request) }) };
            }
            if ((event.key == luil::key_code::arrow_up || event.key == luil::key_code::arrow_down) && event.control == false && event.alt == false)
                return std::vector<luil::input_action> { message(rebase_body_line_intent { .down = event.key == luil::key_code::arrow_down, .extend = event.shift }) };
            return std::nullopt;
        }
        if (snapshot.focused_input != luil::ui_element_id {} || tree == nullptr)
            return std::nullopt;
        // 편집기가 떠 있을 때만이다. 목록이 tree에 있는지로 안다 (정책은 상태를 두지 않는다).
        if (tree->find(luil::ui_element_id { luil::ui_element_kind::virtual_list, u8"rebase-todo" }) == nullptr)
            return std::nullopt;
        // Alt+↑↓는 고른 줄을 옮긴다. 목록의 커서가 곧 고른 줄이다. p·r·s·f·d는 Windows가 키가 아니라 문자로만 보내므로 목록의
        // 글자 탐색 자리(`rebase_editor_ui.cpp`)가 받는다.
        if (event.alt && event.control == false && (event.key == luil::key_code::arrow_up || event.key == luil::key_code::arrow_down))
            return std::vector<luil::input_action> { message(rebase_move_intent { .row_key = {}, .delta = event.key == luil::key_code::arrow_up ? -1 : 1 }) };
        return std::nullopt;
    }

    std::optional<luil::menu_kinds> cyella_policy::menu() const
    {
        return luil::menu_kinds { .container = luil::ui_element_kind::menu, .item = luil::ui_element_kind::menu_item };
    }

    std::vector<luil::input_action> cyella_policy::close_menu() const
    {
        return { message(close_menu_intent {}) };
    }

    luil::input_action cyella_policy::make_text_edit_action(const luil::text_edit_request& request) const
    {
        return message(edit_text_intent { request });
    }

    luil::input_action cyella_policy::make_text_composition_action(const luil::text_composition_event& event) const
    {
        return message(compose_text_intent { event });
    }

    std::vector<luil::input_action> cyella_policy::on_wheel(const luil::ui_tree& tree, const luil::mouse_wheel_event& event, const float scroll_delta)
    {
        return luil::route_wheel(tree, event, scroll_delta);
    }

    std::vector<luil::input_action> cyella_policy::on_focus_moved(const luil::ui_tree& tree, const luil::ui_element_id& focused)
    {
        return luil::route_reveal(tree, focused);
    }

    std::vector<luil::input_action> cyella_policy::on_key(const luil::ui_tree* tree, const luil::key_pressed_event& event, const luil::interaction_snapshot& snapshot)
    {
        const luil::ui_element_kind focused { snapshot.focused_input.kind };
        // 인터랙티브 rebase 편집기의 키다. 본문 칸의 Enter·↑↓와, 텍스트 칸 밖의 Alt+↑↓(고른 줄 옮기기)다.
        if (std::optional<std::vector<luil::input_action>> rebase { rebase_editor_key(tree, event, snapshot) }; rebase.has_value())
            return std::move(*rebase);
        switch (event.key)
        {
        case luil::key_code::enter:
            // Ctrl+Enter는 커밋 상자 어디서든 커밋이다.
            if (event.control && (focused == kind_commit_subject_input || focused == kind_commit_body_input))
                return { message(commit_intent {}) };
            if (focused == kind_clone_url_input || focused == kind_clone_path_input)
                return { message(submit_clone_intent {}) };
            if (focused == kind_settings_git_path_input)
                return { message(apply_git_path_intent {}) };
            if (focused == kind_settings_test_host_input)
                return { message(test_ssh_connection_intent {}) };
            if (focused == kind_form_input_0 || focused == kind_form_input_1 || focused == kind_form_input_2)
                return { message(form_enter_intent {}) };
            return {};
        case luil::key_code::tab:
            if (event.control && event.repeat == false)
                return { message(cycle_tab_intent { event.shift == false }) };
            return {};
        case luil::key_code::key_w:
            if (event.control && event.repeat == false)
                return { message(close_selected_tab_intent {}) };
            return {};
        case luil::key_code::f10:
            // Shift+F10은 고른 항목의 우클릭 메뉴다 (마우스 없이 메뉴를 여는 길).
            if (event.shift && event.repeat == false)
                return { message(open_menu_for_selection_intent {}) };
            return {};
        case luil::key_code::f5:
            if (event.repeat == false)
                return { message(refresh_intent {}) };
            return {};
        case luil::key_code::escape:
            // 열린 대화 상자의 Esc는 luil `modal_host_element`가 먼저 받는다. 여기 오는 것은 서랍·떠 있는 판·diff다.
            return { message(close_overlays_intent {}) };
        default:
            break;
        }
        return {};
    }
} // namespace cyella::ui
