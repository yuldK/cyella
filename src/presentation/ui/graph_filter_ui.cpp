#include "presentation/ui/graph_filter_ui.h"

#include "domain/text_catalog.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"

#include "luil/theme/ui_theme.h"
#include "luil/ui/draw_primitives.h"
#include "luil/ui/stack_element.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"

#include <memory>
#include <utility>

namespace cyella::ui {
    namespace {
        constexpr float icon_width { 18.0f };
        constexpr float button_height { 22.0f };

        const color_selector accent_color { [](const luil::ui_color_palette& palette) { return palette.accent; } };

        // 띠 전체다. 보조 기술과 UI Automation에 한 묶음으로 서야 "필터가 걸려 있다"를 띠의 있고 없음으로 물을 수 있다
        // (`bar_element`는 구조일 뿐이라 접근 tree에서 접힌다).
        class graph_filter_bar_element final : public luil::ui_element
        {
        public:
            graph_filter_bar_element(std::u8string name, std::unique_ptr<luil::ui_element> content)
                : ui_element { luil::ui_element_id { kind_panel, u8"graph-filter-bar" } }
                , name_ { std::move(name) }
            {
                content_ = content.get();
                add_child(std::move(content));
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                content_->arrange(context.for_child(context.slot));
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const luil::rect_f box { bounds() };
                // 입력 칸과 같은 옅은 바탕이다. 열 머리·검색 막대와 갈려 "평소와 다른 상태"로 읽힌다.
                context.canvas.drawRect(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), luil::solid_paint(context.palette.input_background));
                draw_children(context, interaction);
                context.canvas.drawRect(SkRect::MakeXYWH(box.x, box.y + box.height - context.scale, box.width, context.scale), luil::solid_paint(context.palette.divider));
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::group, .name = name_ };
            }

        private:
            std::u8string name_ {};
            luil::ui_element* content_ { nullptr };
        };
    } // namespace

    std::unique_ptr<luil::ui_element> build_graph_filter_bar(const graph_filter_view& filter)
    {
        luil::stack_config row_config { .direction = luil::stack_direction::row, .spacing = 6.0f, .padding = luil::edge_insets { 10.0f, 0.0f, 6.0f, 0.0f } };
        row_config.cross_alignment = luil::stack_alignment::center;
        auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"graph-filter-row" }, row_config) };
        row->add(std::make_unique<icon_element>(luil::ui_element_id { kind_label, u8"graph-filter-icon" }, codicons::icon_filter, accent_color, 13.0f), icon_width);
        row->add_flexible(make_text(u8"graph-filter-status", filter.status, text_style { .size = 11.5f }));
        // 띠가 서 있는 동안 늘 같은 일을 한다: "이것만 보기"와 숨기기를 모두 푼다 (팔레트의 `graph.filter.clear`와 같다).
        std::u8string label { tr(u8"graph_filter.show_all") };
        const float width { text_button_width(label) };
        row->add(make_text_button(u8"graph-filter-reset", std::move(label), luil::text_button_visual::normal, message(clear_graph_filter_intent {})), { .length = width, .cross_length = button_height });
        return std::make_unique<graph_filter_bar_element>(filter.status, std::move(row));
    }
} // namespace cyella::ui
