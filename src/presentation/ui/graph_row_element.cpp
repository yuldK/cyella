#include "presentation/ui/graph_row_element.h"

#include "domain/text_catalog.h"
#include "presentation/dialog_intents.h"
#include "presentation/drop_hints.h"
#include "presentation/signing_view.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ref_chip_element.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/effects/SkDashPathEffect.h"

#include <algorithm>
#include <array>

namespace cyella::ui {
    namespace {
        // 라벨 칩의 치수다 (논리 픽셀).
        constexpr float label_height { 18.0f };
        constexpr float label_padding { 6.0f };
        constexpr float label_icon_size { 12.0f };
        constexpr float label_gap { 4.0f };
        constexpr float label_font_size { 11.0f };
        constexpr float label_max_text { 140.0f };
        constexpr float cell_padding { 8.0f };
        // 서명이 있는 커밋의 제목 앞 표지다.
        constexpr float signature_mark_size { 13.0f };
        constexpr float signature_mark_gap { 4.0f };

        char32_t label_glyph(const ref_label& label) noexcept
        {
            switch (label.kind)
            {
            case ref_label_kind::head:
                return codicons::icon_target;
            case ref_label_kind::local:
                return label.current ? codicons::icon_check : codicons::icon_git_branch;
            case ref_label_kind::remote:
                return codicons::icon_cloud;
            case ref_label_kind::tag:
                return codicons::icon_tag;
            case ref_label_kind::stash:
                return codicons::icon_archive;
            }
            return codicons::icon_git_branch;
        }

        float text_width(const luil::draw_context& context, const ref_label& label)
        {
            const SkFont font { ui_font(context, label_font_size, label.current) };
            return std::min(luil::measure_text(label.text, font), label_max_text * context.scale);
        }

        void stroke_curve(SkCanvas& canvas, const graph_curve& curve, const float left, const float top, const float scale, const SkPaint& paint)
        {
            SkPathBuilder builder {};
            builder.moveTo(left + curve.start.x * scale, top + curve.start.y * scale);
            if (curve.straight)
                builder.lineTo(left + curve.end.x * scale, top + curve.end.y * scale);
            else
                builder.cubicTo(left + curve.control1.x * scale, top + curve.control1.y * scale, left + curve.control2.x * scale, top + curve.control2.y * scale, left + curve.end.x * scale,
                    top + curve.end.y * scale);
            canvas.drawPath(builder.detach(), paint);
        }

        SkPaint stroke_paint(const luil::ui_color color, const float width)
        {
            SkPaint paint { luil::solid_paint(color) };
            paint.setAntiAlias(true);
            paint.setStyle(SkPaint::kStroke_Style);
            paint.setStrokeWidth(width);
            return paint;
        }

        void dash(SkPaint& paint, const float on, const float off)
        {
            const std::array<SkScalar, 2> intervals { on, off };
            paint.setPathEffect(SkDashPathEffect::Make(intervals, 0.0f));
        }
    } // namespace

    float estimate_ref_label_strip_width(const std::vector<ref_label>& labels) noexcept
    {
        float width { 0.0f };
        for (const ref_label& label : labels)
            width += estimate_ref_label_width(label) + (width > 0.0f ? label_gap : 0.0f);
        return width;
    }

    graph_columns make_graph_columns(const std::uint16_t lane_count, const bool compact, const float available_width, const float labels_width) noexcept
    {
        graph_columns columns {};
        columns.compact = compact;
        columns.refs = compact ? 110.0f : 160.0f;
        const float minimum_refs { compact ? 72.0f : 112.0f };
        // 칩은 칸 왼쪽 여백(cell_padding) 뒤에서 시작하고 오른쪽에 label_padding을 남긴다.
        if (labels_width >= 0.0f)
            columns.refs = std::clamp(labels_width + cell_padding + label_padding, minimum_refs, columns.refs);
        columns.graph = std::min(graph_column_width(lane_count), graph_columns::maximum_graph);
        if (available_width <= 0.0f)
            return columns;

        // 메시지 열의 하한이다. 넓은 목록에서는 폭의 비율로, 좁은 목록에서도 제목의 앞부분이 읽히는 길이로 둔다.
        const float minimum_summary { std::max(compact ? 140.0f : 220.0f, available_width * 0.34f) };
        const float tail { compact ? 0.0f : columns.author + columns.date };
        float shortage { columns.refs + columns.graph + tail + minimum_summary - available_width };
        const auto shrink = [&shortage](float& width, const float minimum) {
            if (shortage <= 0.0f || width <= minimum)
                return;
            const float give { std::min(shortage, width - minimum) };
            width -= give;
            shortage -= give;
        };
        // 그래프는 레인 셋이 보이는 폭까지만 줄인다. 레인이 그보다 적으면 지금 폭이 곧 하한이다.
        shrink(columns.graph, std::min(columns.graph, graph_column_width(3)));
        shrink(columns.refs, minimum_refs);
        if (compact == false)
        {
            shrink(columns.author, 76.0f);
            shrink(columns.date, 70.0f);
        }
        return columns;
    }

