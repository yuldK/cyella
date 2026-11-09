#include "domain/text_catalog.h"
#include "presentation/dialog_intents.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ref_chip_element.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui/ui_parts.h"

#include "luil/ui/draw_primitives.h"
#include "luil/ui/menu_element.h"
#include "luil/ui/scroll_area_element.h"
#include "luil/ui/ui_tree.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"

#include <algorithm>
#include <array>
#include <functional>

namespace cyella::ui {
    namespace {
        // 메뉴 폭의 하한·상한과 항목 하나가 글 밖에 쓰는 폭(아이콘 칸·여백)이다 (논리 픽셀).
        constexpr float menu_minimum_width { 190.0f };
        constexpr float menu_maximum_width { 560.0f };
        constexpr float menu_chrome_width { 60.0f };

        // 참조 목록 카드의 치수다 (논리 픽셀).
        //  - 메뉴는 사각으로 바탕을 칠하므로 둥근 모서리 안쪽으로 `padding`만큼 들여 앉힌다.
        //  - 꼬리는 칩 가운데를 가리킨다. 카드는 꼬리에서 `tail_inset`만큼 왼쪽에서 시작해 오른쪽으로 펼쳐지고, 창 끝에 닿으면
        //    카드가 밀려도 꼬리는 칩에 남는다. 꼬리는 모서리 둥근 자리에 서지 않도록 끝에서 `tail_inset_minimum` 안쪽이다.
        constexpr float ref_card_padding { 4.0f };
        constexpr float ref_card_radius { 8.0f };
        constexpr float ref_card_minimum_width { 240.0f };
        constexpr float ref_card_maximum_width { 380.0f };
        constexpr float ref_card_minimum_height { 80.0f };
        constexpr float ref_card_maximum_height { 420.0f };
        constexpr float ref_card_margin { 8.0f };
        constexpr float ref_card_gap { 2.0f };
        constexpr float ref_card_tail_height { 7.0f };
        constexpr float ref_card_tail_half_width { 7.0f };
        constexpr float ref_card_tail_inset { 28.0f };
        constexpr float ref_card_tail_inset_minimum { 18.0f };

        // 꼬리 달린 참조 목록 카드다. 둥근 몸통과 꼬리를 경로 하나로 칠하고 테두리를 그 경로로 그어, 꼬리와 몸통 사이에 선이 서지 않는다.
        //  - 주 창 tree에 서므로 그림자를 직접 드리운다 (popup 창이면 OS가 맡던 몫이다).
        //  - 누름이 아래 그래프 행으로 새지 않게 막는다.
        class ref_list_card_element final : public luil::ui_element
        {
        public:
            ref_list_card_element(const float tail_x, const bool tail_up, const std::size_t count, std::unique_ptr<luil::ui_element> content)
                : ui_element { ref_list_card_id() }
                , tail_x_ { tail_x }
                , tail_up_ { tail_up }
                , count_ { count }
            {
                set_hit_opaque(true);
                add_child(std::move(content));
            }

