#include "presentation/ui/settings/tab_style_ui.h"

#include "domain/text_catalog.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/repository_tabs_element.h"
#include "presentation/ui/ui_helpers.h"

#include "luil/ui/draw_primitives.h"
#include "luil/ui/tab_bar_element.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace cyella::ui {
    namespace {
        constexpr float card_padding { 10.0f };
        constexpr float card_radius { 7.0f };
        constexpr float card_title_height { 20.0f };
        constexpr float card_gap { 6.0f };
        constexpr float card_check_size { 18.0f };
        constexpr float card_spacing { 8.0f };
        // 견본 막대와 그 밑의 도구 막대 띠다. 띠가 있어야 선택된 탭이 아래와 이어지는 것이 보인다.
        constexpr float preview_bar_height { 40.0f };
        constexpr float preview_strip_height { 10.0f };
        constexpr float preview_radius { 5.0f };
        constexpr float preview_tab_width { 150.0f };
        constexpr float preview_tab_min_width { 96.0f };
        constexpr float card_height { card_padding * 2.0f + card_title_height + card_gap + preview_bar_height + preview_strip_height };

        struct preview_tab
        {
            std::u8string label {};
            tab_marker marker { tab_marker::none };
            std::uint32_t changed_files { 0 };
        };

        // 견본 막대다. 첫 탭이 고른 탭이고 둘째 탭에 포인터가 올라간 것처럼 그린다.
        std::unique_ptr<luil::ui_element> make_preview_bar(
            const std::u8string& owner, const std::u8string& name, const tab_shape shape, const tab_marker_place place, const std::vector<preview_tab>& tabs)
        {
            luil::tab_bar_config config {};
            config.owner = owner;
            config.tab_width = preview_tab_width;
            config.min_tab_width = preview_tab_min_width;
            config.style = repository_tab_style(shape, place);
            for (std::size_t index { 0 }; index < tabs.size(); ++index)
            {
                luil::tab_item item { .key = owner + u8"-" + number_text(index), .label = tabs[index].label, .icon = codicons::icon_repo, .closable = true };
                apply_tab_marker(item, tabs[index].marker, tabs[index].changed_files, place);
                config.items.push_back(std::move(item));
            }
            config.selected = owner + u8"-0";
            config.preview = luil::tab_bar_preview { .name = name, .hovered = owner + u8"-1" };
            return std::make_unique<luil::tab_bar_element>(std::move(config));
        }

        // 견본 카드 한 장이다: 위에 이름과 한 줄 설명, 아래에 견본 막대와 도구 막대 띠. 고른 카드는 강조 테와 체크다.
        class tab_style_option_element final : public luil::ui_element
        {
        public:
            tab_style_option_element(std::u8string owner, std::u8string title, std::u8string description, const bool selected, luil::input_action action, std::unique_ptr<luil::ui_element> preview)
                : ui_element { { kind_tab_style_option, std::move(owner) } }
                , title_ { std::move(title) }
                , description_ { std::move(description) }
                , selected_ { selected }
            {
                set_cursor(luil::ui_cursor::hand);
                set_text_tooltip(*this, description_);
                set_action(luil::ui_trigger::left_click, action_of(std::move(action)));
                preview_ = preview.get();
                add_child(std::move(preview));
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                const float scale { context.scale };
                const luil::rect_f frame { preview_frame(scale) };
                preview_->arrange(context.for_child({ frame.x, frame.y, frame.width, preview_bar_height * scale }));
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const float scale { context.scale };
                const luil::rect_f box { bounds() };
                const SkRRect card { SkRRect::MakeRectXY(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), card_radius * scale, card_radius * scale) };
                if (interaction.hovered == id() && selected_ == false)
                    context.canvas.drawRRect(card, luil::solid_paint(context.palette.button_hover_background));

                const SkFont title_font { ui_font(context, 12.0f, true) };
                const float text_x { box.x + card_padding * scale };
                const float baseline { box.y + card_padding * scale + luil::centered_text_baseline(title_font, card_title_height * scale) };
                const float limit { box.width - card_padding * 2.0f * scale - (card_check_size + 6.0f) * scale };
                const float title_width { std::min(luil::measure_text(title_, title_font), limit) };
                static_cast<void>(luil::draw_text_within(context.canvas, title_, text_x, baseline, title_width, title_font, luil::solid_paint(context.palette.primary_foreground)));
                const SkFont description_font { ui_font(context, 11.5f) };
                const float description_x { text_x + title_width + 10.0f * scale };
                static_cast<void>(luil::draw_text_within(context.canvas, description_, description_x, baseline, std::max(limit - title_width - 10.0f * scale, 0.0f), description_font,
                    luil::solid_paint(context.palette.secondary_foreground)));

                // 견본은 둥근 판 안에서 그린다. 막대 밑 띠는 도구 막대(창 바탕)다 — 선택된 탭이 그 띠와 한 판이다.
                const luil::rect_f frame { preview_frame(scale) };
                const SkRRect shape { SkRRect::MakeRectXY(SkRect::MakeXYWH(frame.x, frame.y, frame.width, frame.height), preview_radius * scale, preview_radius * scale) };
                context.canvas.save();
                context.canvas.clipRRect(shape, true);
                context.canvas.drawRect(SkRect::MakeXYWH(frame.x, frame.y, frame.width, frame.height), luil::solid_paint(context.palette.window_background));
                preview_->draw(context, interaction);
                context.canvas.restore();
                SkPaint frame_border { luil::solid_paint(context.palette.divider) };
                frame_border.setStyle(SkPaint::kStroke_Style);
                frame_border.setStrokeWidth(scale);
                frame_border.setAntiAlias(true);
                context.canvas.drawRRect(shape, frame_border);

                SkPaint border { luil::solid_paint(selected_ ? context.palette.accent : context.palette.divider) };
                border.setStyle(SkPaint::kStroke_Style);
                border.setStrokeWidth((selected_ ? 2.0f : 1.0f) * scale);
                border.setAntiAlias(true);
                context.canvas.drawRRect(card, border);
                if (selected_)
                {
                    const float size { card_check_size * scale };
                    const luil::rect_f check { box.x + box.width - card_padding * scale - size, box.y + card_padding * scale + (card_title_height * scale - size) / 2.0f, size, size };
                    SkPaint disc { luil::solid_paint(context.palette.accent) };
                    disc.setAntiAlias(true);
                    context.canvas.drawCircle(check.x + size / 2.0f, check.y + size / 2.0f, size / 2.0f, disc);
                    luil::draw_centered_glyph(context.canvas, codicons::icon_check, check, icon_font(context, 12.0f), luil::solid_paint(context.palette.accent_foreground));
                }
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::radio_button, .name = title_, .selected = selected_ };
            }

        private:
            [[nodiscard]] luil::rect_f preview_frame(const float scale) const noexcept
            {
                const luil::rect_f box { bounds() };
                const float top { box.y + (card_padding + card_title_height + card_gap) * scale };
                return { box.x + card_padding * scale, top, std::max(box.width - card_padding * 2.0f * scale, 0.0f), (preview_bar_height + preview_strip_height) * scale };
            }

            std::u8string title_ {};
            std::u8string description_ {};
            bool selected_ { false };
            luil::ui_element* preview_ { nullptr };
        };

        // 견본 카드를 세로로 쌓은 배타 선택 묶음이다. ↑/↓가 카드 사이를 옮긴다.
        class tab_style_picker_element final : public luil::ui_element
        {
        public:
            tab_style_picker_element(std::u8string owner, std::u8string name, const std::u8string& selected_owner, std::vector<std::unique_ptr<luil::ui_element>> options)
                : ui_element { { kind_tab_style_picker, std::move(owner) } }
                , name_ { std::move(name) }
            {
                set_focus_group(luil::focus_axis::vertical);
                set_focus_entry({ kind_tab_style_option, selected_owner });
                for (std::unique_ptr<luil::ui_element>& option : options)
                    add_child(std::move(option));
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                float y { context.slot.y };
                for (const auto& child : children())
                {
                    child->arrange(context.for_child({ context.slot.x, y, context.slot.width, card_height * context.scale }));
                    y += (card_height + card_spacing) * context.scale;
                }
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                draw_children(context, interaction);
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::radio_group, .name = name_ };
            }

        private:
            std::u8string name_ {};
        };

        std::u8string shape_owner(const tab_shape shape)
        {
            return u8"settings-tab-shape-" + std::u8string { tab_shape_name(shape) };
        }

        std::u8string marker_owner(const tab_marker_place place)
        {
            return u8"settings-tab-marker-" + std::u8string { tab_marker_place_name(place) };
        }
    } // namespace

    std::unique_ptr<luil::ui_element> make_tab_shape_picker(const settings_view& view)
    {
        // 모양 견본은 지금 고른 표시 자리로 그린다. 바뀐 탭과 실행 중인 탭이 함께 있어야 모양이 표시와 어울리는지 보인다.
        const std::vector<preview_tab> tabs {
            { tr(u8"settings.tabs.preview_first"), tab_marker::none, 0 },
            { tr(u8"settings.tabs.preview_second"), tab_marker::changes, 3 },
            { tr(u8"settings.tabs.preview_third"), tab_marker::running, 0 },
        };
        std::vector<std::unique_ptr<luil::ui_element>> options {};
        const auto add = [&](const tab_shape shape, const std::u8string& title, const std::u8string& description) {
            const std::u8string owner { shape_owner(shape) };
            options.push_back(std::make_unique<tab_style_option_element>(owner, title, description, view.tab_shape_choice == shape, message(set_tab_shape_intent { shape }),
                make_preview_bar(owner + u8"-preview", tr(u8"settings.tabs.preview_name", { { u8"name", title } }), shape, view.tab_marker_choice, tabs)));
        };
        add(tab_shape::connected, tr(u8"settings.tabs.shape_connected"), tr(u8"settings.tabs.shape_connected_help"));
        add(tab_shape::flat, tr(u8"settings.tabs.shape_flat"), tr(u8"settings.tabs.shape_flat_help"));
        return std::make_unique<tab_style_picker_element>(u8"settings-tab-shape", tr(u8"settings.tabs.shape"), shape_owner(view.tab_shape_choice), std::move(options));
    }

    std::unique_ptr<luil::ui_element> make_tab_marker_picker(const settings_view& view)
    {
        // 표시 견본은 바뀐 탭과 입력을 기다리는 탭이다. 둘 다 고르지 않은 탭이라야 표시가 제 일을 하는 자리가 보인다.
        const std::vector<preview_tab> tabs {
            { tr(u8"settings.tabs.preview_first"), tab_marker::none, 0 },
            { tr(u8"settings.tabs.preview_second"), tab_marker::changes, 3 },
            { tr(u8"settings.tabs.preview_third"), tab_marker::needs_input, 0 },
        };
        std::vector<std::unique_ptr<luil::ui_element>> options {};
        const auto add = [&](const tab_marker_place place, const std::u8string& title, const std::u8string& description) {
            const std::u8string owner { marker_owner(place) };
            options.push_back(std::make_unique<tab_style_option_element>(owner, title, description, view.tab_marker_choice == place, message(set_tab_marker_intent { place }),
                make_preview_bar(owner + u8"-preview", tr(u8"settings.tabs.preview_name", { { u8"name", title } }), view.tab_shape_choice, place, tabs)));
        };
        add(tab_marker_place::badge, tr(u8"settings.tabs.marker_badge"), tr(u8"settings.tabs.marker_badge_help"));
        add(tab_marker_place::icon_corner, tr(u8"settings.tabs.marker_icon_corner"), tr(u8"settings.tabs.marker_icon_corner_help"));
        add(tab_marker_place::close_slot, tr(u8"settings.tabs.marker_close_slot"), tr(u8"settings.tabs.marker_close_slot_help"));
        return std::make_unique<tab_style_picker_element>(u8"settings-tab-marker", tr(u8"settings.tabs.marker"), marker_owner(view.tab_marker_choice), std::move(options));
    }

    float tab_style_picker_height(const std::size_t count) noexcept
    {
        if (count == 0)
            return 0.0f;
        return static_cast<float>(count) * card_height + static_cast<float>(count - 1) * card_spacing;
    }
} // namespace cyella::ui
