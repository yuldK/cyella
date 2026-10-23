#include "presentation/ui/notification_ui.h"

#include "domain/text_catalog.h"
#include "presentation/dialog_intents.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/segmented_control.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"

#include "luil/theme/ui_theme.h"
#include "luil/ui/draw_primitives.h"
#include "luil/ui/panel_element.h"
#include "luil/ui/stack_element.h"
#include "luil/ui/ui_tree.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace cyella::ui {
    namespace {
        constexpr float card_padding { 10.0f };
        constexpr float icon_column { 22.0f };
        constexpr float card_button { 24.0f };
        constexpr float card_gap { 6.0f };
        constexpr float text_line_height { 18.0f };
        constexpr float action_height { 26.0f };
        constexpr float action_line_gap { 4.0f };
        constexpr float detail_line_height { 16.0f };
        constexpr std::size_t detail_visible_lines { 8 };
        constexpr float toast_maximum_width { 380.0f };
        constexpr float toast_margin { 16.0f };
        constexpr float toast_spacing { 10.0f };
        // 토스트는 모서리를 크게 굴리고 그림자를 짙게 드리워 화면 위에 떠 있는 카드로 읽히게 한다.
        constexpr float toast_radius { 10.0f };
        constexpr float toast_shadow { 0.55f };
        // 짙기를 겹쳐 그릴 때 그림자까지 담는 둘레다 (논리 픽셀).
        constexpr float toast_layer_outset { 32.0f };
        // 이보다 덜 나온 토스트는 짓지 않는다. 다 물러난 카드가 누름을 받지 않게 한다.
        constexpr float toast_presence_minimum { 0.02f };
        constexpr float center_maximum_width { 420.0f };
        constexpr float center_maximum_height { 560.0f };
        constexpr float center_header_height { 44.0f };
        constexpr float center_empty_height { 132.0f };
        constexpr float center_margin { 8.0f };
        // 센터는 토스트와 같은 카드를 한 판 위에 쌓는다. 판은 창 바탕, 카드는 표면색이라 카드가 판 위에 떠 보인다.
        //  - popup 창은 창 바탕색으로 지운 불투명한 직사각형이다. 판도 같은 색이라 둥근 모서리 바깥이 튀지 않는다.
        //    그림자는 창 밖이라 그릴 수 없으므로 두지 않는다.
        constexpr float center_radius { 10.0f };
        constexpr float center_list_padding { 8.0f };
        constexpr float center_filter_width { 210.0f };
        constexpr float center_day_height { 28.0f };
        constexpr float center_card_gap { 6.0f };
        constexpr float center_card_radius { 10.0f };
        constexpr float center_card_padding { 10.0f };
        constexpr float center_card_left { 12.0f };
        constexpr float center_disc { 26.0f };
        constexpr float center_disc_gap { 10.0f };
        constexpr float center_title_height { 20.0f };
        // 머리 줄 오른쪽에 시각이 서는 자리다. 포인터를 올리면 그 자리에 [×]가 선다.
        constexpr float center_time_width { 72.0f };
        constexpr float center_close_size { 22.0f };
        constexpr float center_close_inset { 8.0f };
        constexpr float center_toggle_height { 24.0f };
        constexpr float center_actions_gap { 8.0f };
        constexpr float center_details_padding { 6.0f };
        constexpr float center_details_inset { 10.0f };
        constexpr float center_details_radius { 6.0f };

        const text_style body_style { .size = 12.0f };

        char32_t severity_glyph(const notification_severity severity) noexcept
        {
            switch (severity)
            {
            case notification_severity::warning:
                return codicons::icon_warning;
            case notification_severity::error:
                return codicons::icon_error;
            case notification_severity::info:
                break;
            }
            return codicons::icon_info;
        }

        color_selector severity_color(const notification_severity severity)
        {
            return [severity](const luil::ui_color_palette& palette) {
                switch (severity)
                {
                case notification_severity::warning:
                    return palette.warning_accent;
                case notification_severity::error:
                    return palette.error_accent;
                case notification_severity::info:
                    break;
                }
                return palette.accent;
            };
        }

        color_selector dim_color()
        {
            return [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; };
        }

        // 토스트 머리 줄 오른쪽의 단추([자세히]·[×])를 뺀 글 폭이다.
        float text_width(const notification_view& value, const float width)
        {
            const float buttons { card_button * (value.details.empty() ? 1.0f : 2.0f) };
            return std::max(width - card_padding * 2.0f - icon_column - card_gap - buttons, 40.0f);
        }

        float action_width(const notification_action& action)
        {
            return text_button_width(action.label) + 24.0f;
        }

        // 조치 단추가 `available` 폭의 한 줄에 들어가지 않으면 한 줄에 하나씩 세운다 (커밋 전 경고는 프로필마다 단추가 선다).
        bool actions_stacked(const notification_view& value, const float available)
        {
            float total { 0.0f };
            for (const notification_action& action : value.actions)
                total += action_width(action);
            total += card_gap * static_cast<float>(value.actions.size() > 0 ? value.actions.size() - 1 : 0);
            return total > available;
        }

        float actions_height(const notification_view& value, const float available)
        {
            if (value.actions.empty())
                return 0.0f;
            if (actions_stacked(value, available) == false)
                return action_height;
            const auto count { static_cast<float>(value.actions.size()) };
            return count * action_height + (count - 1.0f) * action_line_gap;
        }

        std::size_t detail_line_count(const notification_view& value) noexcept
        {
            if (value.expanded == false || value.details.empty())
                return 0;
            return std::min(value.details.size(), detail_visible_lines) + (value.details.size() > detail_visible_lines ? 1u : 0u);
        }

        float message_height(const notification_view& value, const float width)
        {
            float height { value.title.empty() ? 0.0f : text_line_height };
            height += wrapped_text_height(value.text, text_width(value, width), body_style, text_line_height);
            return std::max(height, card_button);
        }

        std::unique_ptr<luil::stack_element> make_stack(std::u8string owner, const luil::stack_config& config)
        {
            return std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, std::move(owner) }, config);
        }

        // 조치 단추 줄이다. 오른쪽에 붙이고, 들어가지 않으면 한 줄에 하나씩 세운다.
        //  - 센터의 카드는 마지막 단추만 강조한다 (주 조치). 토스트는 모두 강조한다.
        std::unique_ptr<luil::ui_element> make_actions(const std::u8string& owner, const notification_view& value, const float available, const bool emphasize_last)
        {
            const bool stacked { actions_stacked(value, available) };
            luil::stack_config actions_config {};
            actions_config.direction = stacked ? luil::stack_direction::column : luil::stack_direction::row;
            actions_config.spacing = stacked ? action_line_gap : card_gap;
            actions_config.cross_alignment = stacked ? luil::stack_alignment::end : luil::stack_alignment::stretch;
            auto actions { make_stack(owner + u8"-actions", actions_config) };
            if (stacked == false)
                actions->add_flexible_gap();
            for (std::size_t index { 0 }; index < value.actions.size(); ++index)
            {
                const notification_action& action { value.actions[index] };
                const notification_action_intent pressed { .key = action.key, .id = value.operation, .notification = value.id };
                const bool accent { emphasize_last == false || index + 1 == value.actions.size() };
                auto button { make_text_button(owner + u8"-action-" + action.key, action.label, accent ? luil::text_button_visual::accent : luil::text_button_visual::normal, message(pressed)) };
                if (stacked)
                    actions->add(std::move(button), { .length = action_height, .cross_length = std::min(action_width(action), available) });
                else
                    actions->add(std::move(button), action_width(action));
            }
            return actions;
        }

        // [자세히]가 편 줄이다. 넘치는 줄은 개수만 적는다.
        std::unique_ptr<luil::stack_element> make_detail_lines(const std::u8string& owner, const notification_view& value, const luil::edge_insets padding)
        {
            auto details { make_stack(owner + u8"-details-list", { .padding = padding }) };
            const std::size_t shown { std::min(value.details.size(), detail_visible_lines) };
            for (std::size_t index { 0 }; index < shown; ++index)
            {
                auto line { make_text(owner + u8"-detail-" + number_text(index), value.details[index], text_style { .size = 11.0f, .color = dim_color(), .code = true }) };
                line->set_tooltip(value.details[index]);
                details->add(std::move(line), detail_line_height);
            }
            if (value.details.size() > shown)
                details->add(
                    make_text(owner + u8"-detail-more", tr(u8"tree.notification.more_lines", { { u8"count", value.details.size() - shown } }), text_style { .size = 11.0f, .color = dim_color() }),
                    detail_line_height);
            return details;
        }

        // 토스트 한 장이다: 아이콘 · 머리말과 글 · [자세히] · [×], 그 아래 조치 단추와 편 줄.
        std::unique_ptr<luil::ui_element> make_card(const notification_view& value, const float width)
        {
            const std::u8string owner { u8"toast-" + number_text(value.id) };
            auto column { make_stack(owner + u8"-column", { .spacing = card_gap, .padding = luil::edge_insets::all(card_padding) }) };

            auto head { make_stack(owner + u8"-head", { .direction = luil::stack_direction::row, .cross_alignment = luil::stack_alignment::start }) };
            head->add(std::make_unique<icon_element>(luil::ui_element_id { kind_label, owner + u8"-icon" }, severity_glyph(value.severity), severity_color(value.severity), 16.0f),
                { .length = icon_column, .cross_length = text_line_height });
            auto body { make_stack(owner + u8"-message", {}) };
            if (value.title.empty() == false)
                body->add(make_text(owner + u8"-title", value.title, text_style { .size = 12.0f, .color = severity_color(value.severity), .bold = true }), text_line_height);
            if (value.text.empty() == false)
                body->add(make_wrapped_text(owner + u8"-text", value.text, body_style, text_line_height), wrapped_text_height(value.text, text_width(value, width), body_style, text_line_height));
            head->add_flexible(std::move(body));
            head->add_gap(card_gap);
            if (value.details.empty() == false)
            {
                const std::u8string tooltip { value.expanded ? tr(u8"tree.notification.collapse") : tr(u8"tree.notification.details") };
                head->add(make_icon_button(owner + u8"-details", value.expanded ? codicons::icon_chevron_up : codicons::icon_chevron_down, tooltip,
                              message(toggle_notification_details_intent { value.id }), true, value.expanded),
                    { .length = card_button, .cross_length = card_button });
            }
            head->add(make_icon_button(owner + u8"-close", codicons::icon_close, tr(u8"tree.notification.clear"), message(dismiss_notification_intent { value.id })),
                { .length = card_button, .cross_length = card_button });
            column->add(std::move(head), message_height(value, width));

            const float available { width - card_padding * 2.0f };
            if (value.actions.empty() == false)
                column->add(make_actions(owner, value, available, false), actions_height(value, available));

            if (const std::size_t lines { detail_line_count(value) }; lines > 0)
                column->add(make_detail_lines(owner, value, luil::edge_insets { icon_column, 0.0f, 0.0f, 0.0f }), static_cast<float>(lines) * detail_line_height);
            return column;
        }

        // ---------------------------------------------------------------- 알림 센터의 카드

        // 카드 안에서 심각도 원과 바깥 여백을 뺀 글 폭이다.
        float center_body_width(const float card_width)
        {
            return std::max(card_width - center_card_left - center_disc - center_disc_gap - center_card_padding, 40.0f);
        }

        // 머리말이 없으면 글의 첫 줄이 시각과 한 줄을 나눠 쓰므로 글 전체를 그 폭으로 접는다.
        float center_text_width(const notification_view& value, const float card_width)
        {
            const float body { center_body_width(card_width) };
            return value.title.empty() ? std::max(body - center_time_width, 40.0f) : body;
        }

        float center_head_height(const notification_view& value, const float card_width)
        {
            float height { value.title.empty() ? 0.0f : center_title_height };
            if (value.text.empty() == false)
                height += wrapped_text_height(value.text, center_text_width(value, card_width), body_style, text_line_height);
            return std::max(height, center_title_height);
        }

        float center_details_height(const std::size_t lines)
        {
            return static_cast<float>(lines) * detail_line_height + center_details_padding * 2.0f;
        }

        float center_card_height(const notification_view& value, const float card_width)
        {
            float height { center_card_padding * 2.0f + center_head_height(value, card_width) };
            if (value.details.empty() == false)
                height += card_gap + center_toggle_height;
            if (const std::size_t lines { detail_line_count(value) }; lines > 0)
                height += card_gap + center_details_height(lines);
            if (value.actions.empty() == false)
                height += center_actions_gap + actions_height(value, center_body_width(card_width));
            return height;
        }

        bool holds(const luil::ui_element& element, const luil::ui_element_id& target)
        {
            if (element.id() == target)
                return true;
            return std::any_of(element.children().begin(), element.children().end(), [&target](const std::unique_ptr<luil::ui_element>& child) { return holds(*child, target); });
        }

        // 알림 센터의 카드 한 장이다. 바탕과 심각도 원은 직접 그리고, 머리 줄 오른쪽에는 시각과 [×] 가운데 하나만 그린다.
        //  - [×]는 포인터가 카드 위에 있거나 초점이 카드 안에 있을 때만 보인다. 늘 서 있으면 시각과 자리를 다투고, 카드마다 ×가 줄지어 시끄럽다.
        //  - 숨은 [×]도 tree에는 남는다. 키보드 초점이 가면 보이고, 보조 기술은 언제든 누를 수 있다.
        class center_card_element final : public luil::ui_element
        {
        public:
            center_card_element(luil::ui_element_id id, const notification_severity severity, std::unique_ptr<luil::ui_element> content, std::unique_ptr<luil::ui_element> time,
                std::unique_ptr<luil::ui_element> close)
                : ui_element { std::move(id) }
                , severity_ { severity }
                , content_ { content.get() }
                , time_ { time.get() }
                , close_ { close.get() }
            {
                // 글 위에서도 카드가 hover의 임자가 되어야 [×]가 선다.
                set_hit_opaque(true);
                add_child(std::move(content));
                add_child(std::move(time));
                add_child(std::move(close));
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                const float scale { context.scale };
                const luil::rect_f box { context.slot };
                content_->arrange(context.for_child(box));
                const float right { box.x + box.width };
                time_->arrange(
                    context.for_child({ right - (center_card_padding + center_time_width) * scale, box.y + center_card_padding * scale, center_time_width * scale, center_title_height * scale }));
                const float close { center_close_size * scale };
                const float top { box.y + (center_card_padding + (center_title_height - center_close_size) / 2.0f) * scale };
                close_->arrange(context.for_child({ right - center_close_inset * scale - close, top, close, close }));
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const float scale { context.scale };
                const luil::rect_f box { bounds() };
                const SkRRect shape { SkRRect::MakeRectXY(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), center_card_radius * scale, center_card_radius * scale) };
                SkPaint fill { luil::solid_paint(context.palette.surface_background) };
                fill.setAntiAlias(true);
                context.canvas.drawRRect(shape, fill);
                const bool hovered { interaction.hovered != luil::ui_element_id {} && holds(*this, interaction.hovered) };
                const bool focused { interaction.focused != luil::ui_element_id {} && holds(*this, interaction.focused) };
                const bool engaged { hovered || focused };
                if (engaged)
                {
                    SkPaint wash { luil::solid_paint(luil::with_alpha(context.palette.primary_foreground, 0.04f)) };
                    wash.setAntiAlias(true);
                    context.canvas.drawRRect(shape, wash);
                }
                // 밝은 테마의 흰 카드는 밝은 판 위에서 묻힌다. 그때만 테를 두른다.
                if (luil::is_dark_background(context.palette.surface_background) == false)
                {
                    SkPaint border { luil::solid_paint(context.palette.divider) };
                    border.setStyle(SkPaint::kStroke_Style);
                    border.setStrokeWidth(scale);
                    border.setAntiAlias(true);
                    context.canvas.drawRRect(shape, border);
                }

                // 심각도는 옅게 칠한 원 안의 글리프다. 글리프가 뜻을 지니므로 고대비에서 색이 접혀도 남는다.
                const luil::ui_color tone { severity_color(severity_)(context.palette) };
                const float radius { center_disc * scale / 2.0f };
                const float center_x { box.x + center_card_left * scale + radius };
                const float center_y { box.y + (center_card_padding + center_title_height / 2.0f) * scale };
                SkPaint disc { luil::solid_paint(luil::with_alpha(tone, 0.16f)) };
                disc.setAntiAlias(true);
                context.canvas.drawCircle(center_x, center_y, radius, disc);
                luil::draw_centered_glyph(
                    context.canvas, severity_glyph(severity_), { center_x - radius, center_y - radius, radius * 2.0f, radius * 2.0f }, icon_font(context, 15.0f), luil::solid_paint(tone));

                content_->draw(context, interaction);
                if (engaged)
                    close_->draw(context, interaction);
                else
                    time_->draw(context, interaction);
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::group, .name = access_name() };
            }

        private:
            notification_severity severity_ { notification_severity::info };
            luil::ui_element* content_ { nullptr };
            luil::ui_element* time_ { nullptr };
            luil::ui_element* close_ { nullptr };
        };

        // 센터의 카드 한 장이다: 심각도 원 · 머리말과 글(오른쪽에 시각, hover면 [×]) · [자세히] · 편 줄 · 조치 단추.
        std::unique_ptr<luil::ui_element> make_center_card(const notification_view& value, const float card_width)
        {
            const std::u8string owner { u8"notification-" + number_text(value.id) };
            const float body_width { center_body_width(card_width) };
            const luil::stack_config column_config {
                .padding = luil::edge_insets { center_card_left + center_disc + center_disc_gap, center_card_padding, center_card_padding, center_card_padding },
            };
            auto column { make_stack(owner + u8"-column", column_config) };

            auto head { make_stack(owner + u8"-message", {}) };
            if (value.title.empty() == false)
            {
                auto title { make_stack(owner + u8"-title-row", { .direction = luil::stack_direction::row }) };
                title->add_flexible(make_text(owner + u8"-title", value.title, text_style { .size = 12.0f, .bold = true }));
                title->add_gap(center_time_width);
                head->add(std::move(title), center_title_height);
            }
            if (value.text.empty() == false)
            {
                // 머리말이 있으면 글은 그 아래의 흐린 설명이고, 없으면 글이 곧 머리 줄이다.
                const text_style style { .size = 12.0f, .color = value.title.empty() ? color_selector {} : dim_color() };
                auto text { make_stack(owner + u8"-text-row", { .direction = luil::stack_direction::row }) };
                text->add_flexible(make_wrapped_text(owner + u8"-text", value.text, style, text_line_height));
                if (value.title.empty())
                    text->add_gap(center_time_width);
                head->add(std::move(text), wrapped_text_height(value.text, center_text_width(value, card_width), style, text_line_height));
            }
            column->add(std::move(head), center_head_height(value, card_width));

            if (value.details.empty() == false)
            {
                const std::u8string label { value.expanded ? tr(u8"tree.notification.collapse") : tr(u8"tree.notification.details") };
                auto toggle { make_stack(owner + u8"-details-row", { .direction = luil::stack_direction::row }) };
                toggle->add(make_text_button(owner + u8"-details", label, luil::text_button_visual::normal, message(toggle_notification_details_intent { value.id }), true,
                                value.expanded ? codicons::icon_chevron_up : codicons::icon_chevron_down),
                    text_button_width(label, true));
                toggle->add_flexible_gap();
                column->add_gap(card_gap);
                column->add(std::move(toggle), center_toggle_height);
            }

            if (const std::size_t lines { detail_line_count(value) }; lines > 0)
            {
                luil::panel_config box {};
                box.background = [](const luil::ui_color_palette& palette) { return palette.window_background; };
                box.corner_radius = center_details_radius;
                auto panel { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_panel, owner + u8"-details-box" }, box) };
                panel->set_content(make_detail_lines(owner, value, luil::edge_insets { center_details_inset, center_details_padding, center_details_inset, center_details_padding }));
                column->add_gap(card_gap);
                column->add(std::move(panel), center_details_height(lines));
            }

            if (value.actions.empty() == false)
            {
                column->add_gap(center_actions_gap);
                column->add(make_actions(owner, value, body_width, true), actions_height(value, body_width));
            }

            auto time { make_text(owner + u8"-time", value.time, text_style { .size = 11.0f, .color = dim_color(), .align = 2 }) };
            time->set_tooltip(value.time_tooltip);
            auto close { make_icon_button(owner + u8"-close", codicons::icon_close, tr(u8"tree.notification.clear"), message(dismiss_notification_intent { value.id })) };
            const luil::ui_element_id id { kind_panel, owner };
            auto card { std::make_unique<center_card_element>(id, value.severity, std::move(column), std::move(time), std::move(close)) };
            card->set_access_name(value.title.empty() ? value.text : value.title + u8" — " + value.text);
            return card;
        }

        std::shared_ptr<const luil::ui_tree> arranged(std::unique_ptr<luil::ui_element> content, const float width, const float height, const float scale)
        {
            return std::make_shared<const luil::ui_tree>(luil::make_arranged_tree(std::move(content), { 0.0f, 0.0f, width * scale, height * scale }, scale));
        }

        // 떠오르고 물러나는 토스트 한 장이다. 나와 있는 정도(`presence`)만큼 짙게 그리고, 쌓인 자리의 아래 끝을 넘는 부분은 자른다.
        //  - 자리는 배치가 정한다 (`build_toasts`가 나온 정도만큼 위로 밀어 올린다). 여기서는 짙기와 자르기만 한다.
        //  - 다 나온 토스트는 겹쳐 그리지 않는다. 겹친 판은 글자의 subpixel 그리기를 잃는다.
        class toast_motion_element final : public luil::ui_element
        {
        public:
            toast_motion_element(luil::ui_element_id id, std::unique_ptr<luil::ui_element> content, const float presence, const float clip_bottom)
                : ui_element { std::move(id) }
                , presence_ { presence }
                , clip_bottom_ { clip_bottom }
            {
                add_child(std::move(content));
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                children()[0]->arrange(context.for_child(context.slot));
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const float scale { context.scale };
                const luil::rect_f box { bounds() };
                const float outset { toast_layer_outset * scale };
                context.canvas.save();
                context.canvas.clipRect(SkRect::MakeLTRB(box.x - outset, box.y - outset, box.x + box.width + outset, clip_bottom_ * scale));
                if (presence_ < 1.0f)
                {
                    const SkRect layer { SkRect::MakeLTRB(box.x - outset, box.y - outset, box.x + box.width + outset, box.y + box.height + outset) };
                    context.canvas.saveLayerAlpha(&layer, static_cast<U8CPU>(std::clamp(presence_, 0.0f, 1.0f) * 255.0f + 0.5f));
                }
                draw_children(context, interaction);
                if (presence_ < 1.0f)
                    context.canvas.restore();
                context.canvas.restore();
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::group, .name = children()[0]->accessibility().name };
            }

        private:
            float presence_ { 1.0f };
            float clip_bottom_ { 0.0f };
        };

        // 센터 목록의 한 줄이다. 날 머리글이거나 카드 한 장이다.
        struct center_row
        {
            bool day { false };
            std::size_t entry { 0 };
            float top { 0.0f };
            float bottom { 0.0f };
        };

        std::u8string filter_value(const notification_filter filter)
        {
            switch (filter)
            {
            case notification_filter::warning:
                return u8"notification-filter-warning";
            case notification_filter::error:
                return u8"notification-filter-error";
            case notification_filter::all:
                break;
            }
            return u8"notification-filter-all";
        }

        // 머리의 거르기다. 고른 종류의 알림이 없을 때 목록 자리에 서는 말도 함께 정한다.
        std::unique_ptr<luil::ui_element> make_filter(const notification_center_view& center)
        {
            luil::choice_group_config config {};
            config.owner = u8"notification-filter";
            config.name = tr(u8"tree.notification.filter");
            config.items = {
                { filter_value(notification_filter::all), tr(u8"tree.notification.filter_all", { { u8"count", center.total } }) },
                { filter_value(notification_filter::warning), tr(u8"tree.notification.filter_warning", { { u8"count", center.warnings } }) },
                { filter_value(notification_filter::error), tr(u8"tree.notification.filter_error", { { u8"count", center.errors } }) },
            };
            config.selected = filter_value(center.filter);
            config.select = [](const std::u8string& value) {
                notification_filter filter { notification_filter::all };
                if (value == filter_value(notification_filter::warning))
                    filter = notification_filter::warning;
                else if (value == filter_value(notification_filter::error))
                    filter = notification_filter::error;
                return message(filter_notifications_intent { filter });
            };
            return make_segmented_control(std::move(config));
        }

        // 비었을 때의 자리다. 모두 비었으면 무엇이 모이는 곳인지 함께 적는다.
        std::unique_ptr<luil::ui_element> make_empty(const notification_center_view& center)
        {
            const luil::stack_config config { .spacing = 4.0f, .main_alignment = luil::stack_main_alignment::center };
            auto column { make_stack(u8"notification-center-empty-column", config) };
            const bool filtered { center.total > 0 };
            column->add(make_text(u8"notification-center-empty", filtered ? tr(u8"tree.notification.empty_filtered") : tr(u8"tree.notification.empty"),
                            text_style { .size = 13.0f, .color = dim_color(), .align = 1 }),
                text_line_height);
            if (filtered == false)
                column->add(make_text(u8"notification-center-empty-hint", tr(u8"tree.notification.empty_hint"),
                                text_style { .size = 12.0f, .color = [](const luil::ui_color_palette& palette) { return palette.disabled_foreground; }, .align = 1 }),
                    text_line_height);
            return column;
        }
    } // namespace

    float notification_card_height(const notification_view& value, const float width, const bool center)
    {
        if (center)
            return center_card_height(value, width);
        float height { card_padding * 2.0f + message_height(value, width) };
        const float available { width - card_padding * 2.0f };
        if (const float actions { actions_height(value, available) }; actions > 0.0f)
            height += card_gap + actions;
        if (const std::size_t lines { detail_line_count(value) }; lines > 0)
            height += card_gap + static_cast<float>(lines) * detail_line_height;
        return height;
    }

    std::vector<placed_toast> build_toasts(const view_snapshot& view, const float bottom)
    {
        std::vector<placed_toast> toasts {};
        const float width { std::min(toast_maximum_width, std::max(view.logical_width - toast_margin * 2.0f, 160.0f)) };
        float next_bottom { bottom - toast_margin };
        for (const notification_view& value : view.toasts)
        {
            if (value.presence < toast_presence_minimum)
                continue;
            const float height { notification_card_height(value, width, false) };
            // 창이 낮아 위로 쌓을 자리가 없으면 거기서 멈춘다. 나머지는 알림 센터에서 본다.
            if (next_bottom - height < view.logical_height * 0.25f && toasts.empty() == false)
                break;
            // 나온 만큼만 자리를 차지한다. 떠오르는 카드는 아래 끝 너머에서 올라오며 위의 카드를 밀어 올리고,
            // 물러나는 카드는 자리를 접으며 위의 카드를 끌어내린다. 그래서 쌓인 줄이 한 번에 튀지 않는다.
            const float shown { (height + toast_spacing) * value.presence };
            const std::u8string owner { u8"toast-" + number_text(value.id) };
            auto panel { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_panel, owner + u8"-panel" }, floating_panel_config(toast_radius, toast_shadow)) };
            panel->set_content(make_card(value, width));
            panel->set_access_name(value.title.empty() ? value.text : value.title + u8" — " + value.text);
            panel->set_hit_opaque(true);
            placed_toast toast {};
            toast.rect = { view.logical_width - toast_margin - width, next_bottom - shown + toast_spacing * value.presence, width, height };
            toast.presence = value.presence;
            toast.element = std::make_unique<toast_motion_element>(luil::ui_element_id { kind_panel, owner }, std::move(panel), value.presence, bottom);
            toasts.push_back(std::move(toast));
            next_bottom -= shown;
        }
        // 덜 나온 카드(떠오르거나 물러나는 중)를 먼저 그려 다 나온 카드 밑으로 지나가게 한다.
        std::stable_sort(toasts.begin(), toasts.end(), [](const placed_toast& left, const placed_toast& right) { return left.presence < right.presence; });
        return toasts;
    }

    built_popup build_notification_center_popup(const notification_center_view& center, const view_snapshot& view, const float bottom, const float scale, const luil::virtual_list_element** list)
    {
        const float width { std::min(center_maximum_width, std::max(view.logical_width - center_margin * 2.0f, 200.0f)) };
        const float card_width { width - center_list_padding * 2.0f };
        auto entries { std::make_shared<std::vector<notification_view>>(center.entries) };
        auto rows { std::make_shared<std::vector<center_row>>() };
        float content { 0.0f };
        luil::virtual_list_config config {};
        config.owner = u8"notification-list";
        // 날 머리글은 알림이 두 날 이상에 걸칠 때만 세운다. 카드 줄은 아래 간격까지 품고, 목록의 처음과 끝 줄은 판의 여백을 품는다.
        std::u8string day {};
        for (std::size_t index { 0 }; index < entries->size(); ++index)
        {
            const notification_view& value { (*entries)[index] };
            if (center.show_days && value.day != day)
            {
                day = value.day;
                rows->push_back({ .day = true, .entry = index });
                config.items.push_back({ .key = u8"notification-day-" + number_text(value.id), .label = value.day, .height = center_day_height });
                content += center_day_height;
            }
            const bool first { rows->empty() };
            const bool last { index + 1 == entries->size() };
            const center_row row { .entry = index, .top = first ? center_list_padding : 0.0f, .bottom = last ? center_list_padding : center_card_gap };
            const float height { row.top + notification_card_height(value, card_width, true) + row.bottom };
            rows->push_back(row);
            config.items.push_back({ .key = u8"notification-" + number_text(value.id), .label = value.title.empty() ? value.text : value.title, .height = height });
            content += height;
        }
        const float available { std::max(std::min(center_maximum_height, bottom - view.logical_height * 0.2f) - center_header_height, center_empty_height) };
        const float list_height { entries->empty() ? center_empty_height : std::min(content, available) };

        auto column { make_stack(u8"notification-center-column", {}) };
        const luil::stack_config header_config {
            .direction = luil::stack_direction::row, .spacing = 4.0f, .padding = luil::edge_insets { 14.0f, 0.0f, 8.0f, 0.0f }, .cross_alignment = luil::stack_alignment::center
        };
        auto header { make_stack(u8"notification-center-header", header_config) };
        const std::u8string title { tr(u8"tree.notification.center") };
        header->add(make_text(u8"notification-center-title", title, text_style { .size = 13.0f, .bold = true }), text_button_width(title) - 12.0f);
        if (center.total > 0)
        {
            header->add_gap(8.0f);
            header->add(make_filter(center), { .length = center_filter_width, .cross_length = segmented_control_height });
        }
        header->add_flexible_gap();
        header->add(make_icon_button(u8"notification-center-clear", codicons::icon_clear_all, tr(u8"tree.notification.clear_all"), message(clear_notifications_intent {}), center.total > 0),
            { .length = card_button + 4.0f, .cross_length = card_button + 4.0f });
        header->add(make_icon_button(u8"notification-center-close", codicons::icon_chevron_down, tr(u8"tree.notification.hide_center"), message(toggle_notification_center_intent {})),
            { .length = card_button + 4.0f, .cross_length = card_button + 4.0f });
        column->add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"notification-center-bar" }, bar_style { .divider_bottom = true }, std::move(header)), center_header_height);

        const luil::virtual_list_element* shown_list { nullptr };
        if (entries->empty())
            column->add(make_empty(center), center_empty_height);
        else
        {
            config.scroll_offset = view.notifications_scroll;
            config.build_row = [entries, rows, card_width](const luil::virtual_list_item&, const std::size_t index, bool) -> std::unique_ptr<luil::ui_element> {
                const center_row& row { (*rows)[index] };
                const notification_view& value { (*entries)[row.entry] };
                if (row.day)
                {
                    const luil::stack_config day_config { .padding = luil::edge_insets { center_list_padding + 4.0f, 10.0f, center_list_padding, 2.0f } };
                    auto label { make_stack(u8"notification-day-row-" + number_text(value.id), day_config) };
                    label->add(make_text(u8"notification-day-" + number_text(value.id), value.day, text_style { .size = 11.0f, .color = dim_color(), .bold = true }), 16.0f);
                    return label;
                }
                const luil::stack_config row_config { .padding = luil::edge_insets { center_list_padding, row.top, center_list_padding, row.bottom } };
                auto line { make_stack(u8"notification-row-" + number_text(value.id), row_config) };
                line->add(make_center_card(value, card_width), notification_card_height(value, card_width, true));
                return line;
            };
            config.scroll = [](const float delta) { return message(scroll_intent { .target = scroll_target::notifications, .value = delta }); };
            config.scroll_to = [](const float offset) { return message(scroll_intent { .target = scroll_target::notifications, .value = offset, .absolute = true }); };
            config.edges = luil::scroll_edges { .shadows = true };
            auto items { std::make_unique<luil::virtual_list_element>(std::move(config)) };
            items->set_access_name(tr(u8"tree.notification.center"));
            shown_list = items.get();
            column->add(std::move(items), list_height);
        }

        built_popup popup {};
        popup.id = u8"notification-center";
        popup.notifications = true;
        popup.width = width;
        popup.height = center_header_height + list_height;
        popup.x = view.logical_width - center_margin - width;
        popup.y = std::max(bottom - 4.0f - popup.height, 0.0f);
        // 경계는 판의 둥근 테두리 하나다. 판이 창 바탕색이라 아래 화면과 가르려면 구분선보다 또렷한 선이 있어야 한다.
        popup.border = false;
        luil::panel_config panel_config { floating_panel_config(center_radius, 0.0f) };
        panel_config.background = [](const luil::ui_color_palette& palette) { return palette.window_background; };
        panel_config.border = [](const luil::ui_color_palette& palette) { return palette.group_border; };
        auto panel { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_panel, u8"notification-center-panel" }, panel_config) };
        panel->set_content(std::move(column));
        panel->set_access_name(tr(u8"tree.notification.center"));
        popup.tree = arranged(std::move(panel), width, popup.height, scale);
        if (list != nullptr)
            *list = shown_list;
        return popup;
    }
} // namespace cyella::ui
