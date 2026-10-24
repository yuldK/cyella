#include "domain/text_catalog.h"
#include "presentation/dialog_intents.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ref_chip_element.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui/ui_parts.h"

#include "luil/ui/menu_element.h"
#include "luil/ui/scroll_area_element.h"
#include "luil/ui/ui_tree.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"

#include <algorithm>

namespace cyella::ui {
    namespace {
        // 메뉴 폭의 하한·상한과 항목 하나가 글 밖에 쓰는 폭(아이콘 칸·여백)이다 (논리 픽셀).
        constexpr float menu_minimum_width { 190.0f };
        constexpr float menu_maximum_width { 560.0f };
        constexpr float menu_chrome_width { 60.0f };

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

    built_popup build_ref_list_popup(const ref_list_state& state, const std::vector<ref_label>& labels, const float scale, const float maximum_height)
    {
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

        luil::menu_config config {};
        config.owner = u8"ref-list";
        config.items.push_back({ .key = u8"ref-list-title", .label = tr(u8"ref_list.title", { { u8"count", entries.size() } }), .enabled = false });
        float widest { 220.0f };
        for (const ref_label& label : entries)
        {
            std::u8string text { label.text };
            if (label.current)
                text += tr(u8"ref_list.current_branch_suffix");
            if (label.worktree_path.empty() == false)
                text += tr(u8"ref_list.in_other_worktree_suffix");
            char32_t icon { codicons::icon_git_branch };
            if (label.current)
                icon = codicons::icon_check;
            else if (label.kind == ref_label_kind::remote)
                icon = codicons::icon_cloud;
            else if (label.kind == ref_label_kind::tag)
                icon = codicons::icon_tag;
            const bool enabled { label.kind == ref_label_kind::local || label.kind == ref_label_kind::remote || label.kind == ref_label_kind::tag };
            config.items.push_back({ .key = u8"ref-list-" + ref_label_key(label), .label = text, .icon = icon, .enabled = enabled });
            widest = std::max(widest, estimate_text_width(text, 12.0f));
        }
        config.select = [entries, state](const std::u8string& key) -> luil::input_action {
            for (const ref_label& label : entries)
            {
                if (key != u8"ref-list-" + ref_label_key(label))
                    continue;
                menu_target target {};
                target.kind = label.kind == ref_label_kind::local ? menu_target_kind::local_branch : label.kind == ref_label_kind::remote ? menu_target_kind::remote_branch : menu_target_kind::tag;
                target.name = label.ref_name;
                target.sha = state.sha;
                return message(open_context_menu_intent { target, state.x, state.y });
            }
            return {};
        };

        built_popup popup {};
        popup.id = u8"ref-list";
        popup.ref_list = true;
        popup.x = state.x / scale;
        popup.y = state.y / scale + 2.0f;
        popup.width = std::clamp(widest + menu_chrome_width, 280.0f, menu_maximum_width);
        popup.height = luil::menu_element::height_for(config);
        auto root { std::make_unique<luil::menu_element>(std::move(config)) };
        for (std::size_t index { 0 }; index < entries.size(); ++index)
        {
            luil::ui_element& item { *root->children()[index + 1] };
            const ref_label& label { entries[index] };
            set_text_tooltip(item, label.text);
            if (const luil::ui_action* const select { item.action(luil::ui_trigger::left_click) }; select != nullptr)
                item.set_action(luil::ui_trigger::right_click, *select);
        }
        std::unique_ptr<luil::ui_element> content { std::move(root) };
        if (popup.height > maximum_height)
        {
            luil::scroll_area_config scroll {};
            scroll.owner = u8"ref-list-scroll";
            scroll.content_height = popup.height;
            scroll.scroll_offset = std::clamp(state.scroll, 0.0f, popup.height - maximum_height);
            const float maximum { popup.height - maximum_height };
            scroll.scroll = [offset = scroll.scroll_offset, maximum](const float delta) { return message(scroll_ref_list_intent { std::clamp(offset + delta, 0.0f, maximum), true }); };
            scroll.scroll_to = [](const float value) { return message(scroll_ref_list_intent { value, true }); };
            auto area { std::make_unique<luil::scroll_area_element>(std::move(scroll)) };
            area->set_content(std::move(content));
            content = std::move(area);
            popup.height = maximum_height;
        }
        popup.tree = std::make_shared<const luil::ui_tree>(luil::make_arranged_tree(std::move(content), { 0.0f, 0.0f, popup.width * scale, popup.height * scale }, scale));
        return popup;
    }
} // namespace cyella::ui
