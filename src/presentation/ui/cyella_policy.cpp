#include "presentation/ui/cyella_policy.h"

#include "presentation/commands/command_registry.h"
#include "presentation/dialog_intents.h"
#include "presentation/palette/palette_intents.h"
#include "presentation/rebase/rebase_editor_intents.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/settings/shortcuts_intents.h"
#include "presentation/settings/signing_intents.h"
#include "presentation/ui/command_elements.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/press_row.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"
#include "presentation/ui_state.h"

namespace cyella::ui {
    namespace {
        // tree에 단축키 표가 없을 때(표를 싣지 않는 test나 첫 frame 전) 쓰는 기본 표다.
        const std::vector<shortcut_binding>& default_bindings()
        {
            static const std::vector<shortcut_binding> bindings { resolve_shortcut_bindings(app_settings {}) };
            return bindings;
        }

        bool is_commit_input(const luil::ui_element_kind kind) noexcept
        {
            return kind == kind_commit_subject_input || kind == kind_commit_body_input;
        }
    } // namespace

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
        if (kind == kind_ui_font_input)
            return to_luil_target(text_target::settings_ui_font);
        if (kind == kind_code_font_input)
            return to_luil_target(text_target::settings_code_font);
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
        if (kind == kind_selector_filter_input)
            return to_luil_target(text_target::selector_filter);
        if (kind == kind_settings_signing_gpg_input)
            return to_luil_target(text_target::settings_signing_gpg_program);
        if (kind == kind_settings_signing_ssh_input)
            return to_luil_target(text_target::settings_signing_ssh_program);
        if (kind == kind_settings_signing_allowed_input)
            return to_luil_target(text_target::settings_signing_allowed_signers);
        if (kind == kind_settings_hosting_host_input)
            return to_luil_target(text_target::settings_hosting_host);
        if (kind == kind_settings_hosting_api_input)
            return to_luil_target(text_target::settings_hosting_api_base);
        if (kind == kind_settings_web_link_host_input)
            return to_luil_target(text_target::settings_web_link_host);
        if (kind == kind_settings_web_link_base_input)
            return to_luil_target(text_target::settings_web_link_base);
        if (kind == kind_settings_web_link_icon_input)
            return to_luil_target(text_target::settings_web_link_icon);
        if (kind == kind_palette_filter_input)
            return to_luil_target(text_target::palette_filter);
        if (kind == kind_shortcut_input)
            return to_luil_target(text_target::settings_shortcut_input);
        if (kind == kind_profile_name_input)
            return to_luil_target(text_target::settings_profile_name);
        if (kind == kind_profile_user_name_input)
            return to_luil_target(text_target::settings_profile_user_name);
        if (kind == kind_profile_email_input)
            return to_luil_target(text_target::settings_profile_user_email);
        if (kind == kind_profile_signing_key_input)
            return to_luil_target(text_target::settings_profile_signing_key);
        if (kind == kind_profile_rule_prefix_input)
            return to_luil_target(text_target::settings_rule_prefix);
        return std::nullopt;
    }

    std::optional<std::vector<luil::input_action>> cyella_policy::palette_key(const luil::ui_tree* const tree, const luil::key_pressed_event& event, const luil::interaction_snapshot& snapshot)
    {
        if (snapshot.focused_input.kind != kind_palette_filter_input || event.control || event.alt)
            return std::nullopt;
        if (event.key == luil::key_code::arrow_up || event.key == luil::key_code::arrow_down)
            return std::vector<luil::input_action> { message(palette_move_intent { .delta = event.key == luil::key_code::arrow_up ? -1 : 1 }) };
        if (event.key == luil::key_code::enter && tree != nullptr)
        {
            // 커서 항목의 액션은 표식에 실려 있다. 항목이 없으면 Enter는 아무 일도 하지 않는다.
            const auto* const marker { dynamic_cast<const palette_enter_element*>(tree->find(luil::ui_element_id { kind_command_marker, std::u8string { palette_enter_owner } })) };
            if (marker == nullptr || std::holds_alternative<std::monostate>(marker->action()))
                return std::vector<luil::input_action> {};
            return std::vector<luil::input_action> { marker->action() };
        }
        return std::nullopt;
    }

    std::optional<std::vector<luil::input_action>> cyella_policy::shortcut_capture_key(const luil::key_pressed_event& event, const luil::interaction_snapshot& snapshot)
    {
        if (snapshot.focused_input.kind != kind_shortcut_input)
            return std::nullopt;
        if (event.key == luil::key_code::enter && event.control == false && event.alt == false)
            return std::vector<luil::input_action> { message(apply_shortcut_intent {}) };
        const key_chord chord { chord_of(event) };
        if (chord.valid() == false || event.repeat)
            return std::nullopt;
        // 수정키 없는 글자는 문자 입력으로 와서 손으로 치는 것이다. Ctrl·Alt 조합과 기능 키만 단축키로 잡는다 (Esc는 대화 상자가 받는다).
        if (event.control || event.alt || check_key_chord(chord).empty())
            if (event.key != luil::key_code::escape)
                return std::vector<luil::input_action> { message(capture_shortcut_intent { chord }) };
        return std::nullopt;
    }

    std::optional<std::vector<luil::input_action>> cyella_policy::registered_shortcut(const luil::ui_tree* const tree, const luil::key_pressed_event& event, const luil::interaction_snapshot& snapshot)
    {
        const key_chord chord { chord_of(event) };
        if (chord.valid() == false || event.repeat)
            return std::nullopt;
        const shortcut_table_element* const table {
            tree != nullptr ? dynamic_cast<const shortcut_table_element*>(tree->find(luil::ui_element_id { kind_command_marker, std::u8string { shortcut_table_owner } })) : nullptr
        };
        const std::vector<shortcut_binding>& bindings { table != nullptr ? table->bindings() : default_bindings() };
        for (const shortcut_binding& binding : bindings)
        {
            if (binding.chord != chord)
                continue;
            // 커밋 상자에 초점이 있을 때만 듣는 명령이다. 다른 곳에서는 같은 키가 다른 뜻(없으면 아무 일도 안 함)이다.
            if (binding.scope == command_scope::commit_input && is_commit_input(snapshot.focused_input.kind) == false)
                continue;
            return std::vector<luil::input_action> { action_for_command(binding.command) };
        }
        return std::nullopt;
    }

    std::optional<std::vector<luil::input_action>> cyella_policy::rebase_editor_key(const luil::ui_tree* tree, const luil::key_pressed_event& event, const luil::interaction_snapshot& snapshot)
    {
        const luil::ui_element_kind input { snapshot.focused_input.kind };
        // 본문은 여러 줄 칸이다. Enter는 줄바꿈이고 ↑↓는 윗줄·아랫줄로 간다 (Ctrl+Enter는 그대로 앱의 것이다).
        if (input == kind_rebase_body_input || input == kind_commit_body_input)
        {
            if (event.key == luil::key_code::enter && event.control == false && event.alt == false)
            {
                luil::text_edit_request request {};
                request.target = to_luil_target(input == kind_commit_body_input ? text_target::commit_body : text_target::rebase_message_body);
                request.command = luil::text::text_edit_command::insert;
                request.text = u8"\n";
                return std::vector<luil::input_action> { message(edit_text_intent { std::move(request) }) };
            }
            if ((event.key == luil::key_code::arrow_up || event.key == luil::key_code::arrow_down) && event.control == false && event.alt == false)
            {
                if (input == kind_commit_body_input)
                    return std::vector<luil::input_action> { message(commit_body_line_intent { .down = event.key == luil::key_code::arrow_down, .extend = event.shift }) };
                return std::vector<luil::input_action> { message(rebase_body_line_intent { .down = event.key == luil::key_code::arrow_down, .extend = event.shift }) };
            }
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
        // 명령 팔레트의 거르는 칸이다. ↑↓로 커서를 옮기고 Enter로 고른다.
        if (std::optional<std::vector<luil::input_action>> palette { palette_key(tree, event, snapshot) }; palette.has_value())
            return std::move(*palette);
        // 환경 설정의 단축키 입력 칸이다. 눌린 키 조합을 칸에 적는다 (이미 쓰이는 키도 일단 받고, 적용 때 충돌을 알린다).
        if (std::optional<std::vector<luil::input_action>> capture { shortcut_capture_key(event, snapshot) }; capture.has_value())
            return std::move(*capture);
        // 단축키는 명령 레지스트리가 정한다 (Ctrl+Enter 커밋, Ctrl+Tab·Ctrl+W 탭, Shift+F10 메뉴, F5 새로 고침, Esc 닫기, Ctrl+Shift+P 팔레트 …).
        // 표는 frame마다 tree에 실려 오고, 설정에서 바꾼 키가 여기서 곧바로 쓰인다.
        if (std::optional<std::vector<luil::input_action>> registered { registered_shortcut(tree, event, snapshot) }; registered.has_value())
            return std::move(*registered);
        switch (event.key)
        {
        case luil::key_code::enter:
            if (focused == kind_ui_font_input || focused == kind_code_font_input)
                return { message(apply_font_settings_intent {}) };
            if (focused == kind_clone_url_input || focused == kind_clone_path_input)
                return { message(submit_clone_intent {}) };
            if (focused == kind_settings_git_path_input)
                return { message(apply_git_path_intent {}) };
            if (focused == kind_settings_test_host_input)
                return { message(test_ssh_connection_intent {}) };
            if (focused == kind_settings_signing_gpg_input)
                return { message(apply_signing_text_intent { signing_key_id::gpg_program }) };
            if (focused == kind_settings_signing_ssh_input)
                return { message(apply_signing_text_intent { signing_key_id::gpg_ssh_program }) };
            if (focused == kind_settings_signing_allowed_input)
                return { message(apply_signing_text_intent { signing_key_id::gpg_ssh_allowed_signers }) };
            if (focused == kind_settings_hosting_host_input || focused == kind_settings_hosting_api_input)
                return { message(add_hosting_host_intent {}) };
            if (focused == kind_settings_web_link_host_input || focused == kind_settings_web_link_base_input || focused == kind_settings_web_link_icon_input)
                return { message(add_web_link_intent {}) };
            if (focused == kind_form_input_0 || focused == kind_form_input_1 || focused == kind_form_input_2)
                return { message(form_enter_intent {}) };
            return {};
        default:
            break;
        }
        return {};
    }

    std::vector<luil::input_action> cyella_policy::on_press(const luil::ui_tree&, const luil::ui_element& element, const luil::pointer_pressed_event& event)
    {
        const auto* const row { dynamic_cast<const press_row_element*>(&element) };
        if (row == nullptr)
            return {};
        return row->press(luil::ui_action_context { .element = row->id(), .x = event.x, .y = event.y, .control = event.control, .shift = event.shift, .alt = event.alt });
    }
} // namespace cyella::ui
