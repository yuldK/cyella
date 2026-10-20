#include "presentation/ui/diff_special_card.h"

#include "application/app_messages.h"
#include "presentation/extension_intents.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ref_chip_element.h"
#include "presentation/ui/ui_helpers.h"

#include "luil/ui/stack_element.h"

#include <algorithm>
#include <utility>

namespace cyella::ui {
    namespace {
        constexpr float card_width { 560.0f };
        constexpr float row_height { 24.0f };
        constexpr float label_column { 84.0f };
        constexpr float note_line_height { 18.0f };
        constexpr float note_font { 11.5f };
        constexpr float button_height { 30.0f };

        const color_selector dim_color { [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } };
        const color_selector warning_color { [](const luil::ui_color_palette& palette) { return status_color(palette, 'M'); } };

        luil::ui_element_id layout_id(std::u8string owner)
        {
            return luil::ui_element_id { kind_layout, std::move(owner) };
        }

        luil::stack_config row_config()
        {
            return luil::stack_config { .direction = luil::stack_direction::row, .spacing = 8.0f };
        }

        std::unique_ptr<luil::ui_element> detail_row(const std::u8string& owner, const diff_special_row& row)
        {
            auto line { std::make_unique<luil::stack_element>(layout_id(owner), row_config()) };
            line->add(make_text(owner + u8"-label", row.label, text_style { .size = 12.0f, .color = dim_color }), label_column);
            line->add_flexible(make_text(owner + u8"-value", row.value, text_style { .size = 12.0f, .code = true }));
            return line;
        }
    } // namespace

    std::unique_ptr<luil::ui_element> build_diff_special_card(const diff_special_view& special, const std::string& repository_id)
    {
        const std::vector<std::u8string> note_lines { wrap_estimated(special.note, card_width - 24.0f, note_font) };
        const bool has_buttons { special.action.has_value() || special.open_path.empty() == false };

        const luil::stack_config card_config { .spacing = 6.0f, .padding = luil::edge_insets::all(12.0f) };
        auto card { std::make_unique<luil::stack_element>(layout_id(u8"diff-special-card"), card_config) };
        float height { 24.0f };
        auto title_row { std::make_unique<luil::stack_element>(layout_id(u8"diff-special-title-row"), row_config()) };
        title_row->add(std::make_unique<icon_element>(luil::ui_element_id { kind_label, u8"diff-special-icon" },
                           special.kind == diff_special_kind::lfs_pointer ? codicons::icon_package : codicons::icon_file_submodule, color_selector {}, 16.0f),
            20.0f);
        title_row->add_flexible(make_text(u8"diff-special-title", special.title, text_style { .size = 14.0f, .bold = true }));
        card->add(std::move(title_row), 24.0f);
        for (std::size_t index { 0 }; index < special.rows.size(); ++index)
        {
            card->add(detail_row(u8"diff-special-row-" + number_text(index), special.rows[index]), row_height);
            height += row_height + 6.0f;
        }
        card->add_gap(4.0f);
        height += 10.0f;
        for (std::size_t index { 0 }; index < note_lines.size(); ++index)
        {
            card->add(make_text(u8"diff-special-note-" + number_text(index), note_lines[index], text_style { .size = note_font, .color = dim_color }), note_line_height);
            height += note_line_height + 6.0f;
        }
        if (has_buttons)
        {
            auto buttons { std::make_unique<luil::stack_element>(layout_id(u8"diff-special-buttons"), row_config()) };
            if (special.action.has_value())
                buttons->add(make_text_button(u8"diff-special-action", special.action_label, luil::text_button_visual::accent, message(submit_operation_intent { repository_id, *special.action }),
                                 true, codicons::icon_cloud_download),
                    text_button_width(special.action_label, true));
            if (special.open_path.empty() == false)
                buttons->add(
                    make_text_button(u8"diff-special-open", u8"서브모듈 열기", luil::text_button_visual::accent, message(open_submodule_intent { special.open_path }), true, codicons::icon_repo),
                    text_button_width(u8"서브모듈 열기", true));
            buttons->add_flexible_gap();
            card->add_gap(6.0f);
            card->add(std::move(buttons), button_height);
            height += button_height + 12.0f;
        }
        height += 24.0f;

        // 카드를 판 위쪽 가운데에 둔다.
        auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"diff-special-row" }, luil::stack_config { .direction = luil::stack_direction::row }) };
        row->add_flexible_gap();
        row->add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"diff-special-surface" },
                     bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.window_background; } }, std::move(card)),
            card_width);
        row->add_flexible_gap();
        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"diff-special" }, luil::stack_config { .padding = luil::edge_insets { 0.0f, 24.0f, 0.0f, 0.0f } }) };
        column->add(std::move(row), height);
        column->add_flexible_gap();
        return column;
    }
} // namespace cyella::ui
