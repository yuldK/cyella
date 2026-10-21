#include "presentation/ui/settings/appearance_ui.h"

#include "domain/text_catalog.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"

#include "luil/text/fonts.h"
#include "luil/ui/panel_element.h"
#include "luil/ui/stack_element.h"
#include "luil/ui/virtual_list_element.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace cyella::ui {
    namespace {
        constexpr float theme_gap { 12.0f };
        constexpr float theme_caption_height { 24.0f };
        constexpr float theme_radius { 7.0f };
        constexpr float theme_check_size { 20.0f };
        constexpr float font_arrow_width { 28.0f };
        constexpr float font_popup_padding { 6.0f };
        constexpr float font_row_height { 26.0f };
        constexpr float font_search_height { 28.0f };
        constexpr std::size_t font_visible_rows { 9 };

        void rounded(luil::draw_context& context, const luil::rect_f& box, const float radius, const luil::ui_color color)
        {
            context.canvas.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), radius, radius), luil::solid_paint(color));
        }

        void draw_theme_preview(luil::draw_context& context, const luil::rect_f& box, const luil::ui_color_palette& palette)
        {
            const float scale { context.scale };
            rounded(context, box, theme_radius * scale, palette.window_background);
            const auto bar = [&](const float x, const float y, const float width, const float height, const luil::ui_color color) {
                rounded(context, { box.x + box.width * x, box.y + box.height * y, box.width * width, box.height * height }, 1.5f * scale, color);
            };
            bar(0.06f, 0.07f, 0.21f, 0.035f, palette.secondary_foreground);
            bar(0.06f, 0.20f, 0.12f, 0.04f, palette.accent);
            bar(0.23f, 0.20f, 0.10f, 0.04f, palette.disabled_foreground);
            bar(0.38f, 0.20f, 0.12f, 0.04f, palette.disabled_foreground);
            bar(0.06f, 0.32f, 0.55f, 0.59f, palette.surface_background);
            bar(0.66f, 0.32f, 0.28f, 0.59f, palette.surface_background);
            for (int line { 0 }; line < 4; ++line)
                bar(0.11f, 0.40f + static_cast<float>(line) * 0.105f, line % 2 == 0 ? 0.41f : 0.32f, 0.032f, palette.disabled_foreground);
            bar(0.70f, 0.38f, 0.20f, 0.20f, luil::with_alpha(palette.accent, 0.6f));
            bar(0.70f, 0.66f, 0.18f, 0.032f, palette.disabled_foreground);
            bar(0.70f, 0.76f, 0.13f, 0.032f, palette.disabled_foreground);
        }

        class theme_option_element final : public luil::ui_element
        {
        public:
            theme_option_element(const theme_preference theme, const bool selected, std::u8string title)
                : ui_element { { kind_theme_option, u8"settings-theme-" + std::u8string { theme_preference_name(theme) } } }
                , theme_ { theme }
                , selected_ { selected }
                , title_ { std::move(title) }
            {
                set_cursor(luil::ui_cursor::hand);
                set_tooltip(title_);
                set_action(luil::ui_trigger::left_click, action_of(message(set_theme_intent { theme })));
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const luil::rect_f box { bounds() };
                const float scale { context.scale };
                const luil::rect_f preview { box.x + scale, box.y + scale, box.width - scale * 2.0f, box.height - theme_caption_height * scale - scale * 2.0f };
                const SkRRect shape { SkRRect::MakeRectXY(SkRect::MakeXYWH(preview.x, preview.y, preview.width, preview.height), theme_radius * scale, theme_radius * scale) };
                context.canvas.save();
                context.canvas.clipRRect(shape, true);
                const luil::ui_color_palette light { luil::color_palette_for(luil::color_theme::light) };
                const luil::ui_color_palette dark { luil::color_palette_for(luil::color_theme::dark) };
                draw_theme_preview(context, preview, theme_ == theme_preference::dark ? dark : light);
                if (theme_ == theme_preference::system)
                {
                    SkPathBuilder diagonal {};
                    diagonal.moveTo(preview.x + preview.width * 0.62f, preview.y);
                    diagonal.lineTo(preview.x + preview.width, preview.y);
                    diagonal.lineTo(preview.x + preview.width, preview.y + preview.height);
                    diagonal.lineTo(preview.x + preview.width * 0.38f, preview.y + preview.height);
                    diagonal.close();
                    context.canvas.clipPath(diagonal.detach(), true);
                    draw_theme_preview(context, preview, dark);
                }
                context.canvas.restore();
                SkPaint border { luil::solid_paint(selected_ ? context.palette.accent : context.palette.divider) };
                border.setStyle(SkPaint::kStroke_Style);
                border.setStrokeWidth((selected_ ? 2.0f : 1.0f) * scale);
                border.setAntiAlias(true);
                context.canvas.drawRRect(shape, border);
                if (interaction.hovered == id() && selected_ == false)
                {
                    border.setColor(context.palette.secondary_foreground);
                    context.canvas.drawRRect(shape, border);
                }
                if (selected_)
                {
                    const float size { theme_check_size * scale };
                    const luil::rect_f check { preview.x + preview.width - size - 6.0f * scale, preview.y + 6.0f * scale, size, size };
                    rounded(context, check, size / 2.0f, context.palette.accent);
                    luil::draw_centered_glyph(context.canvas, codicons::icon_check, check, icon_font(context, 13.0f), luil::solid_paint(context.palette.accent_foreground));
                }
                const SkFont font { ui_font(context, 12.0f, selected_) };
                const float width { std::min(luil::measure_text(title_, font), box.width) };
                static_cast<void>(luil::draw_text_within(context.canvas, title_, box.x + (box.width - width) / 2.0f,
                    box.y + box.height - theme_caption_height * scale + luil::centered_text_baseline(font, theme_caption_height * scale), width, font,
                    luil::solid_paint(context.palette.primary_foreground)));
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::radio_button, .name = title_, .selected = selected_ };
            }

        private:
            theme_preference theme_ { theme_preference::system };
            bool selected_ { false };
            std::u8string title_ {};
        };

        class theme_picker_element final : public luil::ui_element
        {
        public:
            explicit theme_picker_element(const theme_preference selected)
                : ui_element { { kind_segmented, u8"settings-theme" } }
            {
                set_focus_group(luil::focus_axis::horizontal);
                set_focus_entry({ kind_theme_option, u8"settings-theme-" + std::u8string { theme_preference_name(selected) } });
                add_child(std::make_unique<theme_option_element>(theme_preference::light, selected == theme_preference::light, tr(u8"settings.appearance.theme_light")));
                add_child(std::make_unique<theme_option_element>(theme_preference::system, selected == theme_preference::system, tr(u8"settings.appearance.theme_system")));
                add_child(std::make_unique<theme_option_element>(theme_preference::dark, selected == theme_preference::dark, tr(u8"settings.appearance.theme_dark")));
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                const float gap { theme_gap * context.scale };
                const float width { std::max(context.slot.width - gap * 2.0f, 0.0f) / 3.0f };
                float x { context.slot.x };
                for (const auto& child : children())
                {
                    child->arrange(context.for_child({ x, context.slot.y, width, context.slot.height }));
                    x += width + gap;
                }
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                draw_children(context, interaction);
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::radio_group, .name = tr(u8"settings.general.theme") };
            }
        };

        class font_combo_element final : public luil::ui_element
        {
        public:
            font_combo_element(const settings_view& view, const bool code)
                : ui_element { { kind_font_combo, code ? u8"settings-code-font-combo" : u8"settings-ui-font-combo" } }
                , code_ { code }
                , open_ { view.font_list.has_value() && view.font_list->code == code }
            {
                const std::u8string owner { code ? u8"settings-code-font" : u8"settings-ui-font" };
                const luil::text_input_config input_config { .placeholder = tr(u8"settings.appearance.font_placeholder") };
                const luil::ui_element_id input_id { code ? kind_code_font_input : kind_ui_font_input, owner };
                auto input { std::make_unique<luil::text_input_element>(input_id, code ? view.code_font : view.ui_font, input_config) };
                input->set_access_name(code ? tr(u8"settings.general.code_font") : tr(u8"settings.general.ui_font"));
                add_child(std::move(input));
                auto arrow { make_icon_button(owner + u8"-expand", codicons::icon_chevron_down, tr(u8"settings.appearance.installed_fonts"), {}, true) };
                arrow->set_action(luil::ui_trigger::left_click, [this](const luil::ui_action_context&) { return open_action(open_ == false); });
                add_child(std::move(arrow));
                set_access_handler([this](const luil::access_request& request) -> std::optional<std::vector<luil::input_action>> {
                    if (request.command == luil::access_command::expand || request.command == luil::access_command::collapse)
                        return open_action(request.command == luil::access_command::expand);
                    return std::nullopt;
                });
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                const float arrow { font_arrow_width * context.scale };
                const float input { std::max(context.slot.width - arrow, 0.0f) };
                children()[0]->arrange(context.for_child({ context.slot.x, context.slot.y, input, context.slot.height }));
                children()[1]->arrange(context.for_child({ context.slot.x + input, context.slot.y, arrow, context.slot.height }));
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                draw_children(context, interaction);
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::combo_box, .name = code_ ? tr(u8"settings.appearance.choose_code_font") : tr(u8"settings.appearance.choose_ui_font"), .expanded = open_ };
            }

        private:
            std::vector<luil::input_action> open_action(const bool open) const
            {
                if (open == false)
                    return { message(close_font_list_intent {}) };
                const luil::rect_f box { bounds() };
                return { message(open_font_list_intent { { code_, box.x, box.y + box.height, box.width } }) };
            }

            bool code_ { false };
            bool open_ { false };
        };

        std::u8string folded(std::u8string text)
        {
            for (char8_t& value : text)
                if (value < 0x80)
                    value = static_cast<char8_t>(std::tolower(static_cast<unsigned char>(value)));
            return text;
        }
    } // namespace

    std::unique_ptr<luil::ui_element> make_theme_picker(const theme_preference selected)
    {
        return std::make_unique<theme_picker_element>(selected);
    }

    std::unique_ptr<luil::ui_element> make_font_combo(const settings_view& view, const bool code)
    {
        return std::make_unique<font_combo_element>(view, code);
    }

    built_popup build_font_popup(const settings_view& view, const float scale)
    {
        const font_list_state& anchor { *view.font_list };
        const luil::text_input_view& input { anchor.code ? view.code_font : view.ui_font };
        // 설치 글꼴 조회는 시스템 호출이므로 프레임마다 반복하지 않는다.
        static const std::vector<std::u8string> installed { luil::installed_font_families() };
        const std::u8string query { folded(std::u8string { input.displayed_text() }) };
        luil::virtual_list_config config {};
        config.edges.shadows = true;
        config.owner = u8"settings-font-list";
        config.row_height = font_row_height;
        config.items.push_back({ .key = u8"font-default", .label = tr(u8"settings.appearance.default_font") });
        for (const std::u8string& family : installed)
            if (query.empty() || folded(family).find(query) != std::u8string::npos)
                config.items.push_back({ .key = family, .label = family });
        config.scroll_offset = view.font_scroll;
        config.scroll = [](const float delta) { return message(font_list_scroll_intent { delta, false }); };
        config.scroll_to = [](const float offset) { return message(font_list_scroll_intent { offset, true }); };
        config.build_row = [code = anchor.code](const luil::virtual_list_item& item, std::size_t, bool) {
            const std::u8string family { item.key == u8"font-default" ? std::u8string {} : item.key };
            return make_text_button(u8"font-option-" + item.key, item.label, luil::text_button_visual::normal, message(select_font_intent { code, family }));
        };
        config.select = [code = anchor.code](const std::u8string& key) { return message(select_font_intent { code, key == u8"font-default" ? std::u8string {} : key }); };
        const float list_height { static_cast<float>(std::min(config.items.size(), font_visible_rows)) * font_row_height };

        built_popup popup {};
        popup.id = u8"font-list";
        popup.font = true;
        popup.x = anchor.x / scale;
        popup.y = anchor.y / scale + 3.0f;
        popup.width = std::max(anchor.width / scale, 240.0f);
        popup.height = list_height + font_search_height + font_popup_padding * 3.0f;
        const luil::stack_config column_config { .spacing = font_popup_padding, .padding = luil::edge_insets::all(font_popup_padding) };
        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"font-list-column" }, column_config) };
        const luil::ui_element_id search { anchor.code ? kind_code_font_input : kind_ui_font_input, u8"settings-font-search" };
        const luil::text_input_config filter_config { .placeholder = tr(u8"settings.appearance.search_fonts"), .leading_glyph = codicons::icon_search };
        auto filter { std::make_unique<luil::text_input_element>(search, input, filter_config) };
        filter->set_access_name(tr(u8"settings.appearance.search_fonts"));
        column->add(std::move(filter), font_search_height);
        column->add(std::make_unique<luil::virtual_list_element>(std::move(config)), list_height);
        auto panel { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_panel, u8"font-list-panel" }, floating_panel_config(6.0f, 0.3f)) };
        panel->set_content(std::move(column));
        panel->set_focus_trap(true);
        panel->set_focus_entry(search);
        panel->set_dismiss_action(action_of(message(close_font_list_intent {})));
        popup.tree = std::make_shared<const luil::ui_tree>(luil::make_arranged_tree(std::move(panel), { 0.0f, 0.0f, popup.width * scale, popup.height * scale }, scale));
        return popup;
    }
} // namespace cyella::ui
