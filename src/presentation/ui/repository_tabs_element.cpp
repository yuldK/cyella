#include "presentation/ui/repository_tabs_element.h"

#include "application/repository_tabs.h"
#include "domain/text_catalog.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"

#include "luil/ui/draw_primitives.h"
#include "luil/ui/stack_element.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <utility>

namespace cyella::ui {
    namespace {
        constexpr float add_width { 32.0f };
        constexpr float scrollbar_height { 10.0f };
        // 탭마다 그리는 둥근 테두리의 자리다. 끌리는 탭도 같은 모양으로 뜬다.
        SkRect tab_shape(const luil::rect_f& box, const float scale)
        {
            return SkRect::MakeXYWH(box.x + 2.5f * scale, box.y + 3.5f * scale, std::max(box.width - 5.0f * scale, 0.0f), std::max(box.height - 6.0f * scale, 0.0f));
        }

        class tab_scrollbar_element final : public luil::ui_element
        {
        public:
            tab_scrollbar_element(const float content, const float viewport, const float offset)
                : ui_element { { luil::ui_element_kind::scroll_area_bar, u8"tabs-scroll" } }
                , content_ { content }
                , viewport_ { viewport }
                , offset_ { offset }
            {
                set_access_name(tr(u8"tabs.horizontal_scroll"));
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

        void collect_tabs(luil::ui_element& element, std::vector<luil::ui_element*>& tabs)
        {
            if (element.id().kind == luil::ui_element_kind::tab)
            {
                tabs.push_back(&element);
                const std::u8string name { tr(u8"tabs.close_tab", { { u8"name", element.accessibility().name } }) };
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

    repository_tabs_element::repository_tabs_element(
        luil::tab_bar_config config, place_factory place, std::unique_ptr<luil::ui_element> add_button, const float viewport_width, const bool reveal_selected)
        : ui_element { { kind_layout, u8"repository-tabs" } }
        , selected_ { config.selected }
        , place_ { std::move(place) }
    {
        // luil 탭은 이 메시지가 있어야 끌기와 놓기를 단다. 놓기는 아래에서 앞·뒤를 가르는 것으로 바꿔 다므로 이 길로는 나가지 않는다.
        if (place_ != nullptr)
            config.reorder = [place = place_](const std::u8string& moved, const std::u8string& target) { return place(moved, target, false); };
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
        bar->set_access_name(tr(u8"tabs.repository_tabs"));
        collect_tabs(*bar, tabs_);
        for (luil::ui_element* const tab : tabs_)
        {
            if (const luil::drag_source* const source { tab->drag() }; source != nullptr)
            {
                // 끌리는 탭과 끼울 틈은 이 줄이 그린다. 트리의 기본 상자와 대상 테두리, 탭의 대상 배경은 끈다.
                luil::drag_source own {};
                own.make_payload = [make = source->make_payload](const luil::ui_action_context& context) -> luil::drag_payload {
                    luil::drag_payload payload { make(context) };
                    payload.custom_visual = true;
                    payload.suppress_drop_highlight = true;
                    return payload;
                };
                tab->set_drag_source(std::move(own));
            }
            if (const luil::drop_target* const drop { tab->drop() }; drop != nullptr)
            {
                luil::drop_target own { .accepts = drop->accepts };
                own.on_drop = [this, target = tab->id()](const luil::drag_payload& payload, const luil::ui_action_context& context) -> std::vector<luil::input_action> {
                    const std::optional<std::size_t> moved { index_of(payload.source) };
                    const std::optional<std::size_t> over { index_of(target) };
                    if (moved.has_value() == false || over.has_value() == false)
                        return {};
                    const bool after { lands_after(*over, context.x) };
                    if (insertion_index(*moved, *over, after).has_value() == false)
                        return {};
                    return { place_(payload.dragged_owner, target.owner, after) };
                };
                tab->set_drop_target(std::move(own));
            }
        }
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
        draw_children(context, interaction);
        const float scale { context.scale };
        const luil::rect_f clip { lane_->bounds() };
        context.canvas.save();
        context.canvas.clipRect(SkRect::MakeXYWH(clip.x, clip.y, clip.width, clip.height));
        for (const luil::ui_element* const tab : tabs_)
        {
            const bool selected { tab->id().owner == selected_ };
            SkPaint border { luil::solid_paint(selected ? luil::with_alpha(context.palette.accent, 0.65f) : context.palette.divider) };
            border.setStyle(SkPaint::kStroke_Style);
            border.setStrokeWidth(scale);
            context.canvas.drawRoundRect(tab_shape(tab->bounds(), scale), 4.0f * scale, 4.0f * scale, border);
        }
        const std::optional<std::size_t> moved { interaction.drag.has_value() ? index_of(interaction.drag->payload.source) : std::nullopt };
        if (moved.has_value())
            draw_drop(context, *interaction.drag, *moved);
        else
            motion_.visible = false;
        context.canvas.restore();
        const float reach { std::min(12.0f * scale, clip.width / 2.0f) };
        const auto shadow = [&](const bool left, const float strength) {
            context.canvas.save();
            context.canvas.translate(left ? clip.x : clip.x + clip.width, clip.y);
            context.canvas.rotate(left ? -90.0f : 90.0f);
            const luil::rect_f band { left ? -clip.height : 0.0f, 0.0f, clip.height, reach };
            luil::draw_downward_shadow(context.canvas, band, context.palette.content_shadow, 0.38f * strength);
            context.canvas.restore();
        };
        // 흘리는 창의 위·아래 그림자와 같은 곡선이다: 끝에서 멀어진 만큼 서서히 짙어져, 흘리기 시작한 순간 툭 켜지지 않는다.
        const float left { luil::scroll_edge_strength(lane_->scroll_offset()) };
        const float right { luil::scroll_edge_strength(lane_->maximum_scroll() - lane_->scroll_offset()) };
        if (left > 0.0f)
            shadow(true, left);
        if (right > 0.0f)
            shadow(false, right);
        // 끌리는 탭은 가장자리 그림자보다 위에 뜬다.
        if (moved.has_value())
            draw_dragged(context, interaction, *moved);
    }

    std::optional<std::chrono::steady_clock::time_point> repository_tabs_element::next_update(const luil::update_context& context, const luil::interaction_snapshot& interaction) const
    {
        if (interaction.drag.has_value() == false || motion_.visible == false)
            return std::nullopt;
        if (luil::transition_finished(motion_.caret, context.now) && luil::transition_finished(motion_.wash, context.now))
            return std::nullopt;
        return context.now + luil::transition_frame;
    }

    std::optional<std::size_t> repository_tabs_element::index_of(const luil::ui_element_id& id) const noexcept
    {
        for (std::size_t index { 0 }; index < tabs_.size(); ++index)
            if (tabs_[index]->id() == id)
                return index;
        return std::nullopt;
    }

    bool repository_tabs_element::lands_after(const std::size_t target, const float x) const noexcept
    {
        const luil::rect_f box { tabs_[target]->bounds() };
        return x >= box.x + box.width / 2.0f;
    }

    void repository_tabs_element::draw_drop(luil::draw_context& context, const luil::drag_visual& drag, const std::size_t moved) const
    {
        // 틈과 반쪽 강조의 자리를 탭 폭 단위로 구한다. 순서가 그대로인 자리에는 표시가 없다.
        std::optional<float> caret {};
        float wash { 0.0f };
        if (const std::optional<std::size_t> over { index_of(drag.hovered_drop_target) }; over.has_value())
        {
            const bool after { lands_after(*over, drag.x) };
            if (insertion_index(moved, *over, after).has_value())
            {
                caret = static_cast<float>(*over) + (after ? 1.0f : 0.0f);
                wash = static_cast<float>(*over) + (after ? 0.75f : 0.25f);
            }
        }
        if (caret.has_value() == false)
        {
            motion_.visible = false;
            return;
        }

        const auto now { context.now };
        // 처음 뜰 때는 그 자리에 바로 서고, 그 뒤로 자리가 바뀌면 지금 보이는 곳에서 미끄러져 간다.
        //  - 나타날 때 흐리게 시작하지 않는다. 넘친 줄의 자동 스크롤은 틱마다 tree를 다시 지어 이 상태를 지우므로,
        //    흐려짐이 있으면 스크롤하는 동안 표시가 내내 옅다.
        if (motion_.visible == false)
            motion_ = { .visible = true, .caret = { *caret, *caret, now }, .wash = { wash, wash, now } };
        else if (motion_.caret.to != *caret)
        {
            motion_.caret = { luil::transition_value(motion_.caret, now), *caret, now, luil::transition_duration };
            motion_.wash = { luil::transition_value(motion_.wash, now), wash, now, luil::transition_duration };
        }

        const float scale { context.scale };
        const luil::rect_f first { tabs_.front()->bounds() };
        const float wash_center { first.x + luil::transition_value(motion_.wash, now) * first.width };
        const float wash_width { std::max(first.width / 2.0f - 6.0f * scale, 0.0f) };
        const SkRect half { SkRect::MakeXYWH(wash_center - wash_width / 2.0f, first.y + 5.5f * scale, wash_width, std::max(first.height - 10.0f * scale, 0.0f)) };
        context.canvas.drawRoundRect(half, 4.0f * scale, 4.0f * scale, luil::solid_paint(luil::with_alpha(context.palette.accent, 0.16f)));
    }

    void repository_tabs_element::draw_caret(luil::draw_context& context) const
    {
        if (motion_.visible == false)
            return;

        // 끝이 둥근 세로선과 위아래의 작은 점이다. 탭 테두리 사이의 틈에 선다.
        const auto now { context.now };
        const float scale { context.scale };
        const luil::rect_f first { tabs_.front()->bounds() };
        const float x { first.x + luil::transition_value(motion_.caret, now) * first.width };
        const float top { first.y + 6.0f * scale };
        const float bottom { first.y + first.height - 6.0f * scale };
        const auto mark = [&](const float spread, const SkPaint& paint) {
            const float half { scale + spread };
            context.canvas.drawRoundRect(SkRect::MakeLTRB(x - half, top, x + half, bottom), half, half, paint);
            context.canvas.drawCircle(x, top, 2.75f * scale + spread, paint);
            context.canvas.drawCircle(x, bottom, 2.75f * scale + spread, paint);
        };
        // 끌리는 탭의 글자를 가로지를 때 섞여 보이지 않게 줄 바탕색 테를 먼저 두른다.
        SkPaint halo { luil::solid_paint(context.palette.window_background) };
        halo.setAntiAlias(true);
        mark(1.5f * scale, halo);
        SkPaint line { luil::solid_paint(context.palette.accent) };
        line.setAntiAlias(true);
        mark(0.0f, line);
    }

    void repository_tabs_element::draw_dragged(luil::draw_context& context, const luil::interaction_snapshot& interaction, const std::size_t moved) const
    {
        const float scale { context.scale };
        const luil::ui_element& tab { *tabs_[moved] };
        const luil::rect_f box { tab.bounds() };
        const luil::rect_f lane { lane_->bounds() };
        context.canvas.save();
        context.canvas.clipRect(SkRect::MakeXYWH(lane.x, bounds().y, lane.width, bounds().height));

        // 떠난 자리는 줄 바탕으로 덮어 흐리게 남긴다 — 어디서 왔는지는 보이되 놓을 자리와 헷갈리지 않는다.
        context.canvas.drawRect(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), luil::solid_paint(luil::with_alpha(context.palette.window_background, 0.6f)));

        // 잡은 지점 그대로 포인터를 따라 줄 위를 미끄러진다. 탭 줄 밖으로는 나가지 않는다.
        const luil::drag_visual& drag { *interaction.drag };
        const float lowest { std::max(lane.x, tabs_.front()->bounds().x) };
        const float highest { std::max(std::min(lane.x + lane.width, tabs_.back()->bounds().x + box.width) - box.width, lowest) };
        const float left { std::clamp(drag.x - drag.payload.grab_offset_x, lowest, highest) };
        const SkRect shape { tab_shape({ left, box.y, box.width, box.height }, scale) };
        const float radius { 5.0f * scale };
        luil::draw_surface_shadow(context, { shape.x(), shape.y(), shape.width(), shape.height() }, radius, 0.45f);
        const SkRRect rounded { SkRRect::MakeRectXY(shape, radius, radius) };
        SkPaint fill { luil::solid_paint(context.palette.surface_background) };
        fill.setAntiAlias(true);
        context.canvas.drawRRect(rounded, fill);

        // 탭의 그림(아이콘·이름·닫기 단추)을 그대로 옮겨 그린다. 끄는 중·hover 상태 없이 가만히 있는 모양이다.
        luil::interaction_snapshot still { interaction };
        still.drag.reset();
        still.hovered = {};
        still.pressed = {};
        context.canvas.save();
        context.canvas.clipRRect(rounded, true);
        context.canvas.translate(left - box.x, 0.0f);
        tab.draw(context, still);
        context.canvas.restore();

        SkPaint border { luil::solid_paint(luil::with_alpha(context.palette.accent, 0.65f)) };
        border.setStyle(SkPaint::kStroke_Style);
        border.setStrokeWidth(scale);
        border.setAntiAlias(true);
        context.canvas.drawRRect(rounded, border);
        // 잡은 지점이 탭 가운데쯤이라 끌리는 탭이 대개 틈을 덮는다. 놓을 자리가 가장 중요한 표시라 그 위에 긋는다.
        draw_caret(context);
        context.canvas.restore();
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