    float measure_ref_label(const luil::draw_context& context, const ref_label& label)
    {
        const float scale { context.scale };
        float width { label_padding * 2.0f * scale + label_icon_size * scale + label_gap * scale + text_width(context, label) };
        if (label.also_remote)
            width += (label_gap + label_icon_size) * scale;
        if (label.worktree_path.empty() == false)
            width += (label_gap + label_icon_size) * scale;
        return width;
    }

    void draw_ref_label(luil::draw_context& context, const ref_label& label, const luil::rect_f& box, const luil::ui_color lane)
    {
        const float scale { context.scale };
        const luil::ui_color_palette& palette { context.palette };
        luil::ui_color fill { luil::with_alpha(lane, 0.25f) };
        luil::ui_color border { lane };
        luil::ui_color text { palette.primary_foreground };
        switch (label.kind)
        {
        case ref_label_kind::remote:
            fill = luil::with_alpha(lane, 0.08f);
            border = luil::with_alpha(lane, 0.45f);
            text = palette.secondary_foreground;
            break;
        case ref_label_kind::tag:
            fill = luil::with_alpha(lane, 0.12f);
            border = luil::with_alpha(lane, 0.7f);
            break;
        case ref_label_kind::stash:
            fill = palette.input_background;
            border = palette.group_border;
            text = palette.secondary_foreground;
            break;
        case ref_label_kind::head:
            fill = palette.soft_button_background;
            border = palette.accent;
            break;
        case ref_label_kind::local:
            break;
        }
        const float radius { context.metrics.control_corner_radius * scale };
        const SkRect shape { SkRect::MakeXYWH(box.x, box.y, box.width, box.height) };
        SkPaint fill_paint { luil::solid_paint(fill) };
        fill_paint.setAntiAlias(true);
        context.canvas.drawRRect(SkRRect::MakeRectXY(shape, radius, radius), fill_paint);
        SkPaint border_paint { stroke_paint(border, scale) };
        // 원격에 없는 태그는 점선 테두리다.
        if (label.local_only)
            dash(border_paint, 3.0f * scale, 2.0f * scale);
        context.canvas.drawRRect(SkRRect::MakeRectXY(shape.makeInset(0.5f * scale, 0.5f * scale), radius, radius), border_paint);

        context.canvas.save();
        context.canvas.clipRect(shape);
        const SkPaint text_paint { luil::solid_paint(text) };
        float x { box.x + label_padding * scale };
        luil::draw_centered_glyph(context.canvas, label_glyph(label), { x, box.y, label_icon_size * scale, box.height }, icon_font(context, label_icon_size), text_paint);
        x += (label_icon_size + label_gap) * scale;
        const SkFont font { ui_font(context, label_font_size, label.current) };
        const float marker_width { label.worktree_path.empty() ? 0.0f : (label_gap + label_icon_size) * scale };
        const float trailing { (label.also_remote ? (label_gap + label_icon_size) * scale : 0.0f) + marker_width };
        const float available { box.x + box.width - label_padding * scale - trailing - x };
        if (available > 0.0f)
            static_cast<void>(luil::draw_text_within(context.canvas, label.text, x, box.y + luil::centered_text_baseline(font, box.height), available, font, text_paint));
        if (label.also_remote)
        {
            const float cloud_x { box.x + box.width - label_padding * scale - label_icon_size * scale - marker_width };
            luil::draw_centered_glyph(
                context.canvas, codicons::icon_cloud, { cloud_x, box.y, label_icon_size * scale, box.height }, icon_font(context, label_icon_size), luil::solid_paint(palette.secondary_foreground));
        }
        if (label.worktree_path.empty() == false)
        {
            // 다른 worktree가 이 브랜치를 체크아웃하고 있다.
            const float marker_x { box.x + box.width - label_padding * scale - label_icon_size * scale };
            luil::draw_centered_glyph(
                context.canvas, codicons::icon_worktree, { marker_x, box.y, label_icon_size * scale, box.height }, icon_font(context, label_icon_size), luil::solid_paint(palette.accent));
        }
        context.canvas.restore();
    }

