#include "presentation/ui/notification_ui.h"

#include "domain/text_catalog.h"
#include "presentation/dialog_intents.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"

#include "luil/ui/panel_element.h"
#include "luil/ui/stack_element.h"
#include "luil/ui/ui_tree.h"

#include "include/core/SkCanvas.h"
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
        constexpr float time_line_height { 16.0f };
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
        constexpr float center_maximum_height { 520.0f };
        constexpr float center_header_height { 36.0f };
        constexpr float center_empty_height { 56.0f };
        constexpr float center_margin { 8.0f };
        constexpr float panel_radius { 6.0f };

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

        // 머리 줄 오른쪽의 단추([자세히]·[×])를 뺀 글 폭이다.
        float text_width(const notification_view& value, const float width)
        {
            const float buttons { card_button * (value.details.empty() ? 1.0f : 2.0f) };
            return std::max(width - card_padding * 2.0f - icon_column - card_gap - buttons, 40.0f);
        }

        float action_width(const notification_action& action)
        {
            return text_button_width(action.label) + 24.0f;
        }

        // 조치 단추가 한 줄에 들어가지 않으면 한 줄에 하나씩 세운다 (커밋 전 경고는 프로필마다 단추가 선다).
        bool actions_stacked(const notification_view& value, const float width)
        {
            float total { 0.0f };
            for (const notification_action& action : value.actions)
                total += action_width(action);
            total += card_gap * static_cast<float>(value.actions.size() > 0 ? value.actions.size() - 1 : 0);
            return total > width - card_padding * 2.0f;
        }

        float actions_height(const notification_view& value, const float width)
        {
            if (value.actions.empty())
                return 0.0f;
            if (actions_stacked(value, width) == false)
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

        float message_height(const notification_view& value, const float width, const bool center)
        {
            float height { value.title.empty() ? 0.0f : text_line_height };
            height += wrapped_text_height(value.text, text_width(value, width), body_style, text_line_height);
            if (center)
                height += time_line_height;
            return std::max(height, card_button);
        }

        std::unique_ptr<luil::stack_element> make_stack(std::u8string owner, const luil::stack_config& config)
        {
            return std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, std::move(owner) }, config);
        }

        // 알림 한 장이다: 아이콘 · 머리말과 글(센터는 시각까지) · [자세히] · [×], 그 아래 조치 단추와 편 줄.
        std::unique_ptr<luil::ui_element> make_card(const notification_view& value, const float width, const bool center)
        {
            const std::u8string owner { (center ? u8"notification-" : u8"toast-") + number_text(value.id) };
            auto column { make_stack(owner + u8"-column", { .spacing = card_gap, .padding = luil::edge_insets::all(card_padding) }) };

            auto head { make_stack(owner + u8"-head", { .direction = luil::stack_direction::row, .cross_alignment = luil::stack_alignment::start }) };
            head->add(std::make_unique<icon_element>(luil::ui_element_id { kind_label, owner + u8"-icon" }, severity_glyph(value.severity), severity_color(value.severity), 16.0f),
                { .length = icon_column, .cross_length = text_line_height });
            auto body { make_stack(owner + u8"-message", {}) };
            if (value.title.empty() == false)
                body->add(make_text(owner + u8"-title", value.title, text_style { .size = 12.0f, .color = severity_color(value.severity), .bold = true }), text_line_height);
            if (value.text.empty() == false)
                body->add(make_wrapped_text(owner + u8"-text", value.text, body_style, text_line_height), wrapped_text_height(value.text, text_width(value, width), body_style, text_line_height));
            if (center)
            {
                auto time { make_text(owner + u8"-time", value.time, text_style { .size = 11.0f, .color = dim_color() }) };
                time->set_tooltip(value.time_tooltip);
                body->add(std::move(time), time_line_height);
            }
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
            column->add(std::move(head), message_height(value, width, center));

            if (value.actions.empty() == false)
            {
                const bool stacked { actions_stacked(value, width) };
                luil::stack_config actions_config {};
                actions_config.direction = stacked ? luil::stack_direction::column : luil::stack_direction::row;
                actions_config.spacing = stacked ? action_line_gap : card_gap;
                actions_config.cross_alignment = stacked ? luil::stack_alignment::end : luil::stack_alignment::stretch;
                auto actions { make_stack(owner + u8"-actions", actions_config) };
                if (stacked == false)
                    actions->add_flexible_gap();
                for (const notification_action& action : value.actions)
                {
                    const notification_action_intent pressed { .key = action.key, .id = value.operation, .notification = value.id };
                    auto button { make_text_button(owner + u8"-action-" + action.key, action.label, luil::text_button_visual::accent, message(pressed)) };
                    if (stacked)
                        actions->add(std::move(button), { .length = action_height, .cross_length = std::min(action_width(action), width - card_padding * 2.0f) });
                    else
                        actions->add(std::move(button), action_width(action));
                }
                column->add(std::move(actions), actions_height(value, width));
            }

            if (const std::size_t lines { detail_line_count(value) }; lines > 0)
            {
                auto details { make_stack(owner + u8"-details-list", { .padding = luil::edge_insets { icon_column, 0.0f, 0.0f, 0.0f } }) };
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
                column->add(std::move(details), static_cast<float>(lines) * detail_line_height);
            }
            return column;
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
    } // namespace

    float notification_card_height(const notification_view& value, const float width, const bool center)
    {
        float height { card_padding * 2.0f + message_height(value, width, center) };
        if (const float actions { actions_height(value, width) }; actions > 0.0f)
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
            panel->set_content(make_card(value, width, false));
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
        auto entries { std::make_shared<std::vector<notification_view>>(center.entries) };
        float content { 0.0f };
        luil::virtual_list_config config {};
        config.owner = u8"notification-list";
        for (const notification_view& value : *entries)
        {
            const float height { notification_card_height(value, width, true) };
            config.items.push_back({ .key = u8"notification-" + number_text(value.id), .label = value.title.empty() ? value.text : value.title, .height = height });
            content += height;
        }
        const float available { std::max(std::min(center_maximum_height, bottom - view.logical_height * 0.2f) - center_header_height, center_empty_height) };
        const float list_height { entries->empty() ? center_empty_height : std::min(content, available) };

        auto column { make_stack(u8"notification-center-column", {}) };
        const luil::stack_config header_config {
            .direction = luil::stack_direction::row, .spacing = 4.0f, .padding = luil::edge_insets { 12.0f, 0.0f, 6.0f, 0.0f }, .cross_alignment = luil::stack_alignment::center
        };
        auto header { make_stack(u8"notification-center-header", header_config) };
        header->add_flexible(make_text(u8"notification-center-title", tr(u8"tree.notification.center"), text_style { .size = 12.0f, .bold = true }));
        header->add(make_icon_button(u8"notification-center-clear", codicons::icon_clear_all, tr(u8"tree.notification.clear_all"), message(clear_notifications_intent {}), entries->empty() == false),
            { .length = card_button, .cross_length = card_button });
        header->add(make_icon_button(u8"notification-center-close", codicons::icon_chevron_down, tr(u8"tree.notification.hide_center"), message(toggle_notification_center_intent {})),
            { .length = card_button, .cross_length = card_button });
        column->add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"notification-center-bar" }, bar_style { .divider_bottom = true }, std::move(header)), center_header_height);

        const luil::virtual_list_element* shown_list { nullptr };
        if (entries->empty())
            column->add(make_text(u8"notification-center-empty", tr(u8"tree.notification.empty"), text_style { .size = 12.0f, .color = dim_color(), .align = 1 }), center_empty_height);
        else
        {
            config.scroll_offset = view.notifications_scroll;
            config.build_row = [entries, width](const luil::virtual_list_item&, const std::size_t index, bool) -> std::unique_ptr<luil::ui_element> {
                const notification_view& value { (*entries)[index] };
                return std::make_unique<bar_element>(
                    luil::ui_element_id { kind_panel, u8"notification-row-" + number_text(value.id) }, bar_style { .divider_bottom = true }, make_card(value, width, true));
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
        auto panel { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_panel, u8"notification-center-panel" }, floating_panel_config(panel_radius, 0.3f)) };
        panel->set_content(std::move(column));
        panel->set_access_name(tr(u8"tree.notification.center"));
        popup.tree = arranged(std::move(panel), width, popup.height, scale);
        if (list != nullptr)
            *list = shown_list;
        return popup;
    }
} // namespace cyella::ui