            // 카드 안의 메뉴 항목마다 부른다 (흘리는 창 안에 있어도 찾는다).
            void for_each_item(const std::function<void(luil::ui_element&)>& visit)
            {
                std::function<void(luil::ui_element&)> walk {};
                walk = [&walk, &visit](luil::ui_element& element) {
                    if (element.id().kind == luil::ui_element_kind::menu_item)
                        visit(element);
                    for (const std::unique_ptr<luil::ui_element>& child : element.children())
                        walk(*child);
                };
                walk(*this);
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                const float scale { context.scale > 0.0f ? context.scale : 1.0f };
                const luil::rect_f body { body_rect(scale) };
                const float inset { ref_card_padding * scale };
                for (const std::unique_ptr<luil::ui_element>& child : children())
                    child->arrange(context.for_child({ body.x + inset, body.y + inset, std::max(body.width - inset * 2.0f, 0.0f), std::max(body.height - inset * 2.0f, 0.0f) }));
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const float scale { context.scale > 0.0f ? context.scale : 1.0f };
                const luil::rect_f body { body_rect(scale) };
                const float radius { ref_card_radius * scale };
                luil::draw_surface_shadow(context, body, radius, 0.35f);
                const SkPath shape { bubble_path(body, scale) };
                SkPaint fill { luil::solid_paint(context.palette.surface_background) };
                fill.setAntiAlias(true);
                context.canvas.drawPath(shape, fill);
                draw_children(context, interaction);
                // 테두리는 내용 위다 (popup의 경계와 같은 색).
                SkPaint border { luil::solid_paint(context.palette.tooltip_border) };
                border.setAntiAlias(true);
                border.setStyle(SkPaint::kStroke_Style);
                border.setStrokeWidth(scale);
                context.canvas.drawPath(shape, border);
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::group, .name = tr(u8"ref_list.card_access_name", { { u8"count", count_ } }) };
            }

        private:
            // 꼬리를 뺀 몸통이다 (물리 픽셀).
            [[nodiscard]] luil::rect_f body_rect(const float scale) const
            {
                const luil::rect_f box { bounds() };
                const float tail { ref_card_tail_height * scale };
                return { box.x, tail_up_ ? box.y + tail : box.y, box.width, std::max(box.height - tail, 0.0f) };
            }

            // 둥근 몸통과 꼬리를 시계 방향으로 한 바퀴 잇는다. 테두리 선의 절반이 밖으로 나가지 않게 반 px 안쪽으로 그린다.
            [[nodiscard]] SkPath bubble_path(const luil::rect_f& body, const float scale) const
            {
                const float half_line { scale / 2.0f };
                const float left { body.x + half_line };
                const float right { body.x + body.width - half_line };
                const float top { body.y + half_line };
                const float bottom { body.y + body.height - half_line };
                const float radius { ref_card_radius * scale };
                const float tip_x { bounds().x + tail_x_ * scale };
                const float half { ref_card_tail_half_width * scale };
                const float tail { ref_card_tail_height * scale };
                SkPathBuilder builder {};
                builder.moveTo(left + radius, top);
                if (tail_up_)
                {
                    builder.lineTo(tip_x - half, top);
                    builder.lineTo(tip_x, top - tail);
                    builder.lineTo(tip_x + half, top);
                }
                builder.arcTo({ right, top }, { right, bottom }, radius);
                builder.arcTo({ right, bottom }, { left, bottom }, radius);
                if (tail_up_ == false)
                {
                    builder.lineTo(tip_x + half, bottom);
                    builder.lineTo(tip_x, bottom + tail);
                    builder.lineTo(tip_x - half, bottom);
                }
                builder.arcTo({ left, bottom }, { left, top }, radius);
                builder.arcTo({ left, top }, { right, top }, radius);
                builder.close();
                return builder.detach();
            }

            float tail_x_ { 0.0f };
            bool tail_up_ { true };
            std::size_t count_ { 0 };
        };

