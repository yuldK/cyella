#include "presentation/ui/palette_ui.h"

#include "presentation/palette/palette_intents.h"
#include "presentation/ui/command_elements.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ref_chip_element.h"
#include "presentation/ui/ui_helpers.h"

#include "luil/ui/modal_host_element.h"
#include "luil/ui/stack_element.h"
#include "luil/ui/virtual_list_element.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace cyella::ui {
    namespace {
        constexpr float dialog_maximum_width { 640.0f };
        constexpr float dialog_margin { 24.0f };
        constexpr float dialog_top_gap { 56.0f };
        constexpr float padding { 10.0f };
        constexpr float title_height { 18.0f };
        constexpr float input_height { 32.0f };
        constexpr float row_height { 28.0f };
        constexpr float help_height { 18.0f };
        constexpr float gap { 6.0f };

        const color_selector dim { [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } };

        // 행 내용이다. 왼쪽에 제목, 오른쪽에 흐린 보조 글(단축키·경로)을 두고 커서가 선 행은 강조색으로 칠한다.
        std::unique_ptr<luil::ui_element> build_row_content(const palette_item_view& item, const std::size_t index, const bool cursor)
        {
            const std::u8string suffix { number_text(index) };
            luil::stack_config config { .direction = luil::stack_direction::row, .spacing = 8.0f, .padding = luil::edge_insets { 12.0f, 0.0f, 12.0f, 0.0f } };
            config.cross_alignment = luil::stack_alignment::center;
            auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"palette-row-" + suffix }, config) };
            const color_selector title_color { [enabled = item.enabled](const luil::ui_color_palette& palette) { return enabled ? palette.primary_foreground : palette.secondary_foreground; } };
            row->add_flexible(make_text(u8"palette-row-title-" + suffix, item.title, text_style { .size = 12.5f, .color = title_color }));
            if (item.detail.empty() == false)
            {
                const float width { std::min(260.0f, estimate_text_width(item.detail, 11.0f) + 8.0f) };
                row->add(make_text(u8"palette-row-detail-" + suffix, item.detail, text_style { .size = 11.0f, .color = dim, .align = 2 }), width);
            }
            bar_style style {};
            if (cursor)
                style.background = [](const luil::ui_color_palette& palette) { return luil::with_alpha(palette.accent, 0.22f); };
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"palette-row-bar-" + suffix }, std::move(style), std::move(row));
        }
    } // namespace

    std::unique_ptr<luil::ui_element> build_palette_dialog(const palette_view& view, const float window_width, const float window_height)
    {
        const float width { std::min(dialog_maximum_width, std::max(window_width - dialog_margin * 2.0f, 280.0f)) };
        const float list_height { static_cast<float>(view.items.size()) * row_height };
        // 둘레 여백 두 번 + 제목 + 거르는 칸 + 목록 + 도움말 + 표식 한 칸(높이 0)까지 사이마다 간격이다.
        const float wanted { padding * 2.0f + title_height + input_height + list_height + help_height + gap * 5.0f };
        const float height { std::min(wanted, std::max(window_height - dialog_margin * 2.0f, 200.0f)) };

        luil::modal_host_config config {};
        config.owner = u8"palette-dialog";
        config.name = view.title;
        config.scrim_opacity = 0.25f;
        config.dismiss = action_of(message(close_palette_intent {}));
        config.outside = action_of(message(close_palette_intent {}));
        config.focus_entry = luil::ui_element_id { kind_palette_filter_input, u8"palette-filter" };
        config.surface = floating_panel_config(8.0f, 0.4f);
        config.content_width = width;
        config.content_height = height;
        // 가운데보다 위에 놓는다 (명령 팔레트는 보통 창 위쪽에서 내려온다).
        config.offset_y = std::min(0.0f, dialog_top_gap - (window_height - height) / 2.0f);
        auto host { std::make_unique<luil::modal_host_element>(std::move(config)) };

        luil::stack_config column_config { .spacing = gap, .padding = luil::edge_insets::all(padding) };
        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"palette-column" }, column_config) };
        column->add(make_text(u8"palette-title", view.title, text_style { .size = 12.0f, .color = dim, .bold = true }), title_height);

        luil::text_input_config filter_config {};
        filter_config.placeholder = view.placeholder;
        filter_config.leading_glyph = codicons::icon_search;
        auto filter { std::make_unique<luil::text_input_element>(luil::ui_element_id { kind_palette_filter_input, u8"palette-filter" }, view.filter, std::move(filter_config)) };
        filter->set_access_name(view.title + u8" 검색");
        column->add(std::move(filter), input_height);

        luil::virtual_list_config list_config {};
        list_config.owner = u8"palette-list";
        for (const palette_item_view& item : view.items)
            list_config.items.push_back({ .key = item.key, .label = item.title, .enabled = item.enabled });
        list_config.row_height = row_height;
        list_config.cursor = view.cursor;
        auto items { std::make_shared<std::vector<palette_item_view>>(view.items) };
        const std::u8string cursor { view.cursor };
        list_config.build_row = [items, cursor](const luil::virtual_list_item&, const std::size_t index, const bool) -> std::unique_ptr<luil::ui_element> {
            return build_row_content((*items)[index], index, (*items)[index].key == cursor);
        };
        // 행을 누르거나(UIA Select) 키보드 커서에서 Enter·Space를 누르면 고른 것이다. 화살표는 커서만 옮긴다.
        const auto find_item = [items](const std::u8string& key) -> const palette_item_view* {
            const auto found { std::ranges::find(*items, key, &palette_item_view::key) };
            return found == items->end() ? nullptr : &*found;
        };
        const auto pick = [find_item](const std::u8string& key) -> luil::input_action {
            const palette_item_view* const item { find_item(key) };
            return item == nullptr || item->enabled == false ? luil::input_action {} : palette_item_action(*item);
        };
        list_config.select = pick;
        list_config.activate = pick;
        list_config.move = [](const std::u8string& key) { return message(palette_cursor_intent { key }); };
        auto list { std::make_unique<luil::virtual_list_element>(std::move(list_config)) };
        list->set_access_name(view.title + u8" 목록");
        column->add(std::move(list), list_height);

        column->add(make_text(u8"palette-help", u8"↑↓로 고르고 Enter로 실행합니다 · Esc로 닫습니다", text_style { .size = 11.0f, .color = dim }), help_height);

        // 거르는 칸의 Enter가 읽을 표식이다. 커서 항목이 없으면 낼 것이 없다.
        luil::input_action enter {};
        for (const palette_item_view& item : view.items)
            if (item.key == view.cursor && item.enabled)
                enter = palette_item_action(item);
        column->add(std::make_unique<palette_enter_element>(std::move(enter)), 0.0f);

        host->set_content(std::move(column));
        return host;
    }
} // namespace cyella::ui
