#include "presentation/ui/ui_helpers.h"

#include "presentation/text_wrap.h"

#include "luil/ui/font_scale.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"
#include "include/core/SkTypeface.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace cyella::ui {
    namespace {
        constexpr std::array<luil::ui_color, 8> dark_lanes {
            luil::make_ui_color(0x4f, 0xb3, 0xe8),
            luil::make_ui_color(0xe3, 0xa3, 0x4b),
            luil::make_ui_color(0xb4, 0x8e, 0xf0),
            luil::make_ui_color(0x5f, 0xc9, 0x8a),
            luil::make_ui_color(0xec, 0x7a, 0x9b),
            luil::make_ui_color(0xd8, 0xcf, 0x55),
            luil::make_ui_color(0x6f, 0xd3, 0xd0),
            luil::make_ui_color(0xf0, 0x8a, 0x63),
        };
        constexpr std::array<luil::ui_color, 8> light_lanes {
            luil::make_ui_color(0x1f, 0x7f, 0xc0),
            luil::make_ui_color(0xb8, 0x73, 0x0f),
            luil::make_ui_color(0x7d, 0x4f, 0xd0),
            luil::make_ui_color(0x23, 0x94, 0x4f),
            luil::make_ui_color(0xc4, 0x3c, 0x66),
            luil::make_ui_color(0x8d, 0x84, 0x10),
            luil::make_ui_color(0x18, 0x8e, 0x8a),
            luil::make_ui_color(0xc0, 0x53, 0x2a),
        };

        // 툴바 단추의 아이콘·글자 크기와 아이콘·글자 사이 간격이다 (논리 픽셀).
        constexpr float tool_icon_size { 16.0f };
        constexpr float tool_label_size { 10.5f };
        constexpr float tool_label_gap { 2.0f };
        constexpr float spinner_icon_size { 14.0f };

        bool dark(const luil::ui_color_palette& palette) noexcept
        {
            return luil::is_dark_background(palette.window_background);
        }
    } // namespace

    luil::ui_action action_of(luil::input_action action)
    {
        return [action = std::move(action)](const luil::ui_action_context&) -> std::vector<luil::input_action> { return { action }; };
    }

    luil::input_action ui_command(const std::uint32_t command, std::u8string argument)
    {
        return luil::input_action { luil::app_ui_command { command, std::move(argument) } };
    }

    luil::ui_color lane_color(const luil::ui_color_palette& palette, const std::uint8_t index) noexcept
    {
        const auto& lanes { dark(palette) ? dark_lanes : light_lanes };
        return lanes[index % lanes.size()];
    }

    luil::ui_color success_color(const luil::ui_color_palette& palette) noexcept
    {
        return dark(palette) ? luil::make_ui_color(0x5f, 0xc9, 0x8a) : luil::make_ui_color(0x1a, 0x7f, 0x37);
    }

    luil::ui_color diff_add_background(const luil::ui_color_palette& palette) noexcept
    {
        return dark(palette) ? luil::make_ui_color(0x2e, 0xa0, 0x43, 0x26) : luil::make_ui_color(0x1a, 0x7f, 0x37, 0x20);
    }

    luil::ui_color diff_delete_background(const luil::ui_color_palette& palette) noexcept
    {
        return dark(palette) ? luil::make_ui_color(0xf8, 0x51, 0x49, 0x26) : luil::make_ui_color(0xcf, 0x22, 0x2e, 0x1c);
    }

    luil::ui_color status_color(const luil::ui_color_palette& palette, const char status) noexcept
    {
        switch (status)
        {
        case 'A':
        case '?':
            return success_color(palette);
        case 'D':
        case 'U':
            return palette.error_accent;
        case 'R':
        case 'C':
            return lane_color(palette, 0);
        default:
            break;
        }
        return palette.warning_accent;
    }

    std::u8string number_text(const std::uint64_t value)
    {
        const std::string text { std::to_string(value) };
        return std::u8string { text.begin(), text.end() };
    }

    std::u8string to_u8(const std::string_view text)
    {
        return std::u8string { text.begin(), text.end() };
    }

    std::string to_text(const std::u8string_view text)
    {
        return std::string { text.begin(), text.end() };
    }

    std::unique_ptr<luil::label_element> make_label(std::u8string owner, std::u8string text, const float size, const luil::label_color_role role)
    {
        auto label { std::make_unique<luil::label_element>(luil::ui_element_id { kind_label, std::move(owner) }, luil::label_config { .text = text, .font_size = size, .color = role }) };
        set_text_tooltip(*label, std::move(text));
        label->set_hit_opaque(false);
        return label;
    }

    std::unique_ptr<luil::ui_element> make_icon_button(
        std::u8string owner, const char32_t glyph, std::u8string tooltip, luil::input_action action, const bool enabled, const bool active, const float icon_size)
    {
        auto button { std::make_unique<luil::button_element>(luil::ui_element_id { kind_button, std::move(owner) }, luil::button_config { .glyph = glyph, .icon_size = icon_size, .active = active }) };
        set_text_tooltip(*button, std::move(tooltip));
        button->set_enabled(enabled);
        button->set_cursor(luil::ui_cursor::hand);
        button->set_action(luil::ui_trigger::left_click, action_of(std::move(action)));
        return button;
    }

    std::unique_ptr<luil::ui_element> make_text_button(
        std::u8string owner, std::u8string text, const luil::text_button_visual visual, luil::input_action action, const bool enabled, const char32_t glyph, const bool default_button)
    {
        auto button = std::make_unique<luil::text_button_element>(
            luil::ui_element_id { kind_text_button, std::move(owner) }, luil::text_button_config { .text = text, .visual = visual, .glyph = glyph, .default_button = default_button });
        button->set_enabled(enabled);
        button->set_cursor(luil::ui_cursor::hand);
        button->set_action(luil::ui_trigger::left_click, action_of(std::move(action)));
        button->set_default_button(default_button);
        return button;
    }

    float text_button_width(const std::u8string_view text, const bool glyph) noexcept
    {
        float width { 20.0f };
        for (const char8_t value : text)
            if (value < 0x80)
                width += 7.0f;
            else if ((value & 0xC0u) == 0xC0u)
                width += 12.0f;
        if (glyph)
            width += luil::text_button_icon_size + luil::text_button_icon_gap;
        return width;
    }

    SkFont ui_font(const luil::draw_context& context, const float logical_size, const bool bold)
    {
        SkFont font { sk_ref_sp(context.ui_typeface), luil::scaled_font_size(logical_size, context.scale) };
        font.setEmbolden(bold);
        return font;
    }

    SkFont code_font(const luil::draw_context& context, const float logical_size)
    {
        SkTypeface* const typeface { context.code_typeface != nullptr ? context.code_typeface : context.ui_typeface };
        return SkFont { sk_ref_sp(typeface), luil::scaled_font_size(logical_size, context.scale) };
    }

    SkFont icon_font(const luil::draw_context& context, const float logical_size)
    {
        return SkFont { sk_ref_sp(context.codicon_typeface), logical_size * context.scale };
    }

    luil::panel_config floating_panel_config(const float corner_radius, const float shadow)
    {
        luil::panel_config config {};
        config.background = [](const luil::ui_color_palette& palette) { return palette.surface_background; };
        config.border = [](const luil::ui_color_palette& palette) { return palette.divider; };
        config.corner_radius = corner_radius;
        config.shadow = shadow;
        return config;
    }

    // ---------------------------------------------------------------- text_element

    text_element::text_element(luil::ui_element_id id, std::u8string text, text_style style)
        : ui_element { std::move(id) }
        , text_ { std::move(text) }
        , style_ { std::move(style) }
    {
        set_text_tooltip(*this, text_);
        set_hit_opaque(false);
    }

    void text_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    void text_element::draw(luil::draw_context& context, const luil::interaction_snapshot&) const
    {
        const luil::rect_f box { bounds() };
        if (box.width <= 0.0f || text_.empty())
            return;
        const SkFont font { style_.code ? code_font(context, style_.size) : ui_font(context, style_.size, style_.bold) };
        const luil::ui_color color { style_.color != nullptr ? style_.color(context.palette) : context.palette.primary_foreground };
        const SkPaint paint { luil::solid_paint(color) };
        const float baseline { box.y + luil::centered_text_baseline(font, box.height) };
        float x { box.x };
        if (style_.align != 0)
        {
            const float width { std::min(luil::measure_text(text_, font), box.width) };
            x = style_.align == 1 ? box.x + (box.width - width) / 2.0f : box.x + box.width - width;
        }
        static_cast<void>(luil::draw_text_within(context.canvas, text_, x, baseline, box.x + box.width - x, font, paint));
    }

    luil::access_info text_element::accessibility() const
    {
        return { .role = luil::access_role::static_text, .name = text_ };
    }

    wrapped_text_element::wrapped_text_element(luil::ui_element_id id, std::u8string text, text_style style, const float line_height)
        : ui_element { std::move(id) }
        , text_ { std::move(text) }
        , style_ { std::move(style) }
        , line_height_ { line_height }
    {
        set_hit_opaque(false);
    }

    void wrapped_text_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    void wrapped_text_element::draw(luil::draw_context& context, const luil::interaction_snapshot&) const
    {
        const luil::rect_f box { bounds() };
        if (box.width <= 0.0f || text_.empty())
            return;
        const float scale { context.scale };
        const SkFont font { style_.code ? code_font(context, style_.size) : ui_font(context, style_.size, style_.bold) };
        const SkPaint paint { luil::solid_paint(style_.color != nullptr ? style_.color(context.palette) : context.palette.primary_foreground) };
        const std::vector<text_line> lines { wrap_text_lines(text_, box.width, [&font](const std::u8string_view part) { return luil::measure_text(part, font); }) };
        const float step { line_height_ * scale };
        const auto room { static_cast<std::size_t>(std::max(std::floor(box.height / step + 0.01f), 1.0f)) };
        const std::u8string_view text { text_ };
        for (std::size_t index { 0 }; index < lines.size() && index < room; ++index)
        {
            std::size_t end { lines[index].end };
            // 자리가 모자라면 마지막 줄은 그 문단 끝까지 잇고 말줄임으로 자른다.
            if (index + 1 == room && lines.size() > room)
            {
                end = text.find(u8'\n', lines[index].begin);
                if (end == std::u8string_view::npos)
                    end = text.size();
                if (end > lines[index].begin && text[end - 1] == u8'\r')
                    --end;
            }
            const std::u8string_view part { text.substr(lines[index].begin, end - lines[index].begin) };
            const float top { box.y + static_cast<float>(index) * step };
            static_cast<void>(luil::draw_text_within(context.canvas, part, box.x, top + luil::centered_text_baseline(font, step), box.width, font, paint));
        }
    }

    luil::access_info wrapped_text_element::accessibility() const
    {
        return { .role = luil::access_role::static_text, .name = text_ };
    }

    namespace {
        // 작성자 줄의 치수다 (논리 픽셀).
        constexpr float author_photo_size { 20.0f };
        constexpr float author_gap { 6.0f };
        constexpr float author_font_size { 12.0f };
    } // namespace

    author_element::author_element(std::u8string owner, std::u8string name, std::u8string email, luil::ui_image avatar)
        : ui_element { luil::ui_element_id { kind_label, std::move(owner) } }
        , name_ { std::move(name) }
        , email_ { std::move(email) }
    {
        // 첫 글자 하나다 (UTF-8 한 글자, 라틴 소문자는 대문자로). 이름이 없으면 비운다.
        std::size_t length { 0 };
        if (name_.empty() == false)
        {
            const auto lead { static_cast<unsigned char>(name_.front()) };
            length = lead < 0x80u ? 1 : (lead & 0xE0u) == 0xC0u ? 2 : (lead & 0xF0u) == 0xE0u ? 3 : 4;
        }
        initial_ = name_.substr(0, std::min(length, name_.size()));
        if (initial_.size() == 1 && initial_[0] >= u8'a' && initial_[0] <= u8'z')
            initial_[0] = static_cast<char8_t>(initial_[0] - u8'a' + u8'A');
        // 올려서 메일을 보는 자리라 누름은 받지 않되 hover는 받는다.
        set_tab_stop(false);
        if (email_.empty() == false)
            set_text_tooltip(*this, email_);
        if (avatar.valid())
        {
            luil::image_config config { .image = std::move(avatar), .fit = luil::image_fit::cover };
            auto photo { std::make_unique<luil::image_element>(luil::ui_element_id { kind_image, id().owner + u8"-photo" }, std::move(config)) };
            photo->set_hit_opaque(false);
            photo_ = photo.get();
            add_child(std::move(photo));
        }
    }

    float author_element::width_for(const std::u8string_view name) noexcept
    {
        return author_photo_size + author_gap + estimate_text_width(name, author_font_size, true) + 2.0f;
    }

    void author_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
        if (photo_ == nullptr)
            return;
        const float size { author_photo_size * context.scale };
        photo_->arrange(context.for_child({ context.slot.x, context.slot.y + (context.slot.height - size) / 2.0f, size, size }));
    }

    luil::access_info author_element::accessibility() const
    {
        return { .role = luil::access_role::static_text, .name = email_.empty() ? name_ : name_ + u8" <" + email_ + u8">" };
    }

    void author_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        const luil::rect_f box { bounds() };
        const float scale { context.scale };
        const float size { author_photo_size * scale };
        const float x { box.x };
        const float y { box.y + (box.height - size) / 2.0f };
        const SkRect circle { SkRect::MakeXYWH(x, y, size, size) };
        if (photo_ != nullptr)
        {
            // 사진은 동그라미로 자른다.
            context.canvas.save();
            context.canvas.clipRRect(SkRRect::MakeOval(circle), true);
            draw_children(context, interaction);
            context.canvas.restore();
        }
        else
        {
            // 강조색을 옅게 깐 동그라미에 본문색 글자다. 강조색 글자는 옅은 강조 바탕 위에서 대비가 모자랐다.
            SkPaint fill { luil::solid_paint(luil::with_alpha(context.palette.accent, 0.35f)) };
            fill.setAntiAlias(true);
            context.canvas.drawOval(circle, fill);
            if (initial_.empty() == false)
            {
                const SkFont font { ui_font(context, 11.0f, true) };
                const float width { luil::measure_text(initial_, font) };
                static_cast<void>(luil::draw_text_within(
                    context.canvas, initial_, x + (size - width) / 2.0f, y + luil::centered_text_baseline(font, size), size, font, luil::solid_paint(context.palette.primary_foreground)));
            }
        }
        const float text_x { x + size + author_gap * scale };
        if (box.x + box.width - text_x <= 0.0f)
            return;
        const SkFont font { ui_font(context, author_font_size, true) };
        static_cast<void>(luil::draw_text_within(
            context.canvas, name_, text_x, box.y + luil::centered_text_baseline(font, box.height), box.x + box.width - text_x, font, luil::solid_paint(context.palette.primary_foreground)));
    }

    card_element::card_element(std::u8string owner, std::u8string access_name, std::vector<section> sections)
        : ui_element { luil::ui_element_id { kind_panel, std::move(owner) } }
        , access_name_ { std::move(access_name) }
    {
        set_hit_opaque(false);
        for (section& part : sections)
        {
            if (part.content == nullptr)
                continue;
            sections_.push_back({ .content = part.content.get(), .height = part.height, .tinted = part.tinted });
            add_child(std::move(part.content));
        }
    }

    float card_element::height_for(const std::vector<section>& sections) noexcept
    {
        float height { 2.0f };
        for (const section& part : sections)
            if (part.content != nullptr)
                height += part.height;
        return height;
    }

    void card_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
        const float border { context.scale };
        float y { context.slot.y + border };
        for (const placed& part : sections_)
        {
            const float height { part.height * context.scale };
            part.content->arrange(context.for_child({ context.slot.x + border, y, std::max(context.slot.width - border * 2.0f, 0.0f), height }));
            y += height;
        }
    }

    luil::access_info card_element::accessibility() const
    {
        return { .role = luil::access_role::group, .name = access_name_ };
    }

    void card_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        const luil::rect_f box { bounds() };
        if (box.width <= 0.0f || box.height <= 0.0f)
            return;
        const float scale { context.scale };
        const float radius { context.metrics.control_corner_radius * scale };
        const SkRect shape { SkRect::MakeXYWH(box.x, box.y, box.width, box.height) };
        const SkRRect rounded { SkRRect::MakeRectXY(shape, radius, radius) };
        context.canvas.save();
        context.canvas.clipRRect(rounded, true);
        float y { box.y + scale };
        bool first { true };
        for (const placed& part : sections_)
        {
            const float height { part.height * scale };
            if (part.tinted)
                context.canvas.drawRect(SkRect::MakeXYWH(box.x, y, box.width, height), luil::solid_paint(context.palette.input_background));
            if (first == false)
                context.canvas.drawRect(SkRect::MakeXYWH(box.x, y, box.width, scale), luil::solid_paint(context.palette.divider));
            first = false;
            y += height;
        }
        draw_children(context, interaction);
        context.canvas.restore();
        SkPaint border { luil::solid_paint(context.palette.group_border) };
        border.setAntiAlias(true);
        border.setStyle(SkPaint::kStroke_Style);
        border.setStrokeWidth(scale);
        context.canvas.drawRRect(SkRRect::MakeRectXY(shape.makeInset(scale / 2.0f, scale / 2.0f), radius, radius), border);
    }

    float wrapped_text_height(const std::u8string_view text, const float width, const text_style& style, const float line_height)
    {
        return static_cast<float>(estimate_wrapped_line_count(text, width, style.size, style.bold || style.code)) * line_height;
    }

    std::unique_ptr<luil::ui_element> make_wrapped_text(std::u8string owner, std::u8string text, text_style style, const float line_height)
    {
        return std::make_unique<wrapped_text_element>(luil::ui_element_id { kind_label, std::move(owner) }, std::move(text), std::move(style), line_height);
    }

    std::unique_ptr<luil::ui_element> make_text(std::u8string owner, std::u8string text, text_style style)
    {
        return std::make_unique<text_element>(luil::ui_element_id { kind_label, std::move(owner) }, std::move(text), std::move(style));
    }

    // ---------------------------------------------------------------- absolute_element

    absolute_element::absolute_element(luil::ui_element_id id)
        : ui_element { std::move(id) }
    {}

    void absolute_element::place(std::unique_ptr<luil::ui_element> child, const shell_rect& rect)
    {
        if (child == nullptr || rect.empty())
            return;
        entries_.push_back({ child.get(), rect });
        add_child(std::move(child));
    }

    void absolute_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
        const float scale { context.scale };
        for (const entry& item : entries_)
        {
            const luil::rect_f slot { context.slot.x + item.rect.x * scale, context.slot.y + item.rect.y * scale, item.rect.width * scale, item.rect.height * scale };
            item.child->arrange(context.for_child(slot));
        }
    }

    void absolute_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        draw_children(context, interaction);
    }

    // ---------------------------------------------------------------- bar_element

    bar_element::bar_element(luil::ui_element_id id, bar_style style, std::unique_ptr<luil::ui_element> content)
        : ui_element { std::move(id) }
        , style_ { std::move(style) }
    {
        if (content != nullptr)
        {
            content_ = content.get();
            add_child(std::move(content));
        }
    }

    void bar_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
        if (content_ != nullptr)
            content_->arrange(context.for_child(context.slot));
    }

    void bar_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        const luil::rect_f box { bounds() };
        if (style_.background != nullptr)
            context.canvas.drawRect(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), luil::solid_paint(style_.background(context.palette)));
        draw_children(context, interaction);
        const SkPaint divider { luil::solid_paint(context.palette.divider) };
        if (style_.divider_bottom)
            context.canvas.drawRect(SkRect::MakeXYWH(box.x, box.y + box.height - context.scale, box.width, context.scale), divider);
        if (style_.divider_top)
            context.canvas.drawRect(SkRect::MakeXYWH(box.x, box.y, box.width, context.scale), divider);
    }

    // ---------------------------------------------------------------- tool_button_element

    tool_button_element::tool_button_element(
        std::u8string owner, const char32_t glyph, std::u8string label, std::u8string tooltip, const bool show_label, const bool enabled, luil::input_action action, const bool horizontal)
        : ui_element { luil::ui_element_id { kind_tool_button, std::move(owner) } }
        , glyph_ { glyph }
        , label_ { std::move(label) }
        , show_label_ { show_label }
        , horizontal_ { horizontal }
    {
        set_text_tooltip(*this, tooltip.empty() ? label_ : std::move(tooltip));
        set_access_name(label_);
        set_enabled(enabled);
        set_cursor(luil::ui_cursor::hand);
        set_action(luil::ui_trigger::left_click, action_of(std::move(action)));
    }

    void tool_button_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    void tool_button_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        const luil::rect_f box { bounds() };
        const float scale { context.scale };
        const luil::rect_f inner { box.x + 2.0f * scale, box.y + 4.0f * scale, box.width - 4.0f * scale, box.height - 8.0f * scale };
        luil::draw_hover_fill(context, inner, id(), interaction, enabled(), context.metrics.control_corner_radius);
        const SkPaint paint { luil::solid_paint(enabled() ? context.palette.primary_foreground : context.palette.disabled_foreground) };
        if (show_label_ == false)
        {
            luil::draw_centered_glyph(context.canvas, glyph_, inner, icon_font(context, tool_icon_size), paint);
            return;
        }
        if (horizontal_)
        {
            const float glyph_width { 16.0f * scale };
            const float gap { 6.0f * scale };
            const SkFont font { ui_font(context, 11.5f) };
            const float text_width { std::max(inner.width - glyph_width - gap - 12.0f * scale, 0.0f) };
            const float used { std::min(luil::measure_text(label_, font), text_width) };
            const float left { inner.x + (inner.width - glyph_width - gap - used) / 2.0f };
            luil::draw_centered_glyph(context.canvas, glyph_, { left, inner.y, glyph_width, inner.height }, icon_font(context, tool_icon_size), paint);
            static_cast<void>(luil::draw_text_within(context.canvas, label_, left + glyph_width + gap, inner.y + luil::centered_text_baseline(font, inner.height), text_width, font, paint));
            return;
        }
        const float icon_height { tool_icon_size * scale };
        const float label_height { tool_label_size * scale + 2.0f * scale };
        const float top { inner.y + (inner.height - icon_height - tool_label_gap * scale - label_height) / 2.0f };
        luil::draw_centered_glyph(context.canvas, glyph_, { inner.x, top, inner.width, icon_height }, icon_font(context, tool_icon_size), paint);
        const SkFont font { ui_font(context, tool_label_size) };
        const float width { std::min(luil::measure_text(label_, font), inner.width) };
        const float label_top { top + icon_height + tool_label_gap * scale };
        static_cast<void>(
            luil::draw_text_within(context.canvas, label_, inner.x + (inner.width - width) / 2.0f, label_top + luil::centered_text_baseline(font, label_height), inner.width, font, paint));
    }

    luil::access_info tool_button_element::accessibility() const
    {
        return { .role = luil::access_role::button, .name = enabled() || tooltip().empty() ? label_ : label_ + u8" · " + tooltip() };
    }

    // ---------------------------------------------------------------- icon_element

    icon_element::icon_element(luil::ui_element_id id, const char32_t glyph, color_selector color, const float size)
        : ui_element { std::move(id) }
        , glyph_ { glyph }
        , color_ { std::move(color) }
        , size_ { size }
    {
        set_hit_opaque(false);
    }

    void icon_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    void icon_element::draw(luil::draw_context& context, const luil::interaction_snapshot&) const
    {
        const SkPaint paint { luil::solid_paint(color_ != nullptr ? color_(context.palette) : context.palette.secondary_foreground) };
        luil::draw_centered_glyph(context.canvas, glyph_, bounds(), icon_font(context, size_), paint);
    }

    // ---------------------------------------------------------------- spinner_element

    spinner_element::spinner_element(luil::ui_element_id id, const char32_t glyph, color_selector color)
        : ui_element { std::move(id) }
        , glyph_ { glyph }
        , color_ { std::move(color) }
    {
        set_hit_opaque(false);
    }

    void spinner_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    void spinner_element::draw(luil::draw_context& context, const luil::interaction_snapshot&) const
    {
        const SkPaint paint { luil::solid_paint(color_ != nullptr ? color_(context.palette) : context.palette.accent) };
        luil::draw_spinning_glyph(context.canvas, glyph_, bounds(), icon_font(context, spinner_icon_size), paint, context.now);
    }

    std::optional<std::chrono::steady_clock::time_point> spinner_element::next_update(const luil::update_context& context, const luil::interaction_snapshot&) const
    {
        // 30fps면 도는 것이 매끄럽게 보이고, 작업이 길어도 CPU를 붙잡지 않는다.
        return context.now + std::chrono::milliseconds { 33 };
    }
} // namespace cyella::ui
