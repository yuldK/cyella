// 환경 설정 "단축키" 쪽의 메시지 처리다. settings_controller의 멤버 함수를 파일 하나로 나눠 둔다.
#include "presentation/commands/command_registry.h"
#include "presentation/settings/settings_controller.h"
#include "presentation/settings/shortcuts_intents.h"

#include <utility>

namespace cyella {
    namespace {
        void set_text(luil::text::text_edit_state& state, std::u8string text)
        {
            state = {};
            state.caret = text.size();
            state.anchor = text.size();
            state.text = std::move(text);
        }
    } // namespace

    bool settings_controller::is_shortcut_message(const luil::app_message& message)
    {
        return message.get<edit_shortcut_intent>() != nullptr || message.get<apply_shortcut_intent>() != nullptr || message.get<cancel_shortcut_edit_intent>() != nullptr
            || message.get<reset_shortcut_intent>() != nullptr || message.get<reset_all_shortcuts_intent>() != nullptr || message.get<capture_shortcut_intent>() != nullptr;
    }

    void settings_controller::handle_shortcut_message(const luil::app_message& message)
    {
        settings_state& state { *state_ };
        if (const auto* const edit_intent { message.get<edit_shortcut_intent>() }; edit_intent != nullptr)
        {
            if (find_command(edit_intent->command) == nullptr)
                return;
            shortcut_edit_state edit {};
            edit.command = edit_intent->command;
            set_text(edit.text, shortcut_text(dependencies_.settings(), edit_intent->command));
            state.shortcut_edit = std::move(edit);
            state.message.clear();
        }
        else if (const auto* const capture_intent { message.get<capture_shortcut_intent>() }; capture_intent != nullptr)
        {
            // 실제로 누른 키다. 칸이 비어 있던 것처럼 그 키 하나로 바꾼다 (여러 개는 손으로 ` / `로 잇는다).
            if (state.shortcut_edit.has_value() && capture_intent->chord.valid())
                set_text(state.shortcut_edit->text, format_key_chord(capture_intent->chord));
        }
        else if (message.get<apply_shortcut_intent>() != nullptr)
        {
            if (state.shortcut_edit.has_value() == false)
                return;
            const std::string command { state.shortcut_edit->command };
            const shortcut_edit_check check { check_shortcut_edit(dependencies_.settings(), command, state.shortcut_edit->text.text) };
            if (check.error.empty() == false)
                return;
            const std::vector<key_chord> chords { check.chords };
            update_settings([&command, &chords](app_settings& settings) { set_command_shortcuts(settings, command, chords); });
            const command_definition* const definition { find_command(command) };
            state.message = u8"'" + (definition != nullptr ? definition->title : std::u8string {}) + u8"' 단축키를 " + (chords.empty() ? std::u8string { u8"없앴습니다." } : u8"바꿨습니다.");
            state.shortcut_edit.reset();
        }
        else if (message.get<cancel_shortcut_edit_intent>() != nullptr)
            state.shortcut_edit.reset();
        else if (const auto* const reset_intent { message.get<reset_shortcut_intent>() }; reset_intent != nullptr)
        {
            const std::string command { reset_intent->command };
            update_settings([&command](app_settings& settings) { reset_command_shortcuts(settings, command); });
            if (state.shortcut_edit.has_value() && state.shortcut_edit->command == command)
                state.shortcut_edit.reset();
            const command_definition* const definition { find_command(command) };
            state.message = u8"'" + (definition != nullptr ? definition->title : std::u8string {}) + u8"' 단축키를 기본값으로 되돌렸습니다.";
        }
        else if (message.get<reset_all_shortcuts_intent>() != nullptr)
        {
            update_settings([](app_settings& settings) { reset_all_shortcuts(settings); });
            state.shortcut_edit.reset();
            state.message = u8"모든 단축키를 기본값으로 되돌렸습니다.";
        }
    }
} // namespace cyella
