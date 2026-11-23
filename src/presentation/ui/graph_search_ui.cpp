#include "presentation/ui/graph_search_ui.h"

#include "domain/text_catalog.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"

#include "luil/ui/dialog_elements.h"
#include "luil/ui/stack_element.h"

#include <memory>
#include <utility>

namespace cyella::ui {
    namespace {
        constexpr float status_width { 96.0f };
        constexpr float button_width { 28.0f };

        const color_selector dim_color { [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } };
    } // namespace

    std::unique_ptr<luil::ui_element> build_graph_search_bar(const graph_search_view& search)
    {
        luil::stack_config row_config { .direction = luil::stack_direction::row, .spacing = 4.0f, .padding = luil::edge_insets { 8.0f, 4.0f, 6.0f, 4.0f } };
        row_config.cross_alignment = luil::stack_alignment::center;
        auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"graph-search-row" }, row_config) };
        luil::text_input_config input_config {
            .placeholder = tr(u8"graph_search.placeholder"),
            .leading_glyph = codicons::icon_search,
            .clear = action_of(message(edit_text_intent { luil::text_edit_request { .target = to_luil_target(text_target::graph_search), .command = luil::text::text_edit_command::clear } })),
            .clear_tooltip = tr(u8"graph_search.clear"),
        };
        row->add_flexible(std::make_unique<luil::text_input_element>(luil::ui_element_id { kind_graph_search_input, u8"graph-search" }, search.input, std::move(input_config)));
        // "2 / 17", "결과 없음", "찾는 중…"이다. 폭을 고정해 글이 바뀌어도 단추가 움직이지 않는다.
        row->add(make_text(u8"graph-search-status", search.status, text_style { .size = 11.0f, .color = dim_color, .align = 2 }), status_width);
        // 키(Enter·Shift+Enter·Esc)와 같은 일이다. 단추가 있어야 마우스와 보조 기술로도 닿는다.
        row->add(make_icon_button(u8"graph-search-previous", codicons::icon_arrow_up, tr(u8"graph_search.previous"), message(graph_search_step_intent { .forward = false }), search.can_step),
            button_width);
        row->add(make_icon_button(u8"graph-search-next", codicons::icon_arrow_down, tr(u8"graph_search.next"), message(graph_search_step_intent { .forward = true }), search.can_step), button_width);
        row->add(make_icon_button(u8"graph-search-close", codicons::icon_close, tr(u8"graph_search.close"), message(close_graph_search_intent {})), button_width);
        return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"graph-search-bar" }, bar_style { .divider_bottom = true }, std::move(row));
    }
} // namespace cyella::ui
