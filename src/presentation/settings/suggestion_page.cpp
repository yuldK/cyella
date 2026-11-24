#include "presentation/settings/suggestion_page.h"

#include "domain/path_syntax.h"
#include "domain/text_catalog.h"
#include "presentation/ui_state.h"

namespace cyella {
    namespace {
        std::u8string_view trimmed(std::u8string_view text) noexcept
        {
            while (text.empty() == false && (text.front() == u8' ' || text.front() == u8'\t'))
                text.remove_prefix(1);
            while (text.empty() == false && (text.back() == u8' ' || text.back() == u8'\t'))
                text.remove_suffix(1);
            return text;
        }

#if defined(_WIN32)
        bool ends_with_ignoring_case(const std::u8string_view text, const std::u8string_view suffix) noexcept
        {
            if (text.size() < suffix.size())
                return false;
            for (std::size_t index { 0 }; index < suffix.size(); ++index)
            {
                char8_t value { text[text.size() - suffix.size() + index] };
                if (value >= u8'A' && value <= u8'Z')
                    value = static_cast<char8_t>(value - u8'A' + u8'a');
                if (value != suffix[index])
                    return false;
            }
            return true;
        }
#endif
    } // namespace

    suggestion_draft_check check_suggestion_draft(const std::u8string_view executable, const std::u8string_view arguments, const file_probe* const probe)
    {
        suggestion_draft_check check {};
        if (split_commit_suggestion_arguments(arguments).has_value() == false)
        {
            check.error = tr(u8"settings_view.suggestion.unclosed_quote");
            return check;
        }

        const std::u8string_view path { trimmed(executable) };
#if defined(_WIN32)
        if (ends_with_ignoring_case(path, u8".cmd") || ends_with_ignoring_case(path, u8".bat"))
        {
            check.error = tr(u8"settings_view.suggestion.script_not_supported");
            return check;
        }
#endif
        // 이름만 적었으면 실행할 때 찾는다. 구분자나 드라이브가 있으면 절대 경로여야 한다 — 상대 경로는 어느 폴더 기준인지 정할 수 없다.
        if (path.find_first_of(u8"\\/:") == std::u8string_view::npos)
            return check;
        if (is_absolute_path(path) == false)
        {
            check.error = tr(u8"settings_view.suggestion.relative_path");
            return check;
        }
        if (probe != nullptr && probe->probe(path) != path_kind::file)
            check.warning = tr(u8"settings_view.suggestion.executable_missing");
        return check;
    }

    suggestion_page_view build_suggestion_page_view(
        const commit_suggestion_settings& settings, const settings_state& state, const std::optional<luil::text_composition_event>& composition, const file_probe* const probe)
    {
        suggestion_page_view view {};
        view.enabled = settings.enabled;
        for (const commit_suggestion_preset& preset : commit_suggestion_presets())
            view.presets.push_back({ std::u8string { preset.id }, std::u8string { preset.name } });
        view.executable = luil::make_text_input_view(state.suggestion_executable, composition, to_luil_target(text_target::settings_suggestion_executable));
        view.arguments = luil::make_text_input_view(state.suggestion_arguments, composition, to_luil_target(text_target::settings_suggestion_arguments));
        view.instruction = luil::make_text_input_view(state.suggestion_instruction, composition, to_luil_target(text_target::settings_suggestion_instruction));

        const suggestion_draft_check check { check_suggestion_draft(state.suggestion_executable.text, state.suggestion_arguments.text, probe) };
        view.error = check.error;
        view.warning = check.warning;
        view.dirty = trimmed(state.suggestion_executable.text) != settings.executable || trimmed(state.suggestion_arguments.text) != settings.arguments
            || trimmed(state.suggestion_instruction.text) != settings.instruction;
        view.can_save = view.dirty && view.error.empty();
        return view;
    }
} // namespace cyella
