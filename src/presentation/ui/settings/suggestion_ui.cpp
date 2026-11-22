#include "presentation/ui/settings/suggestion_ui.h"

#include "domain/text_catalog.h"
#include "presentation/settings/suggestion_intents.h"
#include "presentation/ui_intents.h"

#include "luil/ui/check_element.h"

#include <utility>

namespace cyella::ui {
    using namespace pages;

    void build_suggestion_page(page_column& page, const suggestion_page_view& view, const float content_width)
    {
        page.add(section_title(u8"settings-section-suggestion", tr(u8"settings.suggestion.section")), section_height);
        // staged 변경 내용이 밖으로 나가는 기능이라 사용자가 켜야만 쓴다. 무엇이 나가는지를 토글 바로 아래에 적는다.
        luil::check_config enabled {
            .owner = u8"settings-suggestion-enabled",
            .style = luil::check_style::toggle_switch,
            .label = tr(u8"settings.suggestion.enable"),
            .checked = view.enabled,
            .toggle = action_of(message(set_commit_suggestion_enabled_intent { view.enabled == false })),
        };
        page.add(std::make_unique<luil::check_element>(std::move(enabled)), luil::check_row_height);
        add_help(page, u8"settings-suggestion-enable-help", tr(u8"settings.suggestion.enable_help"), content_width);
        add_help(page, u8"settings-suggestion-enable-scope", tr(u8"settings.suggestion.enable_scope"), content_width);

        page.add(section_title(u8"settings-section-suggestion-command", tr(u8"settings.suggestion.command_section")), section_height);
        add_help(page, u8"settings-suggestion-command-help", tr(u8"settings.suggestion.command_help"), content_width);
        add_help(page, u8"settings-suggestion-environment-help", tr(u8"settings.suggestion.environment_help"), content_width);

        page.add(field_label(u8"settings-suggestion-presets-label", tr(u8"settings.suggestion.presets")), label_height);
        auto presets { make_row(u8"settings-suggestion-presets") };
        for (const suggestion_preset_view& preset : view.presets)
            presets->add(button_of(u8"settings-suggestion-preset-" + preset.id, preset.name, message(fill_commit_suggestion_preset_intent { preset.id })),
                { .length = text_button_width(preset.name), .cross_length = button_height });
        presets->add_flexible_gap();
        page.add(std::move(presets), input_height);
        page.gap(4.0f);
        add_help(page, u8"settings-suggestion-preset-help", tr(u8"settings.suggestion.preset_help"), content_width);

        page.add(field_label(u8"settings-suggestion-executable-label", tr(u8"settings.suggestion.executable")), label_height);
        auto executable_row { make_row(u8"settings-suggestion-executable-row") };
        executable_row->add_flexible(input_of(
            kind_settings_suggestion_executable_input, u8"settings-suggestion-executable", view.executable, tr(u8"settings.suggestion.executable_placeholder"), tr(u8"settings.suggestion.executable")));
        executable_row->add(button_of(u8"settings-suggestion-browse", tr(u8"settings.common.browse"), ui_command(static_cast<std::uint32_t>(ui_command_id::choose_suggestion_executable))),
            { .length = 88.0f, .cross_length = button_height });
        page.add(std::move(executable_row), input_height);

        page.add(field_label(u8"settings-suggestion-arguments-label", tr(u8"settings.suggestion.arguments")), label_height);
        page.add(input_of(kind_settings_suggestion_arguments_input, u8"settings-suggestion-arguments", view.arguments, tr(u8"settings.suggestion.arguments_placeholder"), tr(u8"settings.suggestion.arguments")),
            input_height);

        page.add(field_label(u8"settings-suggestion-instruction-label", tr(u8"settings.suggestion.instruction")), label_height);
        page.add(input_of(
                     kind_settings_suggestion_instruction_input, u8"settings-suggestion-instruction", view.instruction, tr(u8"settings.suggestion.instruction_placeholder")
                     , tr(u8"settings.suggestion.instruction")),
            input_height);
        page.gap(4.0f);

        // 한 줄에 하나만 보인다: 저장을 막는 문제, 주의, 저장 전이라는 안내 순이다.
        if (view.error.empty() == false)
            page.add(help_line(u8"settings-suggestion-validation", view.error, tone_color(tone::bad)), line_height);
        else if (view.warning.empty() == false)
            page.add(help_line(u8"settings-suggestion-validation", view.warning, tone_color(tone::warning)), line_height);
        else if (view.dirty)
            page.add(help_line(u8"settings-suggestion-validation", tr(u8"settings.suggestion.pending")), line_height);
        else
            page.gap(line_height); // 알릴 것이 없어도 자리는 둔다. 글이 생길 때 [저장] 단추가 밀리지 않는다.
        page.gap(small_gap);

        auto actions { make_row(u8"settings-suggestion-actions") };
        actions->add_flexible_gap();
        actions->add(button_of(u8"settings-suggestion-save", tr(u8"settings.common.save"), message(save_commit_suggestion_intent {}), view.can_save, luil::text_button_visual::accent),
            { .length = 56.0f, .cross_length = button_height });
        page.add(std::move(actions), input_height);
    }
} // namespace cyella::ui
