#include "presentation/settings/shortcuts_page.h"

#include "presentation/ui_state.h"

#include <utility>

namespace cyella {
    shortcuts_page_view build_shortcuts_page_view(const app_settings& settings, const std::optional<shortcut_edit_state>& edit, const std::optional<luil::text_composition_event>& composition)
    {
        shortcuts_page_view view {};
        // 묶음은 레지스트리의 순서(앱 → 탭 → 저장소 → Git → 화면 → 프로필)다. 키로만 쓰는 명령(Esc로 닫기)도 바꿀 수 있어 모두 보인다.
        for (const command_definition& command : command_definitions())
        {
            const std::u8string title_of_group { command_group_title(command.group) };
            if (view.groups.empty() || view.groups.back().title != title_of_group)
                view.groups.push_back(shortcut_group_view { .title = title_of_group, .rows = {} });
            shortcut_row_view row {};
            row.command = command.id;
            row.title = command.title;
            row.keys = shortcut_text(settings, command.id);
            row.customized = shortcuts_customized(settings, command.id);
            row.editing = edit.has_value() && edit->command == command.id;
            view.any_customized = view.any_customized || row.customized;
            view.groups.back().rows.push_back(std::move(row));
        }
        if (edit.has_value())
            if (const command_definition* const command { find_command(edit->command) }; command != nullptr)
            {
                shortcut_edit_view editing {};
                editing.command = command->id;
                editing.title = command->title;
                editing.input = luil::make_text_input_view(edit->text, composition, to_luil_target(text_target::settings_shortcut_input));
                editing.default_keys = format_key_chord_list(default_shortcuts(*command));
                editing.error = check_shortcut_edit(settings, command->id, edit->text.text).error;
                editing.can_apply = editing.error.empty();
                view.edit = std::move(editing);
            }
        return view;
    }
} // namespace cyella