    // ---------------------------------------------------------------- graph_row_element

    graph_row_element::graph_row_element(std::u8string owner, graph_row_data data, graph_columns columns)
        : press_row_element { luil::ui_element_id { kind_graph_row, std::move(owner) } }
        , data_ { std::move(data) }
        , columns_ { columns }
    {
        // 행 자리의 누름은 이 element가 직접 받는다 (아래). 라벨 칩은 자기 자리를 따로 누른다.
        access_name_ = data_.wip ? tr(u8"graph.wip_access_name", { { u8"count", data_.wip_count } }) : data_.subject;
        if (data_.labels.empty() == false)
        {
            std::u8string names {};
            for (const ref_label& label : data_.labels)
                names += (names.empty() ? u8"" : u8", ") + label.text;
            access_name_ += u8" [" + names + u8"]";
        }
        set_tab_stop(false);
        set_focus_owner(data_.list);
        // 목록 행의 고르기를 가로채지 않도록 같은 고르기 메시지를 누른 순간 낸다 (`press_row_element`).
        // Ctrl·Shift는 여러 커밋 고르기다. 목록의 `pick`이 받는 수식 키를 이 element가 대신 싣는다.
        set_press([key = data_.row_key](const luil::ui_action_context& context) -> std::vector<luil::input_action> {
            return { message(select_commit_intent { .key = key, .control = context.control, .shift = context.shift }) };
        });
        if (data_.wip)
            return;

        set_action(luil::ui_trigger::double_click, action_of(message(activate_commit_intent { data_.sha })));
        menu_target target {};
        target.kind = menu_target_kind::commit;
        target.sha = data_.sha;
        // 고르지 않은 행을 우클릭하면 그 행만 고른 뒤 메뉴를 연다. 고른 행(비교 중인 커밋 포함)이면 선택을 둔다.
        set_action(luil::ui_trigger::right_click, [target, key = data_.row_key, selected = data_.selected](const luil::ui_action_context& context) -> std::vector<luil::input_action> {
            std::vector<luil::input_action> actions {};
            if (selected == false)
                actions.push_back(message(select_commit_intent { .key = key }));
            actions.push_back(message(open_context_menu_intent { target, context.x, context.y }));
            return actions;
        });

        // 커밋 행은 브랜치를 놓을 자리다 ("이 커밋에서 브랜치 만들기", 현재 브랜치면 reset).
        luil::drop_target drop {};
        drop.accepts = [](const luil::drag_payload& payload) { return parse_branch_drag_key(payload.dragged_owner).has_value(); };
        drop.on_drop = [sha = data_.sha](const luil::drag_payload& payload, const luil::ui_action_context& context) -> std::vector<luil::input_action> {
            const std::optional<drop_party> source { parse_branch_drag_key(payload.dragged_owner) };
            if (source.has_value() == false)
                return {};
            drop_party destination {};
            destination.is_commit = true;
            destination.sha = sha;
            return { message(drop_intent { *source, destination, context.x, context.y }) };
        };
        set_drop_target(std::move(drop));

        for (const ref_label& label : data_.labels)
        {
            auto chip { std::make_unique<ref_chip_element>(label, data_.row_key, data_.sha, data_.row.color, data_.selected) };
            chip->set_focus_owner(data_.list);
            chips_.push_back(static_cast<ref_chip_element*>(add_chip(std::move(chip))));
        }
    }

    luil::ui_element* graph_row_element::add_chip(std::unique_ptr<luil::ui_element> chip)
    {
        luil::ui_element* const raw { chip.get() };
        add_child(std::move(chip));
        return raw;
    }

    void graph_row_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
        if (chips_.empty())
            return;
        const luil::rect_f box { bounds() };
        const float scale { context.scale > 0.0f ? context.scale : 1.0f };
        const float left { box.x + cell_padding * scale };
        const float right { box.x + columns_.refs * scale - label_padding * scale };
        const float available { right - left };
        const float gap { label_gap * scale };
        const float height { label_height * scale };
        const float top { box.y + (box.height - height) / 2.0f };

