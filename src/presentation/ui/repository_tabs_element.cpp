#include "presentation/ui/repository_tabs_element.h"

#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"

#include "luil/ui/draw_primitives.h"
#include "luil/ui/stack_element.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"

#include <algorithm>
#include <utility>

namespace cyella::ui {
    namespace {
        constexpr float add_width { 32.0f };
        constexpr float scrollbar_height { 10.0f };

        class tab_scrollbar_element final : public luil::ui_element
        {
        public:
            tab_scrollbar_element(const float content, const float viewport, const float offset)
                : ui_element { { luil::ui_element_kind::scroll_area_bar, u8"tabs-scroll" } }
                , content_ { content }
                , viewport_ { viewport }
                , offset_ { offset }
            {
                set_access_name(u8"탭 가로 스크롤");
                set_tab_stop(true);
                luil::pointer_drag_target drag {};
                drag.follows_pointer = true;
                drag.on_press = [this](const luil::ui_action_context& context) -> std::vector<luil::input_action> {
                    if (context.x >= thumb_left_ && context.x <= thumb_left_ + thumb_width_)
                        return {};
                    return { message(scroll_tabs_intent { delta_for(context.x - thumb_left_ - thumb_width_ / 2.0f) }) };
                };
                drag.on_move = [this](const luil::ui_action_context& previous, const luil::ui_action_context& current) -> std::vector<luil::input_action> {
                    return { message(scroll_tabs_intent { delta_for(current.x - previous.x) }) };
                };
                set_pointer_drag_target(std::move(drag));
                luil::key_step_target keys {};
                keys.axis = luil::focus_axis::horizontal;
                keys.on_step = [this](const luil::value_step step) -> std::optional<std::vector<luil::input_action>> {
                    float delta { 0.0f };
                    switch (step)
                    {
                    case luil::value_step::decrease:
                        delta = -48.0f;
                        break;
                    case luil::value_step::increase:
                        delta = 48.0f;
                        break;
                    case luil::value_step::decrease_page:
                        delta = -viewport_;
                        break;
                    case luil::value_step::increase_page:
                        delta = viewport_;
                        break;
                    case luil::value_step::minimum:
                        delta = -offset_;
                        break;
                    case luil::value_step::maximum:
                        delta = content_ - viewport_ - offset_;
                        break;
                    }
                    return std::vector<luil::input_action> { message(scroll_tabs_intent { delta }) };
                };
                set_key_step_target(std::move(keys));
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                const float scale { context.scale };
                track_width_ = std::max(context.slot.width - 8.0f * scale, 0.0f);
                thumb_width_ = std::min(track_width_, std::max(24.0f * scale, track_width_ * viewport_ / content_));
                thumb_left_ = context.slot.x + 4.0f * scale + (track_width_ - thumb_width_) * offset_ / (content_ - viewport_);
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const float height { 4.0f * context.scale };
                const float top { bounds().y + (bounds().height - height) / 2.0f };
                const SkPaint track { luil::solid_paint(luil::with_alpha(context.palette.primary_foreground, 0.07f)) };
                context.canvas.drawRoundRect(SkRect::MakeXYWH(bounds().x + 4.0f * context.scale, top, track_width_, height), height / 2.0f, height / 2.0f, track);
                const bool active { interaction.hovered == id() || interaction.pressed == id() };
                const SkPaint thumb { luil::solid_paint(luil::with_alpha(context.palette.primary_foreground, active ? 0.65f : 0.38f)) };
                context.canvas.drawRoundRect(SkRect::MakeXYWH(thumb_left_, top, thumb_width_, height), height / 2.0f, height / 2.0f, thumb);
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::scroll_bar, .name = access_name(), .range = luil::access_range { 0.0f, content_ - viewport_, offset_ } };
            }

            [[nodiscard]] std::optional<std::vector<luil::input_action>> access_actions(const luil::access_request& request) const override
            {
                if (request.command != luil::access_command::set_value)
                    return ui_element::access_actions(request);
                return std::vector<luil::input_action> { message(scroll_tabs_intent { std::clamp(request.value, 0.0f, content_ - viewport_), true }) };
            }

        private:
            [[nodiscard]] float delta_for(const float pixels) const noexcept
            {
                const float travel { track_width_ - thumb_width_ };
                return travel > 0.0f ? pixels * (content_ - viewport_) / travel : 0.0f;
            }

            float content_ { 0.0f };
            float viewport_ { 0.0f };
            float offset_ { 0.0f };
            float track_width_ { 0.0f };
            float thumb_left_ { 0.0f };
            float thumb_width_ { 0.0f };
        };

