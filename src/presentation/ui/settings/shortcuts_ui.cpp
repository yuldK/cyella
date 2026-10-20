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
            form.add(field_label(u8"settings-shortcut-input-label", u8"'" + edit.title + u8"' 단축키: 칸을 누르고 키를 누르거나 직접 쓰세요 (여러 개는 ' / '로 이어 씁니다)"), label_height);
            form.add(input_of(kind_shortcut_input, u8"settings-shortcut-input", edit.input, u8"예: Ctrl+Shift+K", u8"단축키 입력"), input_height);
            form.add(help_line(u8"settings-shortcut-default", edit.default_keys.empty() ? u8"기본값: 키 없음" : u8"기본값: " + edit.default_keys), line_height);
            form.add(help_line(u8"settings-shortcut-error", edit.error, error_color()), line_height);
            auto buttons { make_row(u8"settings-shortcut-buttons") };
            buttons->add_flexible_gap();
            buttons->add(button_of(u8"settings-shortcut-cancel", u8"취소", message(cancel_shortcut_edit_intent {})), { .length = 64.0f, .cross_length = button_height });
            buttons->add(button_of(u8"settings-shortcut-apply", u8"적용", message(apply_shortcut_intent {}), edit.can_apply, luil::text_button_visual::accent),
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

        std::unique_ptr<luil::ui_element> build_row(const shortcut_row_view& row)
        {
            const std::u8string key { key_of(row.command) };
            auto line { make_row(u8"shortcut-row-" + key, 8.0f) };
            line->add_flexible(make_text(u8"shortcut-title-" + key, row.title, text_style { .size = 12.0f, .bold = row.customized }));
            line->add(make_text(u8"shortcut-keys-" + key, row.keys.empty() ? std::u8string { u8"(키 없음)" } : row.keys,
                          text_style { .size = 11.0f, .color = row.keys.empty() ? dim : color_selector {}, .code = true }),
                keys_width);
            line->add(button_of(u8"shortcut-edit-" + key, u8"편집", message(edit_shortcut_intent { row.command }), row.editing == false), { .length = edit_width, .cross_length = button_height });
            line->add(button_of(u8"shortcut-reset-" + key, u8"기본값", message(reset_shortcut_intent { row.command }), row.customized), { .length = reset_width, .cross_length = button_height });
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"shortcut-bar-" + key }, bar_style { .divider_bottom = true }, std::move(line));
        }
    } // namespace

    built_settings_page build_shortcuts_page(const shortcuts_page_view& view)
    {
        page_column page { u8"settings-shortcuts-page" };
        page.add(section_title(u8"settings-section-shortcuts", u8"단축키"), section_height);
        page.add(help_line(u8"settings-shortcuts-help-1", u8"명령 팔레트(Ctrl+Shift+P)에서 쓰는 명령과 같은 목록입니다. [편집]을 눌러 새 키를 정하세요."), line_height);
        page.add(help_line(u8"settings-shortcuts-help-2", u8"글자·숫자 키는 Ctrl이나 Alt와 함께여야 하고, 수정키 없이 쓸 수 있는 것은 F1~F12와 Esc뿐입니다. 이미 쓰는 키는 거절합니다."), line_height);
        page.gap(small_gap);
        auto top { make_row(u8"settings-shortcuts-top") };
        top->add_flexible_gap();
        top->add(button_of(u8"settings-shortcuts-reset-all", u8"모두 기본값으로", message(reset_all_shortcuts_intent {}), view.any_customized), { .length = 120.0f, .cross_length = button_height });
        page.add(std::move(top), input_height);
        for (const shortcut_group_view& group : view.groups)
        {
            page.gap(small_gap);
            page.add(section_title(u8"settings-shortcuts-group-" + std::u8string { group.title }, group.title), section_height);
            for (const shortcut_row_view& row : group.rows)
            {
                page.add(build_row(row), row_height);
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