        std::vector<float> widths(chips_.size(), 0.0f);
        float total { 0.0f };
        for (std::size_t index { 0 }; index < chips_.size(); ++index)
        {
            widths[index] = estimate_ref_label_width(data_.labels[index]) * scale;
            total += widths[index] + (index > 0 ? gap : 0.0f);
        }

        std::size_t shown { chips_.size() };
        float overflow_width { 0.0f };
        if (total > available)
        {
            // 칸이 모자라면 앞에서부터 들어가는 만큼만 두고 나머지는 "+N" 칩으로 줄인다.
            shown = 0;
            float used { 0.0f };
            for (std::size_t index { 0 }; index < widths.size(); ++index)
            {
                const std::size_t rest { widths.size() - index - 1 };
                const float rest_chip { rest > 0 ? (estimate_text_width(u8"+" + number_text(rest), label_font_size) + 10.0f) * scale + gap : 0.0f };
                const float next { used + (index > 0 ? gap : 0.0f) + widths[index] };
                if (next + rest_chip > available && index > 0)
                    break;
                used = next;
                shown = index + 1;
            }
            const std::size_t hidden { chips_.size() - shown };
            if (hidden > 0)
            {
                overflow_width = (estimate_text_width(u8"+" + number_text(hidden), label_font_size) + 10.0f) * scale;
                if (overflow_ == nullptr)
                    overflow_ = add_chip(std::make_unique<ref_overflow_element>(u8"overflow-" + data_.row_key, hidden, data_.sha));
            }
            total = used + (overflow_width > 0.0f ? gap + overflow_width : 0.0f);
        }