        void collect_tabs(luil::ui_element& element, std::vector<const luil::ui_element*>& tabs)
        {
            if (element.id().kind == luil::ui_element_kind::tab)
            {
                tabs.push_back(&element);
                const std::u8string name { element.accessibility().name + u8" 탭 닫기" };
                for (const auto& child : element.children())
                    if (child->id().kind == luil::ui_element_kind::tab_close)
                    {
                        child->set_access_name(name);
                        child->set_tooltip(name);
                    }
            }
            for (const auto& child : element.children())
                collect_tabs(*child, tabs);
        }
    } // namespace

    repository_tabs_element::repository_tabs_element(luil::tab_bar_config config, std::unique_ptr<luil::ui_element> add_button, const float viewport_width, const bool reveal_selected)
        : ui_element { { kind_layout, u8"repository-tabs" } }
        , selected_ { config.selected }
    {
        const float tabs_width { static_cast<float>(config.items.size()) * config.tab_width };
        const float content_width { tabs_width + add_width };
        const float maximum { std::max(content_width - viewport_width, 0.0f) };
        float offset { std::clamp(config.scroll_offset, 0.0f, maximum) };
        if (reveal_selected)
        {
            for (std::size_t index { 0 }; index < config.items.size(); ++index)
                if (config.items[index].key == selected_)
                {
                    const float left { static_cast<float>(index) * config.tab_width };
                    const float right { left + config.tab_width + (index + 1 == config.items.size() ? add_width : 0.0f) };
                    offset = std::clamp(std::max(std::min(offset, left), right - viewport_width), 0.0f, maximum);
                    break;
                }
        }
        overflowing_ = maximum > 0.0f;
        const luil::strip_config strip_config { .content_width = content_width, .scroll_offset = offset, .scroll = config.scroll };
        auto lane { std::make_unique<luil::strip_element>(luil::ui_element_id { kind_layout, u8"tabs-lane" }, strip_config) };
        lane_ = lane.get();
        config.scroll_offset = 0.0f;
        config.scroll = {};
        auto bar { std::make_unique<luil::tab_bar_element>(std::move(config)) };
        bar_ = bar.get();
        bar->set_access_name(u8"저장소 탭");
        collect_tabs(*bar, tabs_);
        const luil::stack_config row_config { .direction = luil::stack_direction::row };
        auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"tabs-content" }, row_config) };
        row->add(std::move(bar), tabs_width);
        row->add(std::move(add_button), add_width);
        row->add_flexible_gap();
        lane->set_content(std::move(row));
        add_child(std::move(lane));
        if (overflowing_)
        {
            auto scrollbar { std::make_unique<tab_scrollbar_element>(content_width, viewport_width, offset) };
            scrollbar_ = scrollbar.get();
            add_child(std::move(scrollbar));
        }
    }

    void repository_tabs_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
        const float height { overflowing_ ? scrollbar_height * context.scale : 0.0f };
        lane_->arrange(context.for_child({ context.slot.x, context.slot.y, context.slot.width, std::max(context.slot.height - height, 0.0f) }));
        if (scrollbar_ != nullptr)
            scrollbar_->arrange(context.for_child({ context.slot.x, context.slot.y + context.slot.height - height, context.slot.width, height }));
    }

    void repository_tabs_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        luil::interaction_snapshot normal { interaction };
        if (normal.drag.has_value())
            normal.drag->payload.suppress_drop_highlight = true;
        draw_children(context, normal);
        const float scale { context.scale };
        const luil::rect_f clip { lane_->bounds() };
        context.canvas.save();
        context.canvas.clipRect(SkRect::MakeXYWH(clip.x, clip.y, clip.width, clip.height));
        std::size_t moved { tabs_.size() };
        if (interaction.drag.has_value())
            for (std::size_t index { 0 }; index < tabs_.size(); ++index)
                if (tabs_[index]->id().owner == interaction.drag->payload.dragged_owner)
                    moved = index;
        for (std::size_t index { 0 }; index < tabs_.size(); ++index)
        {
            const luil::ui_element& tab { *tabs_[index] };
            const luil::rect_f box { tab.bounds() };
            const bool selected { tab.id().owner == selected_ };
            SkPaint border { luil::solid_paint(selected ? luil::with_alpha(context.palette.accent, 0.65f) : context.palette.divider) };
            border.setStyle(SkPaint::kStroke_Style);
            border.setStrokeWidth(scale);
            const SkRect shape { SkRect::MakeXYWH(box.x + 2.5f * scale, box.y + 3.5f * scale, std::max(box.width - 5.0f * scale, 0.0f), std::max(box.height - 6.0f * scale, 0.0f)) };
            context.canvas.drawRoundRect(shape, 4.0f * scale, 4.0f * scale, border);
            if (moved == tabs_.size() || moved == index || interaction.drag->hovered_drop_target != tab.id() || interaction.drag->payload.suppress_drop_highlight)
                continue;
            // 순서 변경은 대상 인덱스로 옮긴다. 오른쪽으로 옮기면 대상 뒤, 왼쪽이면 대상 앞이다.
            const bool after { moved < index };
            const float edge { after ? box.x + box.width : box.x };
            const SkPaint glow { luil::solid_paint(luil::with_alpha(context.palette.accent, 0.23f)) };
            context.canvas.drawRect(SkRect::MakeXYWH(after ? edge - 18.0f * scale : edge, box.y + 2.0f * scale, 18.0f * scale, box.height - 4.0f * scale), glow);
            const SkPaint line { luil::solid_paint(context.palette.accent) };
            context.canvas.drawRect(SkRect::MakeXYWH(edge - 1.5f * scale, box.y + 2.0f * scale, 3.0f * scale, box.height - 4.0f * scale), line);
        }
        context.canvas.restore();
        const float reach { std::min(12.0f * scale, clip.width / 2.0f) };
        const auto shadow = [&](const bool left) {
            context.canvas.save();
            context.canvas.translate(left ? clip.x : clip.x + clip.width, clip.y);
            context.canvas.rotate(left ? -90.0f : 90.0f);
            const luil::rect_f band { left ? -clip.height : 0.0f, 0.0f, clip.height, reach };
            luil::draw_downward_shadow(context.canvas, band, context.palette.content_shadow, 0.38f);
            context.canvas.restore();
        };
        if (lane_->scroll_offset() > 0.5f)
            shadow(true);
        if (lane_->scroll_offset() + 0.5f < lane_->maximum_scroll())
            shadow(false);
    }

    void repository_tabs_element::set_tabs_name(std::u8string name)
    {
        bar_->set_access_name(std::move(name));
    }

    float repository_tabs_element::scroll_offset() const noexcept
    {
        return lane_->scroll_offset();
    }
} // namespace cyella::ui
