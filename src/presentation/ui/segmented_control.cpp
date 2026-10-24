#include "presentation/ui/segmented_control.h"

#include "domain/text_catalog.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"

#include <algorithm>
#include <utility>

namespace cyella::ui {
    namespace {
        constexpr float segment_inset { 2.0f };
        constexpr float segment_radius { 4.0f };
        constexpr float segment_icon_size { 14.0f };
        constexpr float segment_font_size { 11.0f };

        class segment_element final : public luil::ui_element
        {
        public:
            segment_element(std::u8string owner, segment_option option, const bool selected, const bool icons_only, const luil::choice_message_factory& select)
                : ui_element { { kind_segment, std::move(owner) } }
                , option_ { std::move(option) }
                , selected_ { selected }
                , icons_only_ { icons_only }
            {
                set_text_tooltip(*this, option_.label);
                set_search_label(option_.label);
                set_cursor(luil::ui_cursor::hand);
                if (select != nullptr)
                    set_action(luil::ui_trigger::left_click, action_of(select(option_.value)));
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const luil::rect_f box { bounds() };
                const float scale { context.scale };
                if (selected_)
                {
                    const SkRRect shape { SkRRect::MakeRectXY(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), segment_radius * scale, segment_radius * scale) };
                    context.canvas.drawRRect(shape, luil::solid_paint(context.palette.surface_background));
                    SkPaint border { luil::solid_paint(luil::with_alpha(context.palette.accent, 0.45f)) };
                    border.setStyle(SkPaint::kStroke_Style);
                    border.setStrokeWidth(scale);
                    border.setAntiAlias(true);
                    context.canvas.drawRRect(shape, border);
                }
                else
                    luil::draw_hover_fill(context, box, id(), interaction, enabled(), segment_radius);

                const SkPaint foreground { luil::solid_paint(selected_ ? context.palette.accent : context.palette.secondary_foreground) };
                if (icons_only_)
                {
                    luil::draw_centered_glyph(context.canvas, option_.glyph, box, icon_font(context, segment_icon_size), foreground);
                    return;
                }
                const SkFont font { ui_font(context, segment_font_size, selected_) };
                const float width { std::min(luil::measure_text(option_.label, font), box.width) };
                static_cast<void>(
                    luil::draw_text_within(context.canvas, option_.label, box.x + (box.width - width) / 2.0f, box.y + luil::centered_text_baseline(font, box.height), width, font, foreground));
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::radio_button, .name = option_.label, .selected = selected_ };
            }

        private:
            segment_option option_ {};
            bool selected_ { false };
            bool icons_only_ { false };
        };
    } // namespace

    segmented_control_element::segmented_control_element(
        std::u8string owner, std::u8string name, std::vector<segment_option> options, std::u8string selected, luil::choice_message_factory select, const bool icons_only)
        : ui_element { { kind_segmented, owner } }
        , name_ { std::move(name) }
    {
        set_focus_group(luil::focus_axis::horizontal);
        for (segment_option& option : options)
        {
            const bool active { option.value == selected };
            const std::u8string key { option.owner.empty() ? owner + u8"-" + option.value : option.owner };
            if (active)
                set_focus_entry({ kind_segment, key });
            add_child(std::make_unique<segment_element>(key, std::move(option), active, icons_only, select));
        }
    }

    void segmented_control_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
        if (children().empty())
            return;
        const float inset { segment_inset * context.scale };
        const float width { std::max(context.slot.width - inset * 2.0f, 0.0f) / static_cast<float>(children().size()) };
        float x { context.slot.x + inset };
        for (const std::unique_ptr<luil::ui_element>& child : children())
        {
            child->arrange(context.for_child({ x, context.slot.y + inset, width, std::max(context.slot.height - inset * 2.0f, 0.0f) }));
            x += width;
        }
    }

    void segmented_control_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        const luil::rect_f box { bounds() };
        const float radius { (segment_radius + segment_inset) * context.scale };
        context.canvas.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), radius, radius), luil::solid_paint(context.palette.input_background));
        draw_children(context, interaction);
    }

    luil::access_info segmented_control_element::accessibility() const
    {
        return { .role = luil::access_role::radio_group, .name = name_ };
    }

    std::unique_ptr<luil::ui_element> make_segmented_control(luil::choice_group_config config)
    {
        std::vector<segment_option> options {};
        for (luil::choice_item& item : config.items)
            options.push_back({ item.value, std::move(item.label), 0, item.value });
        return std::make_unique<segmented_control_element>(std::move(config.owner), std::move(config.name), std::move(options), std::move(config.selected), std::move(config.select));
    }

    std::unique_ptr<luil::ui_element> make_file_view_control(std::u8string owner, const bool tree, luil::choice_message_factory select)
    {
        return std::make_unique<segmented_control_element>(std::move(owner), tr(u8"segmented.file_view"),
            std::vector<segment_option> { { u8"list", tr(u8"segmented.file_view_list"), codicons::icon_list_flat }, { u8"tree", tr(u8"segmented.file_view_tree"), codicons::icon_list_tree } },
            tree ? u8"tree" : u8"list", std::move(select), true);
    }
} // namespace cyella::ui