        float x { std::max(right - total, left) };
        for (std::size_t index { 0 }; index < chips_.size(); ++index)
        {
            ref_chip_element* const chip { chips_[index] };
            chip->set_visible(index < shown);
            if (index >= shown)
            {
                chip->arrange(context.for_child({ box.x, top, 0.0f, 0.0f }));
                continue;
            }
            const float width { std::max(std::min(widths[index], right - x - (overflow_width > 0.0f ? overflow_width + gap : 0.0f)), 0.0f) };
            chip->arrange(context.for_child({ x, top, width, height }));
            x += width + gap;
        }
        if (overflow_ != nullptr)
        {
            overflow_->set_visible(overflow_width > 0.0f);
            overflow_->arrange(context.for_child({ x, top, overflow_width, height }));
        }
    }

    luil::access_info graph_row_element::accessibility() const
    {
        return { .role = luil::access_role::static_text, .name = access_name_ };
    }

    void graph_row_element::draw_graph(luil::draw_context& context, const luil::rect_f& cell) const
    {
        const float scale { context.scale };
        context.canvas.save();
        context.canvas.clipRect(SkRect::MakeXYWH(cell.x, cell.y, cell.width, cell.height));
        const float line_width { graph_line_width * scale };
        for (const graph_edge& edge : data_.row.edges)
        {
            SkPaint paint { stroke_paint(lane_color(context.palette, edge.color), line_width) };
            if (data_.has_wip && graph_edge_is_wip_chain(edge, data_.index, data_.wip_lane, data_.head_row_index))
                dash(paint, 3.0f * scale, 3.0f * scale);
            stroke_curve(context.canvas, graph_edge_curve(edge, cell.height / scale), cell.x, cell.y, scale, paint);
        }

        const float center_x { cell.x + graph_lane_x(data_.row.lane) * scale };
        const float center_y { cell.y + cell.height / 2.0f };
        const luil::ui_color color { lane_color(context.palette, data_.row.color) };
        if (graph_row_needs_upward_stub(data_.row, data_.wip, data_.labels.empty() == false))
        {
            // 위에 아직 읽지 않은 자식이 있다는 짧은 선이다.
            SkPaint stub { stroke_paint(luil::with_alpha(color, 0.5f), line_width) };
            dash(stub, 2.0f * scale, 2.0f * scale);
            context.canvas.drawLine(center_x, cell.y, center_x, center_y, stub);
        }

        if (data_.wip)
        {
            SkPaint fill { luil::solid_paint(context.palette.surface_background) };
            fill.setAntiAlias(true);
            context.canvas.drawCircle(center_x, center_y, graph_node_radius * scale, fill);
            SkPaint ring { stroke_paint(color, line_width) };
            dash(ring, 3.0f * scale, 2.0f * scale);
            context.canvas.drawCircle(center_x, center_y, graph_node_radius * scale, ring);
        }
        else
        {
            if (data_.head)
                context.canvas.drawCircle(center_x, center_y, graph_head_ring_radius * scale, stroke_paint(color, 1.5f * scale));
            if (data_.row.is_merge)
            {
                // merge 커밋은 속이 빈 작은 원이다.
                SkPaint fill { luil::solid_paint(context.palette.window_background) };
                fill.setAntiAlias(true);
                context.canvas.drawCircle(center_x, center_y, graph_merge_node_radius * scale, fill);
                context.canvas.drawCircle(center_x, center_y, graph_merge_node_radius * scale, stroke_paint(color, line_width));
            }
            else
            {
                // 노드 둘레를 바탕색으로 둘러 지나가는 선과 노드를 가른다 (SVG의 바탕색 stroke).
                SkPaint halo { luil::solid_paint(context.palette.surface_background) };
                halo.setAntiAlias(true);
                context.canvas.drawCircle(center_x, center_y, (graph_node_radius + 1.0f) * scale, halo);
                SkPaint fill { luil::solid_paint(color) };
                fill.setAntiAlias(true);
                context.canvas.drawCircle(center_x, center_y, (graph_node_radius - 1.0f) * scale, fill);
            }
        }
        context.canvas.restore();
    }

    void graph_row_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        const luil::rect_f box { bounds() };
        const float scale { context.scale };
        const float refs { columns_.refs * scale };
        const float graph { columns_.graph * scale };
        const float tail { columns_.compact ? 0.0f : (columns_.author + columns_.date) * scale };
        const float summary { std::max(box.width - refs - graph - tail, 0.0f) };

        float x { box.x + refs };
        draw_graph(context, { x, box.y, graph, box.height });
        x += graph;

        const float padding { cell_padding * scale };
        const SkFont body { ui_font(context, 12.0f) };
        const float baseline { box.y + luil::centered_text_baseline(body, box.height) };
        if (data_.wip)
        {
            const SkFont code { code_font(context, 12.0f) };
            const float drawn = luil::draw_text_within(context.canvas, u8"// WIP", x + padding, box.y + luil::centered_text_baseline(code, box.height), summary - padding * 2.0f, code,
                luil::solid_paint(context.palette.secondary_foreground));
            const SkFont small { ui_font(context, 11.0f) };
            const float count_x { x + padding + drawn + padding };
            if (summary - (count_x - x) > 0.0f)
                static_cast<void>(luil::draw_text_within(context.canvas, tr(u8"graph.wip_change_count", { { u8"count", data_.wip_count } }), count_x,
                    box.y + luil::centered_text_baseline(small, box.height), x + summary - padding - count_x, small, luil::solid_paint(context.palette.warning_accent)));
        }
        else if (summary > padding * 2.0f)
        {
            float text_x { x + padding };
            float text_width { summary - padding * 2.0f };
            // 서명이 있는 커밋은 제목 앞에 작은 표지를 단다: 확인됨은 채운 방패, 그 밖은 방패에 물음표 느낌의 빈 표지다.
            if (data_.signature_known && is_signed(data_.signature) && text_width > (signature_mark_size + signature_mark_gap) * scale * 2.0f)
            {
                const bool verified { data_.signature == signature_status::good };
                luil::ui_color mark_color { context.palette.secondary_foreground };
                switch (signature_tone_of(data_.signature))
                {
                case signature_tone::good:
                    mark_color = success_color(context.palette);
                    break;
                case signature_tone::warning:
                    mark_color = context.palette.warning_accent;
                    break;
                case signature_tone::bad:
                    mark_color = context.palette.error_accent;
                    break;
                case signature_tone::neutral:
                    break;
                }
                luil::draw_centered_glyph(context.canvas, verified ? codicons::icon_verified_filled : codicons::icon_unverified, { text_x, box.y, signature_mark_size * scale, box.height },
                    icon_font(context, signature_mark_size), luil::solid_paint(mark_color));
                text_x += (signature_mark_size + signature_mark_gap) * scale;
                text_width -= (signature_mark_size + signature_mark_gap) * scale;
            }
            static_cast<void>(luil::draw_text_within(context.canvas, data_.subject, text_x, baseline, text_width, body, luil::solid_paint(context.palette.primary_foreground)));
        }
        x += summary;
        draw_children(context, interaction);
        if (columns_.compact || data_.wip)
            return;

        const SkFont small { ui_font(context, 11.0f) };
        const SkPaint dim { luil::solid_paint(context.palette.secondary_foreground) };
        const float small_baseline { box.y + luil::centered_text_baseline(small, box.height) };
        static_cast<void>(luil::draw_text_within(context.canvas, data_.author, x + padding, small_baseline, columns_.author * scale - padding * 2.0f, small, dim));
        x += columns_.author * scale;
        static_cast<void>(luil::draw_text_within(context.canvas, data_.date, x + padding, small_baseline, columns_.date * scale - padding * 2.0f, small, dim));
    }

    // ---------------------------------------------------------------- graph_header_element

    graph_header_element::graph_header_element(const graph_columns columns)
        : ui_element { luil::ui_element_id { kind_graph_header, u8"graph-header" } }
        , columns_ { columns }
    {
        set_hit_opaque(false);
    }

    void graph_header_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    void graph_header_element::draw(luil::draw_context& context, const luil::interaction_snapshot&) const
    {
        const luil::rect_f box { bounds() };
        const float scale { context.scale };
        context.canvas.drawRect(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), luil::solid_paint(context.palette.surface_background));
        context.canvas.drawRect(SkRect::MakeXYWH(box.x, box.y + box.height - scale, box.width, scale), luil::solid_paint(context.palette.divider));
        const SkFont font { ui_font(context, 11.0f) };
        const SkPaint dim { luil::solid_paint(context.palette.secondary_foreground) };
        const float baseline { box.y + luil::centered_text_baseline(font, box.height) };
        const float padding { cell_padding * scale };
        const float tail { columns_.compact ? 0.0f : (columns_.author + columns_.date) * scale };
        const float summary { std::max(box.width - (columns_.refs + columns_.graph) * scale - tail, 0.0f) };
        float x { box.x };
        const auto column = [&](const std::u8string_view title, const float width) {
            if (width > padding * 2.0f)
                static_cast<void>(luil::draw_text_within(context.canvas, title, x + padding, baseline, width - padding * 2.0f, font, dim));
            x += width;
        };
        column(tr(u8"graph.column_refs"), columns_.refs * scale);
        const float graph_width { columns_.graph * scale };
        const std::u8string graph_title { tr(u8"graph.column_graph") };
        if (graph_width >= luil::measure_text(graph_title, font) + padding * 2.0f)
            column(graph_title, graph_width);
        else
        {
            luil::draw_centered_glyph(context.canvas, codicons::icon_git_commit, { x, box.y, graph_width, box.height }, icon_font(context, 13.0f), dim);
            x += graph_width;
        }
        column(tr(u8"graph.column_message"), summary);
        if (columns_.compact == false)
        {
            column(tr(u8"graph.column_author"), columns_.author * scale);
            column(tr(u8"graph.column_date"), columns_.date * scale);
        }
    }

    // ---------------------------------------------------------------- ref_label_strip_element

    ref_label_strip_element::ref_label_strip_element(std::u8string owner, std::vector<ref_label> labels, const std::uint8_t lane_color)
        : ui_element { luil::ui_element_id { kind_label, std::move(owner) } }
        , labels_ { std::move(labels) }
        , lane_color_ { lane_color }
    {
        set_hit_opaque(false);
    }

    void ref_label_strip_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    void ref_label_strip_element::draw(luil::draw_context& context, const luil::interaction_snapshot&) const
    {
        const luil::rect_f box { bounds() };
        const float scale { context.scale };
        const float height { label_height * scale };
        const float top { box.y + (box.height - height) / 2.0f };
        float x { box.x };
        const luil::ui_color lane { lane_color(context.palette, lane_color_) };
        for (const ref_label& label : labels_)
        {
            const float width { std::min(measure_ref_label(context, label), box.x + box.width - x) };
            if (width <= 0.0f)
                break;
            draw_ref_label(context, label, { x, top, width, height }, lane);
            x += width + label_gap * scale;
        }
    }

    luil::access_info ref_label_strip_element::accessibility() const
    {
        std::u8string names {};
        for (const ref_label& label : labels_)
            names += (names.empty() ? u8"" : u8", ") + label.text;
        return { .role = luil::access_role::static_text, .name = names };
    }
} // namespace cyella::ui
