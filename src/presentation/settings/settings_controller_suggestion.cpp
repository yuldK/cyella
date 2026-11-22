// 환경 설정 "AI" 쪽의 메시지 처리다 (ADR-009). settings_controller의 멤버 함수를 파일 하나로 나눠 둔다.
#include "domain/commit_suggestion.h"
#include "domain/text_catalog.h"
#include "presentation/settings/settings_controller.h"
#include "presentation/settings/suggestion_intents.h"
#include "presentation/settings/suggestion_page.h"

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

        std::u8string trimmed(std::u8string_view text)
        {
            while (text.empty() == false && (text.front() == u8' ' || text.front() == u8'\t'))
                text.remove_prefix(1);
            while (text.empty() == false && (text.back() == u8' ' || text.back() == u8'\t'))
                text.remove_suffix(1);
            return std::u8string { text };
        }
    } // namespace

    bool settings_controller::is_suggestion_message(const luil::app_message& message)
    {
        return message.get<set_commit_suggestion_enabled_intent>() != nullptr || message.get<fill_commit_suggestion_preset_intent>() != nullptr
            || message.get<save_commit_suggestion_intent>() != nullptr;
    }

    void settings_controller::handle_suggestion_message(const luil::app_message& message)
    {
        settings_state& state { *state_ };
        if (const auto* const enabled_intent { message.get<set_commit_suggestion_enabled_intent>() }; enabled_intent != nullptr)
        {
            // 켬 여부는 다른 토글처럼 바로 저장한다. 명령 칸의 초안은 건드리지 않는다.
            const bool on { enabled_intent->on };
            update_settings([on](app_settings& settings) { settings.commit_suggestion.enabled = on; });
        }
        else if (const auto* const preset_intent { message.get<fill_commit_suggestion_preset_intent>() }; preset_intent != nullptr)
        {
            for (const commit_suggestion_preset& preset : commit_suggestion_presets())
                if (preset.id == preset_intent->id)
                {
                    set_text(state.suggestion_executable, std::u8string { preset.executable });
                    set_text(state.suggestion_arguments, std::u8string { preset.arguments });
                    state.message.clear();
                    break;
                }
        }
        else if (message.get<save_commit_suggestion_intent>() != nullptr)
        {
            if (check_suggestion_draft(state.suggestion_executable.text, state.suggestion_arguments.text, dependencies_.probe).error.empty() == false)
                return;
            commit_suggestion_settings saved { dependencies_.settings().commit_suggestion };
            saved.executable = trimmed(state.suggestion_executable.text);
            saved.arguments = trimmed(state.suggestion_arguments.text);
            saved.instruction = trimmed(state.suggestion_instruction.text);
            update_settings([&saved](app_settings& settings) { settings.commit_suggestion = saved; });
            set_text(state.suggestion_executable, saved.executable);
            set_text(state.suggestion_arguments, saved.arguments);
            set_text(state.suggestion_instruction, saved.instruction);
            state.message = tr(u8"settings_view.suggestion.saved");
        }
    }
} // namespace cyella