        // 끄는 동안 대상 위에 뜨는 문구 알약이다. 포인터 오른쪽 아래에 붙고 창 밖으로 나가지 않는다.
        class drag_hint_element final : public luil::ui_element
        {
        public:
            explicit drag_hint_element(std::shared_ptr<const drop_hint_data> hints)
                : ui_element { luil::ui_element_id { kind_drag_hint, u8"drag-hint" } }
                , hints_ { std::move(hints) }
            {
                set_hit_opaque(false);
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                if (hints_ == nullptr || interaction.drag.has_value() == false || interaction.drag->surface.empty() == false)
                    return;
                const luil::drag_visual& drag { *interaction.drag };
                if (drag.hovered_drop_target == luil::ui_element_id {})
                    return;
                const std::optional<std::u8string> text { hints_->describe(drag.payload.dragged_owner, drag.hovered_drop_target.owner) };
                if (text.has_value() == false)
                    return;
                const float scale { context.scale };
                const SkFont font { ui_font(context, 12.0f) };
                const luil::rect_f window { bounds() };
                const float padding { 10.0f * scale };
                const float width { std::min(luil::measure_text(*text, font) + padding * 2.0f, window.width - 8.0f * scale) };
                const float height { 26.0f * scale };
                float x { drag.x + 14.0f * scale };
                float y { drag.y + 40.0f * scale };
                x = std::max(window.x + 4.0f * scale, std::min(x, window.x + window.width - width - 4.0f * scale));
                y = y + height > window.y + window.height ? drag.y - height - 6.0f * scale : y;
                const SkRect box { SkRect::MakeXYWH(x, y, width, height) };
                const float radius { context.metrics.control_corner_radius * scale };
                context.canvas.drawRRect(SkRRect::MakeRectXY(box, radius, radius), luil::solid_paint(context.palette.surface_background));
                SkPaint border { luil::solid_paint(context.palette.accent) };
                border.setStyle(SkPaint::kStroke_Style);
                border.setStrokeWidth(scale);
                context.canvas.drawRRect(SkRRect::MakeRectXY(box, radius, radius), border);
                static_cast<void>(luil::draw_text_within(
                    context.canvas, *text, x + padding, y + luil::centered_text_baseline(font, height), width - padding * 2.0f, font, luil::solid_paint(context.palette.primary_foreground)));
            }

