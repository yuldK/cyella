#include "presentation/ui/rebase/multiline_text_element.h"

#include "luil/ui/draw_primitives.h"
#include "luil/ui/font_scale.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"

#include <algorithm>
#include <functional>

namespace cyella::ui {
    namespace {
        constexpr float logical_font_size { 11.5f };
        constexpr float line_spacing { 1.5f };
        constexpr float horizontal_padding { 7.0f };
        constexpr float vertical_padding { 5.0f };

        // 줄 안에서 x에 가장 가까운 글자 경계다. `measure`는 줄 앞에서부터의 폭이다.
        std::size_t nearest_boundary(const std::u8string_view line, const float target, const std::function<float(std::u8string_view)>& measure)
        {
            if (target <= 0.0f || line.empty())
                return 0;
            std::vector<std::size_t> boundaries { 0 };
            for (std::size_t offset { 1 }; offset < line.size(); ++offset)
                if ((static_cast<unsigned>(line[offset]) & 0xC0u) != 0x80u)
                    boundaries.push_back(offset);
            boundaries.push_back(line.size());
            const auto narrower = [&](const float position, const std::size_t offset) { return position < measure(line.substr(0, offset)); };
            const auto after { std::upper_bound(boundaries.begin(), boundaries.end(), target, narrower) };
            if (after == boundaries.end())
                return line.size();
            if (after == boundaries.begin())
                return *after;
            const std::size_t before { *(after - 1) };
            return target - measure(line.substr(0, before)) < measure(line.substr(0, *after)) - target ? before : *after;
        }
    } // namespace

    multiline_text_element::multiline_text_element(luil::ui_element_id id, luil::text_input_view value, std::u8string placeholder, std::u8string access_name)
        : ui_element { std::move(id) }
        , value_ { std::move(value) }
        , placeholder_ { std::move(placeholder) }
        , access_name_ { std::move(access_name) }
    {
        // 누름·끌기·더블 클릭은 interaction controller가 직접 다룬다. 여기서는 hit test 대상이 되게만 한다.
        set_action(luil::ui_trigger::left_click, [](const luil::ui_action_context&) -> std::vector<luil::input_action> { return {}; });
        set_cursor(luil::ui_cursor::text);
    }

    std::vector<multiline_text_element::line_range> multiline_text_element::split_lines(const std::u8string_view text)
    {
        std::vector<line_range> lines {};
        std::size_t begin { 0 };
        while (true)
        {
            const std::size_t end { text.find(u8'\n', begin) };
            if (end == std::u8string_view::npos)
            {
                lines.emplace_back(begin, text.size());
                return lines;
            }
            lines.emplace_back(begin, end);
            begin = end + 1;
        }
    }

    std::size_t multiline_text_element::line_of(const std::vector<line_range>& lines, const std::size_t offset) noexcept
    {
        for (std::size_t index { 0 }; index < lines.size(); ++index)
            if (offset <= lines[index].second)
                return index;
        return lines.empty() ? 0 : lines.size() - 1;
    }

    std::u8string_view multiline_text_element::displayed_text() const noexcept
    {
        return value_.displayed_text();
    }

    std::size_t multiline_text_element::displayed_caret() const noexcept
    {
        const std::u8string_view text { displayed_text() };
        const std::size_t caret { value_.composing ? value_.composition_caret : value_.caret };
        return std::min(caret, text.size());
    }

    float multiline_text_element::line_height() const noexcept
    {
        return font_size_ * line_spacing;
    }

    float multiline_text_element::text_left() const noexcept
    {
        return bounds().x + horizontal_padding * scale_;
    }

    float multiline_text_element::text_top() const noexcept
    {
        return bounds().y + vertical_padding * scale_;
    }

    std::size_t multiline_text_element::visible_lines() const noexcept
    {
        const float inner { bounds().height - 2.0f * vertical_padding * scale_ };
        const float height { line_height() };
        if (inner <= 0.0f || height <= 0.0f)
            return 1;
        return std::max<std::size_t>(1, static_cast<std::size_t>(inner / height));
    }

    std::size_t multiline_text_element::first_line(const std::vector<line_range>& lines, const std::size_t caret) const noexcept
    {
        const std::size_t visible { visible_lines() };
        const std::size_t current { line_of(lines, caret) };
        return current + 1 > visible ? current + 1 - visible : 0;
    }

