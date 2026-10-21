#include "domain/text_catalog.h"
#include "presentation/settings/shortcuts_intents.h"
#include "presentation/ui/settings/settings_page_parts.h"
#include "presentation/ui/settings/settings_pages_ui.h"
#include "presentation/ui_intents.h"

#include <algorithm>
#include <utility>

namespace cyella::ui {
    namespace {
        using namespace pages;

        constexpr float keys_width { 240.0f };
        constexpr float edit_width { 56.0f };
        constexpr float reset_width { 70.0f };

        // 고치는 줄 아래에 서는 입력 칸이다. 칸에 초점을 두고 키를 누르면 그 조합이 적히고, 손으로 쳐도 된다.
        void add_edit_form(page_column& page, const shortcut_edit_view& edit)
        {
            page_column form { u8"settings-shortcut-form-column" };
            form.add(field_label(u8"settings-shortcut-input-label", tr(u8"settings.shortcuts.input_label", { { u8"command", edit.title } })), label_height);
            form.add(input_of(kind_shortcut_input, u8"settings-shortcut-input", edit.input, tr(u8"settings.shortcuts.input_placeholder"), tr(u8"settings.shortcuts.input_name")), input_height);
            form.add(help_line(u8"settings-shortcut-default",
                         edit.default_keys.empty() ? tr(u8"settings.shortcuts.default_none") : tr(u8"settings.shortcuts.default_keys", { { u8"keys", edit.default_keys } })),
                line_height);
            form.add(help_line(u8"settings-shortcut-error", edit.error, error_color()), line_height);
            auto buttons { make_row(u8"settings-shortcut-buttons") };
            buttons->add_flexible_gap();
            buttons->add(button_of(u8"settings-shortcut-cancel", tr(u8"settings.common.cancel"), message(cancel_shortcut_edit_intent {})), { .length = 64.0f, .cross_length = button_height });
            buttons->add(button_of(u8"settings-shortcut-apply", tr(u8"settings.common.apply"), message(apply_shortcut_intent {}), edit.can_apply, luil::text_button_visual::accent),
                { .length = 64.0f, .cross_length = button_height });
            form.add(std::move(buttons), input_height);

            const float height { form.height() + 24.0f };
            const luil::stack_config padding_config { .padding = luil::edge_insets::all(12.0f) };
            auto padded { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"settings-shortcut-form-padding" }, padding_config) };
            padded->add_flexible(form.take());
            page.add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-shortcut-form" },
                         bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.window_background; } }, std::move(padded)),
                height);
        }

        std::unique_ptr<luil::ui_element> build_row(const shortcut_row_view& row, const bool stacked)
        {
            const std::u8string key { key_of(row.command) };
            auto line { make_row(u8"shortcut-row-" + key, 8.0f) };
            auto title { make_text(u8"shortcut-title-" + key, row.title, text_style { .size = 12.0f, .bold = row.customized }) };
            if (stacked == false)
                line->add_flexible(std::move(title));
            const text_style key_style { .size = 11.0f, .color = row.keys.empty() ? dim : color_selector {}, .code = true };
            auto keys { make_text(u8"shortcut-keys-" + key, row.keys.empty() ? std::u8string { tr(u8"settings.shortcuts.no_keys") } : row.keys, key_style) };
            if (stacked)
                line->add_flexible(std::move(keys));
            else
                line->add(std::move(keys), keys_width);
            line->add(button_of(u8"shortcut-edit-" + key, tr(u8"settings.common.edit"), message(edit_shortcut_intent { row.command }), row.editing == false),
                { .length = edit_width, .cross_length = button_height });
            line->add(button_of(u8"shortcut-reset-" + key, tr(u8"settings.shortcuts.reset"), message(reset_shortcut_intent { row.command }), row.customized),
                { .length = reset_width, .cross_length = button_height });
            if (stacked == false)
                return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"shortcut-bar-" + key }, bar_style { .divider_bottom = true }, std::move(line));
            auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"shortcut-stack-" + key }, luil::stack_config {}) };
            column->add(std::move(title), 24.0f);
            column->add(std::move(line), row_height);
            column->add_gap(small_gap);
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"shortcut-bar-" + key }, bar_style { .divider_bottom = true }, std::move(column));
        }
    } // namespace

    built_settings_page build_shortcuts_page(const shortcuts_page_view& view, const float content_width)
    {
        page_column page { u8"settings-shortcuts-page" };
        const bool stacked { content_width < 560.0f };
        page.add(section_title(u8"settings-section-shortcuts", tr(u8"settings.pages.shortcuts")), section_height);
        add_help(page, u8"settings-shortcuts-help-1", tr(u8"settings.shortcuts.help_1"), content_width);
        add_help(page, u8"settings-shortcuts-help-2", tr(u8"settings.shortcuts.help_2"), content_width);
        page.gap(small_gap);
        auto top { make_row(u8"settings-shortcuts-top") };
        top->add_flexible_gap();
        top->add(button_of(u8"settings-shortcuts-reset-all", tr(u8"settings.shortcuts.reset_all"), message(reset_all_shortcuts_intent {}), view.any_customized),
            { .length = 120.0f, .cross_length = button_height });
        page.add(std::move(top), input_height);
        for (const shortcut_group_view& group : view.groups)
        {
            page.gap(small_gap);
            page.add(section_title(u8"settings-shortcuts-group-" + group.owner, group.title), section_height);
            for (const shortcut_row_view& row : group.rows)
            {
                page.add(build_row(row, stacked), stacked ? 24.0f + row_height + small_gap : row_height);
                if (row.editing && view.edit.has_value())
                {
                    page.gap(small_gap);
                    add_edit_form(page, *view.edit);
                    page.gap(small_gap);
                }
            }
        }
        const float height { page.height() };
        return built_settings_page { page.take(), height };
    }
} // namespace cyella::ui