        private:
            std::shared_ptr<const drop_hint_data> hints_ {};
        };
    } // namespace

    std::unique_ptr<luil::ui_element> build_drag_hint_overlay(std::shared_ptr<const drop_hint_data> hints)
    {
        return std::make_unique<drag_hint_element>(std::move(hints));
    }

    built_popup build_menu_popup(const menu_view& menu, const float scale)
    {
        luil::menu_config config {};
        config.owner = u8"context-menu";
        float widest { 0.0f };
        if (menu.title.empty() == false)
        {
            // 제목 줄은 누를 수 없는 첫 항목이다.
            config.items.push_back({ .key = u8"title", .label = menu.title, .icon = 0, .enabled = false, .separator_above = false });
            widest = estimate_text_width(menu.title, 12.0f);
        }
        for (const menu_entry& entry : menu.entries)
        {
            config.items.push_back({ .key = entry.key, .label = entry.label, .icon = entry.icon, .enabled = entry.enabled, .separator_above = entry.separator_above });
            widest = std::max(widest, estimate_text_width(entry.label, 12.0f));
        }
        config.select = [](const std::u8string& key) { return message(menu_select_intent { key }); };

        built_popup popup {};
        popup.id = u8"context-menu";
        popup.x = menu.x;
        popup.y = menu.y;
        popup.width = std::clamp(widest + menu_chrome_width, menu_minimum_width, menu_maximum_width);
        popup.height = luil::menu_element::height_for(config);
        auto root { std::make_unique<luil::menu_element>(std::move(config)) };
        popup.tree = std::make_shared<const luil::ui_tree>(luil::make_arranged_tree(std::move(root), { 0.0f, 0.0f, popup.width * scale, popup.height * scale }, scale));
        return popup;
    }

    placed_ref_list build_ref_list_card(const ref_list_state& state, const std::vector<ref_label>& labels, const float scale, const shell_rect& area)
    {
        placed_ref_list placed {};
        // 로컬·원격 짝을 한 라벨로 합친 것을 다시 둘로 편다. 카드는 원격 브랜치도 따로 고를 수 있어야 한다.
        std::vector<ref_label> entries {};
        for (const ref_label& label : labels)
        {
            entries.push_back(label);
            if (label.also_remote)
            {
                ref_label remote {};
                remote.kind = ref_label_kind::remote;
                remote.text = label.remote_name;
                remote.ref_name = to_text(label.remote_name);
                entries.push_back(std::move(remote));
            }
        }
        if (entries.empty())
            return placed;

        // 종류별로 묶는다: 로컬(detached HEAD 포함) · 원격 · 태그 · stash. 묶음 머리는 고를 수 없는 흐린 항목이다.
        struct group
        {
            std::u8string key {};
            std::vector<const ref_label*> members {};
        };
        std::array<group, 4> groups { group { u8"local" }, group { u8"remote" }, group { u8"tag" }, group { u8"stash" } };
        // 문구 키는 변수로 넘기지 않고 글자 그대로 부른다. 문구 키 검사가 코드에 적힌 키 글자를 찾기 때문이다.
        const auto group_head = [](const std::size_t index, const std::size_t count) {
            switch (index)
            {
            case 0:
                return tr(u8"ref_list.group_local", { { u8"count", count } });
            case 1:
                return tr(u8"ref_list.group_remote", { { u8"count", count } });
            case 2:
                return tr(u8"ref_list.group_tag", { { u8"count", count } });
            default:
                return tr(u8"ref_list.group_stash", { { u8"count", count } });
            }
        };
        for (const ref_label& label : entries)
        {
            std::size_t index { 0 };
            if (label.kind == ref_label_kind::remote)
                index = 1;
            else if (label.kind == ref_label_kind::tag)
                index = 2;
            else if (label.kind == ref_label_kind::stash)
                index = 3;
            groups[index].members.push_back(&label);
        }

        luil::menu_config config {};
        config.owner = std::u8string { ref_list_owner_prefix };
        // 둥근 카드가 테두리를 긋는다. 메뉴의 사각 선은 카드 안에서 한 겹 더 선다.
        config.border = false;
        float widest { 0.0f };
        bool first { true };
        for (std::size_t index { 0 }; index < groups.size(); ++index)
        {
            const group& part { groups[index] };
            if (part.members.empty())
                continue;
            const std::u8string head { group_head(index, part.members.size()) };
            config.items.push_back({ .key = std::u8string { ref_list_owner_prefix } + u8"-group-" + part.key, .label = head, .enabled = false, .separator_above = first == false });
            widest = std::max(widest, estimate_text_width(head, 12.0f));
            first = false;
            for (const ref_label* const label : part.members)
            {
                // 현재 브랜치·다른 worktree는 이름 뒤가 아니라 오른쪽 흐린 열에 둔다. 이름이 잘려도 상태는 남는다.
                std::u8string status {};
                if (label->current)
                    status = tr(u8"ref_list.current");
                else if (label->worktree_path.empty() == false)
                    status = tr(u8"ref_list.in_other_worktree");
                char32_t icon { codicons::icon_git_branch };
                if (label->current)
                    icon = codicons::icon_check;
                else if (label->kind == ref_label_kind::remote)
                    icon = codicons::icon_cloud;
                else if (label->kind == ref_label_kind::tag)
                    icon = codicons::icon_tag;
                const bool enabled { label->kind == ref_label_kind::local || label->kind == ref_label_kind::remote || label->kind == ref_label_kind::tag };
                config.items.push_back({ .key = std::u8string { ref_list_owner_prefix } + u8"-" + ref_label_key(*label), .label = label->text, .icon = icon, .enabled = enabled, .shortcut_text = status });
                widest = std::max(widest, estimate_text_width(label->text, 12.0f) + (status.empty() ? 0.0f : estimate_text_width(status, 12.0f) + 24.0f));
            }
        }
        // 항목을 고르면 그 참조의 우클릭 메뉴를 "+N" 칩 아래에 연다. 메뉴가 열리면 카드는 닫힌다 (`open_context_menu`).
        config.select = [entries, state](const std::u8string& key) -> luil::input_action {
            for (const ref_label& label : entries)
            {
                if (key != std::u8string { ref_list_owner_prefix } + u8"-" + ref_label_key(label))
                    continue;
                menu_target target {};
                target.kind = label.kind == ref_label_kind::local ? menu_target_kind::local_branch : label.kind == ref_label_kind::remote ? menu_target_kind::remote_branch : menu_target_kind::tag;
                target.name = label.ref_name;
                target.sha = state.sha;
                return message(open_context_menu_intent { target, state.x, state.y + state.height });
            }
            return {};
        };

        // 자리를 정한다 (논리 픽셀). 카드는 칩에서 오른쪽으로 펼쳐지고 꼬리는 칩 가운데를 가리킨다.
        const float chip_x { state.x / scale };
        const float chip_top { state.y / scale };
        const float chip_bottom { (state.y + state.height) / scale };
        const float chip_center { chip_x + state.width / scale / 2.0f };
        const float menu_height { luil::menu_element::height_for(config) };
        const float width { std::clamp(widest + menu_chrome_width + ref_card_padding * 2.0f, ref_card_minimum_width, ref_card_maximum_width) };
        const float wanted { menu_height + ref_card_padding * 2.0f };
        const float area_bottom { area.y + area.height };
        const float below { area_bottom - ref_card_margin - (chip_bottom + ref_card_gap + ref_card_tail_height) };
        const float above { chip_top - ref_card_gap - ref_card_tail_height - (area.y + ref_card_margin) };
        // 아래가 기본이다. 아래에 다 들어가지 않고 위가 더 넓을 때만 위로 올린다.
        const bool down { wanted <= below || below >= above };
        const float body_height { std::max(std::min(wanted, std::min(down ? below : above, ref_card_maximum_height)), ref_card_minimum_height) };
        const float area_right { area.x + area.width };
        const float x { std::max(std::min(chip_center - ref_card_tail_inset, area_right - ref_card_margin - width), area.x + ref_card_margin) };
        const float tail_x { std::clamp(chip_center - x, ref_card_tail_inset_minimum, width - ref_card_tail_inset_minimum) };
        const float y { down ? chip_bottom + ref_card_gap : chip_top - ref_card_gap - ref_card_tail_height - body_height };

        std::unique_ptr<luil::ui_element> content { std::make_unique<luil::menu_element>(std::move(config)) };
        const float content_height { body_height - ref_card_padding * 2.0f };
        if (menu_height > content_height)
        {
            luil::scroll_area_config scroll {};
            scroll.owner = std::u8string { ref_list_owner_prefix } + u8"-scroll";
            scroll.content_height = menu_height;
            const float maximum { menu_height - content_height };
            scroll.scroll_offset = std::clamp(state.scroll, 0.0f, maximum);
            scroll.scroll = [offset = scroll.scroll_offset, maximum](const float delta) { return message(scroll_ref_list_intent { std::clamp(offset + delta, 0.0f, maximum), true }); };
            scroll.scroll_to = [](const float value) { return message(scroll_ref_list_intent { value, true }); };
            auto scroll_area { std::make_unique<luil::scroll_area_element>(std::move(scroll)) };
            scroll_area->set_content(std::move(content));
            content = std::move(scroll_area);
        }
        auto card { std::make_unique<ref_list_card_element>(tail_x, down, entries.size(), std::move(content)) };
        // 항목마다 이름 전체를 툴팁으로 보이고, 우클릭도 누름과 같이 메뉴를 연다 (칩의 우클릭과 같은 손버릇).
        card->for_each_item([](luil::ui_element& item) {
            if (item.enabled() == false)
                return;
            set_text_tooltip(item, item.accessibility().name);
            if (const luil::ui_action* const select { item.action(luil::ui_trigger::left_click) }; select != nullptr)
                item.set_action(luil::ui_trigger::right_click, *select);
        });
        placed.element = std::move(card);
        placed.rect = { x, y, width, body_height + ref_card_tail_height };
        placed.tail_x = x + tail_x;
        placed.tail_up = down;
        return placed;
    }
} // namespace cyella::ui
