#include "presentation/ui/cyella_policy.h"

#include "presentation/dialog_intents.h"
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

    std::vector<luil::input_action> cyella_policy::on_key(const luil::ui_tree*, const luil::key_pressed_event& event, const luil::interaction_snapshot& snapshot)
    {
        const luil::ui_element_kind focused { snapshot.focused_input.kind };
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
