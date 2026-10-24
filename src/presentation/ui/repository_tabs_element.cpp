#include "presentation/ui/repository_tabs_element.h"

#include "application/repository_tabs.h"
#include "domain/text_catalog.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"

#include "luil/ui/draw_primitives.h"

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
        // 끌리는 탭이 뜨는 둥근 판의 자리다.
        SkRect floating_tab_rect(const luil::rect_f& box, const float scale)
        {
            return SkRect::MakeXYWH(box.x + 2.5f * scale, box.y + 3.5f * scale, std::max(box.width - 5.0f * scale, 0.0f), std::max(box.height - 6.0f * scale, 0.0f));
        }

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

    luil::tab_bar_style repository_tab_style(const tab_shape shape, const tab_marker_place place) noexcept
    {
        luil::tab_bar_style style {};
        style.variant = shape == tab_shape::flat ? luil::tab_bar_variant::flat : luil::tab_bar_variant::connected;
        style.close_visibility = luil::tab_close_visibility::selected_or_hovered;
        style.indicator_placement = place == tab_marker_place::close_slot ? luil::tab_indicator_placement::close_slot : luil::tab_indicator_placement::icon_corner;
        style.bar_background = luil::palette_role::caption_background;
        style.selected_background = luil::palette_role::window_background;
        return style;
    }

    void apply_tab_marker(luil::tab_item& item, const tab_marker marker, const std::uint32_t changed_files, const tab_marker_place place)
    {
        if (marker == tab_marker::none)
            return;
        if (place == tab_marker_place::badge)
        {
            luil::tab_badge badge {};
            switch (marker)
            {
            case tab_marker::changes:
                badge.text = number_text(changed_files);
                badge.tone = luil::badge_tone::neutral;
                badge.description = tr(u8"tabs.state_changes", { { u8"count", changed_files } });
                break;
            case tab_marker::queued:
                badge.text = tr(u8"tabs.badge_queued");
                badge.tone = luil::badge_tone::neutral;
                badge.glyph = codicons::icon_watch;
                badge.description = tr(u8"tabs.state_queued");
                break;
            case tab_marker::running:
                badge.text = tr(u8"tabs.badge_running");
                badge.tone = luil::badge_tone::accent;
                badge.glyph = codicons::icon_sync;
                badge.spinning = true;
                badge.description = tr(u8"tabs.state_running");
                break;
            case tab_marker::needs_input:
                badge.text = tr(u8"tabs.badge_needs_input");
                badge.tone = luil::badge_tone::warning;
                badge.description = tr(u8"tabs.state_needs_input");
                break;
            case tab_marker::none:
                return;
            }
            item.badge = std::move(badge);
            return;
        }

        // 닫기 자리의 바뀜 점은 흐린 색이다. 그 자리는 단추와 번갈아 서므로 눈에 덜 띄어야 하고, 아이콘 모서리의 점은 작아서 강조색이어야 보인다.
        luil::tab_indicator indicator {};
        switch (marker)
        {
        case tab_marker::changes:
            indicator.kind = luil::tab_indicator_kind::dot;
            indicator.tone = place == tab_marker_place::close_slot ? luil::badge_tone::neutral : luil::badge_tone::accent;
            indicator.description = tr(u8"tabs.state_changes", { { u8"count", changed_files } });
            break;
        case tab_marker::queued:
            indicator.kind = luil::tab_indicator_kind::ring;
            indicator.tone = luil::badge_tone::neutral;
            indicator.description = tr(u8"tabs.state_queued");
            break;
        case tab_marker::running:
            indicator.kind = luil::tab_indicator_kind::busy;
            indicator.tone = luil::badge_tone::accent;
            indicator.description = tr(u8"tabs.state_running");
            break;
        case tab_marker::needs_input:
            indicator.kind = luil::tab_indicator_kind::glyph;
            indicator.glyph = codicons::icon_bell;
            indicator.tone = luil::badge_tone::warning;
            indicator.description = tr(u8"tabs.state_needs_input");
            break;
        case tab_marker::none:
            return;
        }
        item.indicator = std::move(indicator);
    }

    repository_tabs_element::repository_tabs_element(luil::tab_bar_config config, place_factory place, const float viewport_width, const bool reveal_selected)
        : ui_element { { kind_layout, u8"repository-tabs" } }
        , selected_ { config.selected }
        , place_ { std::move(place) }
        , stepping_ { config.overflow_mode == luil::tab_overflow_mode::step && config.scroll != nullptr }
        , menu_ { config.overflow != nullptr }
        , trailing_width_ { config.trailing != nullptr ? config.trailing_width : 0.0f }
    {
        if (config.style.has_value())
            bar_background_ = config.style->bar_background;
        // luil 탭은 이 메시지가 있어야 끌기와 놓기를 단다. 놓기는 아래에서 앞·뒤를 가르는 것으로 바꿔 다므로 이 길로는 나가지 않는다.
        if (place_ != nullptr)
            config.reorder = [place = place_](const std::u8string& moved, const std::u8string& target) { return place(moved, target, false); };

        // 고른 탭을 드러낼 스크롤 값은 막대를 짓기 전에 정해야 한다. 막대가 배치에서 쓰는 셈을 그대로 따라간다:
        // 줄여도 넘치면 탭은 최소 폭이고, 띠는 앞·뒤 단추와 넘침·꼬리 단추를 뗀 자리다.
        const std::size_t count { config.items.size() };
        const float smallest { luil::fitted_tab_width(count, config.tab_width, config.min_tab_width, 0.0f) };
        const bool overflowing { static_cast<float>(count) * smallest + trailing_width_ > viewport_width };
        if (overflowing == false)
            config.scroll_offset = 0.0f;
        else
        {
            const float buttons { (menu_ ? luil::tab_overflow_width : 0.0f) + (stepping_ ? luil::tab_overflow_width * 2.0f : 0.0f) };
            const float lane { std::max(viewport_width - trailing_width_ - buttons, 0.0f) };
            const float maximum { std::max(static_cast<float>(count) * smallest - lane, 0.0f) };
            float offset { std::clamp(config.scroll_offset, 0.0f, maximum) };
            if (reveal_selected)
                for (std::size_t index { 0 }; index < count; ++index)
                    if (config.items[index].key == selected_)
                    {
                        const float left { static_cast<float>(index) * smallest };
                        offset = std::clamp(std::max(std::min(offset, left), left + smallest - lane), 0.0f, maximum);
                        break;
                    }
            config.scroll_offset = offset;
        }

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
        add_child(std::move(bar));
    }

    void repository_tabs_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
        bar_->arrange(context.for_child(context.slot));
    }

    luil::rect_f repository_tabs_element::lane_bounds(const float scale) const noexcept
    {
        const luil::rect_f box { bar_->bounds() };
        if (bar_->overflowing() == false)
            return box;
        const float button { luil::tab_overflow_width * scale };
        const float left { box.x + (stepping_ ? button : 0.0f) };
        const float right { box.x + box.width - trailing_width_ * scale - (menu_ ? button : 0.0f) - (stepping_ ? button : 0.0f) };
        return { left, box.y, std::max(right - left, 0.0f), box.height };
    }

    void repository_tabs_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        draw_children(context, interaction);
        const std::optional<std::size_t> moved { interaction.drag.has_value() && tabs_.empty() == false ? index_of(interaction.drag->payload.source) : std::nullopt };
        if (moved.has_value() == false)
        {
            motion_.visible = false;
            return;
        }
        const luil::rect_f clip { lane_bounds(context.scale) };
        context.canvas.save();
        context.canvas.clipRect(SkRect::MakeXYWH(clip.x, clip.y, clip.width, clip.height));
        draw_drop(context, *interaction.drag, *moved);
        context.canvas.restore();
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

        // 끝이 둥근 세로선과 위아래의 작은 점이다. 탭 사이의 틈에 선다.
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
        SkPaint halo { luil::solid_paint(luil::palette_color(context.palette, bar_background_)) };
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
        const luil::rect_f lane { lane_bounds(scale) };
        context.canvas.save();
        context.canvas.clipRect(SkRect::MakeXYWH(lane.x, bounds().y, lane.width, bounds().height));

        // 떠난 자리는 줄 바탕으로 덮어 흐리게 남긴다 — 어디서 왔는지는 보이되 놓을 자리와 헷갈리지 않는다.
        context.canvas.drawRect(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), luil::solid_paint(luil::with_alpha(luil::palette_color(context.palette, bar_background_), 0.6f)));

        // 잡은 지점 그대로 포인터를 따라 줄 위를 미끄러진다. 탭 줄 밖으로는 나가지 않는다.
        const luil::drag_visual& drag { *interaction.drag };
        const float lowest { std::max(lane.x, tabs_.front()->bounds().x) };
        const float highest { std::max(std::min(lane.x + lane.width, tabs_.back()->bounds().x + box.width) - box.width, lowest) };
        const float left { std::clamp(drag.x - drag.payload.grab_offset_x, lowest, highest) };
        const SkRect shape { floating_tab_rect({ left, box.y, box.width, box.height }, scale) };
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
        return bar_->scroll_offset();
    }
} // namespace cyella::ui