    void multiline_text_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
        scale_ = context.scale > 0.0f ? context.scale : 1.0f;
        font_size_ = luil::scaled_font_size(logical_font_size, scale_);
    }

    std::optional<luil::text_input_snapshot> multiline_text_element::text_input() const
    {
        luil::text_input_snapshot snapshot {};
        snapshot.text = value_.text;
        snapshot.caret = value_.caret;
        snapshot.anchor = value_.anchor;
        snapshot.composing = value_.composing;
        snapshot.composition_text = value_.composition_text;
        snapshot.composition_caret = value_.composition_caret;
        snapshot.applied_sequence = value_.applied_sequence;
        // 붙여넣기가 줄바꿈과 탭을 지킨다.
        snapshot.multiline = true;
        return snapshot;
    }

    std::optional<std::size_t> multiline_text_element::offset_at(const float x, const luil::text_measurer& measurer) const
    {
        // 세로 자리를 모르면 caret 줄 안에서 고른다.
        const std::vector<line_range> lines { split_lines(value_.text) };
        const std::size_t line { line_of(lines, std::min(value_.caret, value_.text.size())) };
        const float y { text_top() + (static_cast<float>(line - first_line(lines, value_.caret)) + 0.5f) * line_height() };
        return offset_at_point(x, y, measurer);
    }

    std::optional<std::size_t> multiline_text_element::offset_at_point(const float x, const float y, const luil::text_measurer& measurer) const
    {
        // 조합 중에는 포인터로 caret을 옮기지 않는다 (조합은 IME의 것이다). 그때도 값은 확정 글 기준이다.
        const std::u8string_view text { value_.text };
        const std::vector<line_range> lines { split_lines(text) };
        if (measurer == nullptr)
            return text.size();
        const std::size_t top { first_line(lines, std::min(value_.caret, text.size())) };
        const float relative { (y - text_top()) / line_height() };
        std::size_t line { relative <= 0.0f ? top : top + static_cast<std::size_t>(relative) };
        line = std::min(line, lines.size() - 1);
        const float size { font_size_ };
        const auto measure = [&measurer, size](const std::u8string_view value) { return measurer(value, size); };
        const std::u8string_view content { text.substr(lines[line].first, lines[line].second - lines[line].first) };
        return lines[line].first + nearest_boundary(content, x - text_left(), measure);
    }

    std::optional<luil::rect_f> multiline_text_element::text_span_bounds(const luil::text_span_query& query, const luil::text_measurer& measurer) const
    {
        if (measurer == nullptr)
            return std::nullopt;
        const std::u8string_view text { query.text };
        const std::vector<line_range> lines { split_lines(text) };
        const std::size_t begin { std::min(query.begin, text.size()) };
        const std::size_t end { std::max(begin, std::min(query.end, text.size())) };
        const std::size_t line { line_of(lines, begin) };
        const std::size_t top { first_line(lines, std::min(query.caret, text.size())) };
        const float size { font_size_ };
        const auto measure = [&measurer, size](const std::u8string_view value) { return measurer(value, size); };
        const std::size_t line_begin { lines[line].first };
        const std::size_t clipped_end { std::min(end, lines[line].second) };
        const float from { text_left() + measure(text.substr(line_begin, begin - line_begin)) };
        const float to { text_left() + measure(text.substr(line_begin, clipped_end - line_begin)) };
        const float row_top { text_top() + static_cast<float>(line >= top ? line - top : 0) * line_height() };
        return luil::rect_f { from, row_top, std::max(to - from, 1.0f), line_height() };
    }

    void multiline_text_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        const float scale { context.scale > 0.0f ? context.scale : 1.0f };
        const bool focused { interaction.focused_input == id() };
        const luil::rect_f box { bounds() };
        const SkRect body { SkRect::MakeXYWH(box.x, box.y, box.width, box.height) };
        const float radius { context.metrics.control_corner_radius * scale };
        context.canvas.drawRRect(SkRRect::MakeRectXY(body, radius, radius), luil::solid_paint(context.palette.input_background));
        SkPaint border { luil::solid_paint(focused ? context.palette.accent : context.palette.input_border) };
        border.setStyle(SkPaint::kStroke_Style);
        border.setStrokeWidth(1.0f * scale);
        context.canvas.drawRRect(SkRRect::MakeRectXY(body, radius, radius), border);

        const SkFont font { sk_ref_sp(context.ui_typeface), font_size_ };
        const float left { text_left() };
        const float height { line_height() };
        const float inner_width { std::max(box.width - 2.0f * horizontal_padding * scale, 0.0f) };
        const std::u8string_view text { displayed_text() };
        const std::size_t caret { displayed_caret() };
        const std::vector<line_range> lines { split_lines(text) };
        const std::size_t top { first_line(lines, caret) };
        const std::size_t count { std::min(lines.size() - top, visible_lines()) };

        context.canvas.save();
        context.canvas.clipRect(SkRect::MakeXYWH(box.x + 1.0f * scale, box.y + 1.0f * scale, box.width - 2.0f * scale, box.height - 2.0f * scale));
        if (text.empty() && focused == false && placeholder_.empty() == false)
        {
            SkPaint placeholder { luil::solid_paint(context.palette.primary_foreground) };
            placeholder.setAlphaf(0.5f);
            static_cast<void>(luil::draw_text_within(context.canvas, placeholder_, left, text_top() + luil::centered_text_baseline(font, height), inner_width, font, placeholder));
        }

        const std::size_t selection_begin { value_.composing ? 0u : std::min(value_.caret, value_.anchor) };
        const std::size_t selection_end { value_.composing ? 0u : std::max(value_.caret, value_.anchor) };
        const SkPaint foreground { luil::solid_paint(enabled() ? context.palette.primary_foreground : context.palette.disabled_foreground) };
        for (std::size_t row { 0 }; row < count; ++row)
        {
            const line_range& range { lines[top + row] };
            const std::u8string_view content { text.substr(range.first, range.second - range.first) };
            const float row_top { text_top() + static_cast<float>(row) * height };
            const float baseline { row_top + luil::centered_text_baseline(font, height) };
            // 고른 구간은 줄마다 그린다. 줄 끝을 넘는 선택(줄바꿈 포함)은 글자 하나 폭만큼 더 칠한다.
            if (selection_begin != selection_end && selection_begin <= range.second && selection_end >= range.first)
            {
                const std::size_t from { std::max(selection_begin, range.first) - range.first };
                const std::size_t to { std::min(selection_end, range.second) - range.first };
                const float x0 { left + luil::measure_text(content.substr(0, from), font) };
                float x1 { left + luil::measure_text(content.substr(0, to), font) };
                if (selection_end > range.second)
                    x1 += font_size_ * 0.4f;
                context.canvas.drawRect(SkRect::MakeXYWH(x0, row_top + 1.0f * scale, x1 - x0, height - 2.0f * scale), luil::solid_paint(context.palette.selection_background));
            }
            if (content.empty() == false)
                luil::draw_text(context.canvas, content, left, baseline, font, foreground);
            if (value_.composing && value_.composing_end > value_.composing_begin && value_.composing_begin <= range.second && value_.composing_end >= range.first)
            {
                const std::size_t from { std::max(value_.composing_begin, range.first) - range.first };
                const std::size_t to { std::min(value_.composing_end, range.second) - range.first };
                const float x0 { left + luil::measure_text(content.substr(0, from), font) };
                const float x1 { left + luil::measure_text(content.substr(0, to), font) };
                context.canvas.drawRect(SkRect::MakeXYWH(x0, baseline + 2.0f * scale, x1 - x0, 1.0f * scale), luil::solid_paint(context.palette.accent));
            }
        }

        const bool caret_on { focused && interaction.focus_started_at.has_value() && ((context.now - *interaction.focus_started_at) / luil::caret_blink_interval) % 2 == 0 };
        if (caret_on)
        {
            const std::size_t line { line_of(lines, caret) };
            if (line >= top && line < top + count)
            {
                const line_range& range { lines[line] };
                const float x { left + luil::measure_text(text.substr(range.first, caret - range.first), font) };
                const float row_top { text_top() + static_cast<float>(line - top) * height };
                context.canvas.drawRect(SkRect::MakeXYWH(x, row_top + 2.0f * scale, 1.0f * scale, height - 4.0f * scale), luil::solid_paint(context.palette.primary_foreground));
            }
        }
        context.canvas.restore();
    }

    std::optional<std::chrono::steady_clock::time_point> multiline_text_element::next_update(const luil::update_context& context, const luil::interaction_snapshot& interaction) const
    {
        if (interaction.focused_input != id() || interaction.focus_started_at.has_value() == false)
            return std::nullopt;
        const auto phase { (context.now - *interaction.focus_started_at) % luil::caret_blink_interval };
        return context.now + (luil::caret_blink_interval - phase);
    }

    luil::access_info multiline_text_element::accessibility() const
    {
        return { .role = luil::access_role::edit, .name = access_name_, .value = value_.text };
    }
} // namespace cyella::ui
