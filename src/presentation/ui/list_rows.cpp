#include "presentation/ui/list_rows.h"

#include "presentation/dialog_intents.h"
#include "presentation/drop_hints.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/sidebar_extension_rows.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"

#include <algorithm>

namespace cyella::ui {
    namespace {
        constexpr float row_padding { 8.0f };
        constexpr float icon_size { 14.0f };
        constexpr float indent_step { 14.0f };
        constexpr float row_button_size { 22.0f };
        constexpr float start_corner_radius { 6.0f };
        constexpr float start_icon_size { 32.0f };
        constexpr float start_row_inset { 12.0f };
        constexpr float start_title_height { 20.0f };
        constexpr float start_path_height { 18.0f };
        constexpr float start_card_text_height { start_title_height + start_path_height };

        void fill_rounded(SkCanvas& canvas, const luil::rect_f& box, const float radius, const luil::ui_color color)
        {
            SkPaint paint { luil::solid_paint(color) };
            paint.setAntiAlias(true);
            canvas.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), radius, radius), paint);
        }

        // 개수를 담은 작은 알약이다 (사이드바 머리·작업 영역 머리).
        float draw_count_pill(luil::draw_context& context, const std::u8string& text, const float right, const luil::rect_f& box)
        {
            const float scale { context.scale };
            const SkFont font { ui_font(context, 10.5f) };
            const float width { luil::measure_text(text, font) + 10.0f * scale };
            const float height { 16.0f * scale };
            const luil::rect_f pill { right - width, box.y + (box.height - height) / 2.0f, width, height };
            fill_rounded(context.canvas, pill, height / 2.0f, context.palette.input_background);
            static_cast<void>(luil::draw_text_within(
                context.canvas, text, pill.x + 5.0f * scale, pill.y + luil::centered_text_baseline(font, height), width, font, luil::solid_paint(context.palette.secondary_foreground)));
            return width;
        }

        char32_t sidebar_glyph(const sidebar_row_view& row) noexcept
        {
            switch (row.kind)
            {
            case ref_kind::local:
                return row.current ? codicons::icon_check : codicons::icon_git_branch;
            case ref_kind::remote:
                if (row.group)
                    return row.detail.starts_with(u8"http") ? codicons::icon_globe : codicons::icon_key;
                return codicons::icon_git_branch;
            case ref_kind::tag:
                return codicons::icon_tag;
            case ref_kind::stash:
                return codicons::icon_archive;
            case ref_kind::head:
                return codicons::icon_target;
            }
            return codicons::icon_git_branch;
        }

        char32_t section_glyph(const sidebar_section section) noexcept
        {
            switch (section)
            {
            case sidebar_section::local:
                return codicons::icon_git_branch;
            case sidebar_section::remote:
                return codicons::icon_cloud;
            case sidebar_section::tags:
                return codicons::icon_tag;
            case sidebar_section::stashes:
                return codicons::icon_archive;
            case sidebar_section::submodules:
            case sidebar_section::worktrees:
            case sidebar_section::lfs:
            case sidebar_section::pull_requests:
                return extension_section_glyph(section);
            }
            return codicons::icon_git_branch;
        }

        std::u8string status_letter(const file_row_view& row)
        {
            if (row.conflicted)
                return u8"!";
            if (row.untracked)
                return u8"U";
            return std::u8string(1, static_cast<char8_t>(row.status));
        }
    } // namespace

    // ---------------------------------------------------------------- sidebar_row_element

    sidebar_row_element::sidebar_row_element(sidebar_row_view row, std::optional<luil::input_action> select, std::optional<luil::input_action> activate, std::optional<menu_target> menu)
        : ui_element { luil::ui_element_id { kind_sidebar_row, std::u8string { sidebar_row_owner_prefix } + row.key } }
        , row_ { std::move(row) }
    {
        if (row_.tooltip.empty() == false)
            set_tooltip(row_.tooltip);
        if (select.has_value() == false && activate.has_value() == false && menu.has_value() == false)
        {
            set_hit_opaque(false);
            return;
        }
        set_tab_stop(false);
        if (select.has_value())
            set_action(luil::ui_trigger::left_click, action_of(std::move(*select)));
        if (activate.has_value())
        {
            // 보조 기술의 "실행"은 두 번 누름과 같은 체크아웃이다. 고르기는 목록 항목의 SelectionItem이 맡는다.
            set_access_handler([activate = *activate](const luil::access_request& request) -> std::optional<std::vector<luil::input_action>> {
                if (request.command != luil::access_command::invoke)
                    return std::nullopt;
                return std::vector<luil::input_action> { activate };
            });
            set_action(luil::ui_trigger::double_click, action_of(std::move(*activate)));
        }
        if (menu.has_value())
            set_action(luil::ui_trigger::right_click,
                [target = *menu](const luil::ui_action_context& context) -> std::vector<luil::input_action> { return { message(open_context_menu_intent { target, context.x, context.y }) }; });

        // 로컬·원격 브랜치 행은 끌 수 있고 다른 브랜치가 놓일 수 있다. submodule·worktree·LFS 행은 브랜치가 아니다.
        if (row_.header || row_.group || row_.item != sidebar_item_kind::ref || (row_.kind != ref_kind::local && row_.kind != ref_kind::remote))
            return;
        const drop_party party { .kind = row_.kind, .name = row_.ref_name, .sha = row_.target_sha, .is_commit = false };
        const std::u8string key { branch_drag_key(row_.kind, row_.ref_name) };
        luil::drag_source source {};
        source.make_payload = [key, text = row_.label, id = id()](const luil::ui_action_context&) {
            luil::drag_payload payload {};
            payload.source = id;
            payload.dragged_owner = key;
            payload.label = text;
            payload.rejected_cursor = luil::ui_cursor::not_allowed;
            return payload;
        };
        set_drag_source(std::move(source));
        luil::drop_target target {};
        target.accepts = [party](const luil::drag_payload& payload) {
            const std::optional<drop_party> source { parse_branch_drag_key(payload.dragged_owner) };
            return source.has_value() && can_drop_on(*source, party);
        };
        target.on_drop = [party](const luil::drag_payload& payload, const luil::ui_action_context& context) -> std::vector<luil::input_action> {
            const std::optional<drop_party> source { parse_branch_drag_key(payload.dragged_owner) };
            if (source.has_value() == false)
                return {};
            return { message(drop_intent { *source, party, context.x, context.y }) };
        };
        set_drop_target(std::move(target));
    }

    void sidebar_row_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    luil::access_info sidebar_row_element::accessibility() const
    {
        if (row_.header)
            return { .role = luil::access_role::static_text, .name = row_.label + u8" " + number_text(row_.count) };
        const luil::access_role role { action(luil::ui_trigger::double_click) != nullptr ? luil::access_role::button : luil::access_role::static_text };
        std::u8string name { row_.label + (row_.local_only ? std::u8string { u8" (원격에 없음)" } : std::u8string {}) };
        if (row_.worktree_path.empty() == false)
            name += u8" (다른 worktree에서 사용 중)";
        // PR 행은 브랜치 흐름(`feature → main`)이 화면 읽기에도 닿도록 이름에 넣는다.
        if (row_.item == sidebar_item_kind::pull_request && row_.detail.empty() == false && row_.path.empty() == false)
            name += u8" (" + row_.detail + u8")";
        if (row_.badge.empty() == false)
            name += u8" " + row_.badge;
        return { .role = role, .name = std::move(name) };
    }

    void sidebar_row_element::draw(luil::draw_context& context, const luil::interaction_snapshot&) const
    {
        const luil::rect_f box { bounds() };
        const float scale { context.scale };
        const luil::ui_color_palette& palette { context.palette };
        float x { box.x + row_padding * scale };
        if (row_.header)
        {
            const SkPaint dim { luil::solid_paint(palette.secondary_foreground) };
            luil::draw_centered_glyph(
                context.canvas, row_.collapsed ? codicons::icon_chevron_right : codicons::icon_chevron_down, { x, box.y, icon_size * scale, box.height }, icon_font(context, 12.0f), dim);
            x += (icon_size + 2.0f) * scale;
            luil::draw_centered_glyph(context.canvas, section_glyph(row_.section), { x, box.y, icon_size * scale, box.height }, icon_font(context, 13.0f), dim);
            x += (icon_size + 6.0f) * scale;
            const SkFont font { ui_font(context, 11.0f, true) };
            const float pill { draw_count_pill(context, number_text(row_.count), box.x + box.width - row_padding * scale, box) };
            static_cast<void>(luil::draw_text_within(context.canvas, row_.label, x, box.y + luil::centered_text_baseline(font, box.height), box.x + box.width - pill - x - 12.0f * scale, font, dim));
            return;
        }

        if (row_.item != sidebar_item_kind::ref)
        {
            draw_extension_sidebar_row(context, row_, box);
            return;
        }

        x += (20.0f + static_cast<float>(row_.depth) * indent_step) * scale;
        const luil::ui_color icon_color { row_.current ? palette.accent : palette.secondary_foreground };
        luil::draw_centered_glyph(context.canvas, sidebar_glyph(row_), { x, box.y, icon_size * scale, box.height }, icon_font(context, 13.0f), luil::solid_paint(icon_color));
        x += (icon_size + 6.0f) * scale;

        // 오른쪽 꼬리: ↑앞섬 ↓뒤처짐, upstream 없음·원격에 없는 태그의 구름 아이콘이다.
        float right { box.x + box.width - row_padding * scale };
        // 다른 worktree가 체크아웃한 브랜치다. git이 이 브랜치로 옮기는 것을 거절한다.
        if (row_.worktree_path.empty() == false)
            right -= draw_worktree_marker(context, box, right);
        const SkFont small { ui_font(context, 11.0f) };
        std::u8string tail {};
        if (row_.ahead > 0)
            tail += u8"↑" + number_text(row_.ahead);
        if (row_.behind > 0)
            tail += (tail.empty() ? u8"" : u8" ") + std::u8string { u8"↓" } + number_text(row_.behind);
        if (tail.empty() == false)
        {
            const float width { luil::measure_text(tail, small) };
            static_cast<void>(luil::draw_text_within(context.canvas, tail, right - width, box.y + luil::centered_text_baseline(small, box.height), width, small, luil::solid_paint(palette.accent)));
            right -= width + 6.0f * scale;
        }
        else if ((row_.kind == ref_kind::local && (row_.no_upstream || row_.upstream_gone)) || row_.local_only)
        {
            luil::draw_centered_glyph(
                context.canvas, codicons::icon_cloud, { right - icon_size * scale, box.y, icon_size * scale, box.height }, icon_font(context, 12.0f), luil::solid_paint(palette.disabled_foreground));
            right -= (icon_size + 6.0f) * scale;
        }

        const SkFont font { ui_font(context, 12.0f, row_.current) };
        const float baseline { box.y + luil::centered_text_baseline(font, box.height) };
        const float drawn { luil::draw_text_within(context.canvas, row_.label, x, baseline, std::max(right - x, 0.0f), font, luil::solid_paint(palette.primary_foreground)) };
        if (row_.group && row_.detail.empty() == false)
        {
            const float detail_x { x + drawn + 8.0f * scale };
            if (right - detail_x > 0.0f)
                static_cast<void>(luil::draw_text_within(
                    context.canvas, row_.detail, detail_x, box.y + luil::centered_text_baseline(small, box.height), right - detail_x, small, luil::solid_paint(palette.secondary_foreground)));
        }
        else if (row_.kind == ref_kind::stash && row_.detail.empty() == false)
        {
            const float detail_x { x + drawn + 8.0f * scale };
            if (right - detail_x > 0.0f)
                static_cast<void>(luil::draw_text_within(
                    context.canvas, row_.detail, detail_x, box.y + luil::centered_text_baseline(small, box.height), right - detail_x, small, luil::solid_paint(palette.secondary_foreground)));
        }
    }

    // ---------------------------------------------------------------- file_row_element

    file_row_element::file_row_element(
        file_row_view row, std::vector<file_row_button> buttons, std::optional<luil::input_action> select, std::optional<menu_target> menu, const std::optional<std::size_t> tree_depth)
        : ui_element { luil::ui_element_id { kind_file_row, u8"file-" + row.key } }
        , row_ { std::move(row) }
        , button_count_ { buttons.size() }
        , tree_depth_ { tree_depth }
    {
        if (select.has_value() == false && menu.has_value() == false)
            set_hit_opaque(false);
        else
        {
            set_tab_stop(false);
            if (select.has_value())
            {
                // 보조 기술의 "실행"은 줄을 눌러 diff를 여는 것과 같다 (UI Automation으로 파일을 고를 수 있다).
                set_access_handler([select = *select](const luil::access_request& request) -> std::optional<std::vector<luil::input_action>> {
                    if (request.command != luil::access_command::invoke)
                        return std::nullopt;
                    return std::vector<luil::input_action> { select };
                });
                set_action(luil::ui_trigger::left_click, action_of(std::move(*select)));
            }
            if (menu.has_value())
                set_action(luil::ui_trigger::right_click,
                    [target = *menu](const luil::ui_action_context& context) -> std::vector<luil::input_action> { return { message(open_context_menu_intent { target, context.x, context.y }) }; });
        }
        set_tooltip(to_u8(row_.path));
        for (file_row_button& button : buttons)
        {
            luil::input_action action { std::move(button.action) };
            const bool twice { button.double_click };
            auto element { make_icon_button(std::move(button.owner), button.glyph, std::move(button.tooltip), twice ? luil::input_action {} : action, button.enabled, false, 13.0f) };
            if (twice)
                element->set_action(luil::ui_trigger::double_click, action_of(std::move(action)));
            element->set_tab_stop(false);
            add_child(std::move(element));
        }
    }

    void file_row_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
        const float size { row_button_size * context.scale };
        float right { context.slot.x + context.slot.width - 4.0f * context.scale };
        for (auto it { children().rbegin() }; it != children().rend(); ++it)
        {
            right -= size;
            (*it)->arrange(context.for_child({ right, context.slot.y + (context.slot.height - size) / 2.0f, size, size }));
        }
    }

    luil::access_info file_row_element::accessibility() const
    {
        std::u8string name { row_.name };
        if (row_.submodule)
            name += u8" (서브모듈)";
        if (row_.lfs)
            name += u8" (LFS)";
        return { .role = action(luil::ui_trigger::left_click) != nullptr ? luil::access_role::button : luil::access_role::static_text, .name = std::move(name) };
    }

    void file_row_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        const luil::rect_f box { bounds() };
        const float scale { context.scale };
        const luil::ui_color_palette& palette { context.palette };
        // 트리의 파일은 폴더의 펼침 표지 자리만큼 더 들어가 폴더 이름과 줄을 맞춘다.
        float x { box.x + (row_padding + (tree_depth_.has_value() ? static_cast<float>(*tree_depth_) * indent_step + icon_size + 4.0f : 0.0f)) * scale };

        const float letter { 16.0f * scale };
        const luil::rect_f badge { x, box.y + (box.height - letter) / 2.0f, letter, letter };
        const luil::ui_color color { status_color(palette, row_.conflicted ? 'U' : row_.status) };
        fill_rounded(context.canvas, badge, 3.0f * scale, luil::with_alpha(color, 0.18f));
        const SkFont letter_font { code_font(context, 10.5f) };
        const std::u8string text { status_letter(row_) };
        const float letter_width { luil::measure_text(text, letter_font) };
        static_cast<void>(luil::draw_text_within(
            context.canvas, text, badge.x + (letter - letter_width) / 2.0f, badge.y + luil::centered_text_baseline(letter_font, letter), letter, letter_font, luil::solid_paint(color)));
        x += letter + 8.0f * scale;

        float right { box.x + box.width - (4.0f + static_cast<float>(button_count_) * row_button_size + 4.0f) * scale };
        const SkFont small { ui_font(context, 11.0f) };
        const float baseline_small { box.y + luil::centered_text_baseline(small, box.height) };
        if (row_.removed.empty() == false)
        {
            const float width { luil::measure_text(row_.removed, small) };
            static_cast<void>(luil::draw_text_within(context.canvas, row_.removed, right - width, baseline_small, width, small, luil::solid_paint(palette.error_accent)));
            right -= width + 4.0f * scale;
        }
        if (row_.added.empty() == false)
        {
            const float width { luil::measure_text(row_.added, small) };
            static_cast<void>(luil::draw_text_within(context.canvas, row_.added, right - width, baseline_small, width, small, luil::solid_paint(success_color(palette))));
            right -= width + 4.0f * scale;
        }

        const SkFont font { ui_font(context, 12.0f) };
        const float drawn = luil::draw_text_within(
            context.canvas, row_.name, x, box.y + luil::centered_text_baseline(font, box.height), std::max(right - x, 0.0f), font, luil::solid_paint(palette.primary_foreground));
        float folder_x { x + drawn + 6.0f * scale };
        // Git LFS 추적 파일과 submodule 포인터 표지다.
        const auto draw_tag = [&](const std::u8string& tag, const luil::ui_color color) {
            const SkFont tag_font { ui_font(context, 9.5f, true) };
            const float width { luil::measure_text(tag, tag_font) + 8.0f * scale };
            const float height { 14.0f * scale };
            if (folder_x + width > right)
                return;
            const luil::rect_f pill { folder_x, box.y + (box.height - height) / 2.0f, width, height };
            fill_rounded(context.canvas, pill, 3.0f * scale, luil::with_alpha(color, 0.18f));
            static_cast<void>(luil::draw_text_within(context.canvas, tag, pill.x + 4.0f * scale, pill.y + luil::centered_text_baseline(tag_font, height), width, tag_font, luil::solid_paint(color)));
            folder_x += width + 6.0f * scale;
        };
        if (row_.submodule)
            draw_tag(u8"서브모듈", palette.accent);
        if (row_.lfs)
            draw_tag(u8"LFS", status_color(palette, 'M'));
        if (row_.folder.empty() == false && right - folder_x > 0.0f)
            static_cast<void>(luil::draw_text_within(context.canvas, row_.folder, folder_x, baseline_small, right - folder_x, small, luil::solid_paint(palette.secondary_foreground)));
        draw_children(context, interaction);
    }

    // ---------------------------------------------------------------- folder_row_element

    folder_row_element::folder_row_element(file_tree_row row, luil::input_action toggle)
        : ui_element { luil::ui_element_id { kind_file_row, u8"folder-" + row.key } }
        , row_ { std::move(row) }
    {
        set_tab_stop(false);
        set_cursor(luil::ui_cursor::hand);
        set_tooltip(to_u8(row_.path));
        // 보조 기술의 펼치기·접기·실행은 모두 같은 뒤집기다. 이미 그 상태면 아무것도 내지 않는다.
        set_access_handler([toggle, expanded = row_.expanded](const luil::access_request& request) -> std::optional<std::vector<luil::input_action>> {
            if (request.command == luil::access_command::invoke || (request.command == luil::access_command::expand && expanded == false)
                || (request.command == luil::access_command::collapse && expanded))
                return std::vector<luil::input_action> { toggle };
            return std::nullopt;
        });
        set_action(luil::ui_trigger::left_click, action_of(std::move(toggle)));
    }

    void folder_row_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    luil::access_info folder_row_element::accessibility() const
    {
        return { .role = luil::access_role::button, .name = row_.label + u8" 폴더 (파일 " + number_text(row_.file_count) + u8"개)", .expanded = row_.expanded };
    }

    void folder_row_element::draw(luil::draw_context& context, const luil::interaction_snapshot&) const
    {
        const luil::rect_f box { bounds() };
        const float scale { context.scale };
        const luil::ui_color_palette& palette { context.palette };
        const SkPaint dim { luil::solid_paint(palette.secondary_foreground) };
        float x { box.x + (row_padding + static_cast<float>(row_.depth) * indent_step) * scale };
        luil::draw_centered_glyph(
            context.canvas, row_.expanded ? codicons::icon_chevron_down : codicons::icon_chevron_right, { x, box.y, icon_size * scale, box.height }, icon_font(context, 12.0f), dim);
        x += (icon_size + 4.0f) * scale;
        luil::draw_centered_glyph(context.canvas, row_.expanded ? codicons::icon_folder_opened : codicons::icon_folder, { x, box.y, 16.0f * scale, box.height }, icon_font(context, icon_size), dim);
        x += (16.0f + 8.0f) * scale;

        const SkFont small { ui_font(context, 11.0f) };
        const std::u8string count { number_text(row_.file_count) };
        const float count_width { luil::measure_text(count, small) };
        const float right { box.x + box.width - (row_padding + 4.0f) * scale };
        static_cast<void>(luil::draw_text_within(context.canvas, count, right - count_width, box.y + luil::centered_text_baseline(small, box.height), count_width, small, dim));
        const SkFont font { ui_font(context, 12.0f) };
        static_cast<void>(luil::draw_text_within(context.canvas, row_.label, x, box.y + luil::centered_text_baseline(font, box.height), std::max(right - count_width - 8.0f * scale - x, 0.0f), font,
            luil::solid_paint(palette.primary_foreground)));
    }

    // ---------------------------------------------------------------- file_group_element

    file_group_element::file_group_element(std::u8string owner, std::u8string title, const std::size_t count, std::optional<file_row_button> button)
        : ui_element { luil::ui_element_id { kind_file_row, std::move(owner) } }
        , title_ { std::move(title) }
        , count_ { count }
    {
        set_hit_opaque(false);
        if (button.has_value())
        {
            auto element { make_text_button(std::move(button->owner), std::move(button->tooltip), luil::text_button_visual::accent, std::move(button->action), button->enabled, button->glyph) };
            element->set_tab_stop(false);
            button_ = element.get();
            add_child(std::move(element));
        }
    }

    void file_group_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
        if (button_ != nullptr)
        {
            const float width { 96.0f * context.scale };
            const float height { 20.0f * context.scale };
            button_->arrange(context.for_child({ context.slot.x + context.slot.width - width - 6.0f * context.scale, context.slot.y + (context.slot.height - height) / 2.0f, width, height }));
        }
    }

    luil::access_info file_group_element::accessibility() const
    {
        return { .role = luil::access_role::static_text, .name = title_ + u8" " + number_text(count_) };
    }

    void file_group_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        const luil::rect_f box { bounds() };
        const float scale { context.scale };
        context.canvas.drawRect(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), luil::solid_paint(luil::with_alpha(context.palette.primary_foreground, 0.035f)));
        float x { box.x + row_padding * scale };
        const SkFont font { ui_font(context, 12.0f, true) };
        const float drawn
            = luil::draw_text_within(context.canvas, title_, x, box.y + luil::centered_text_baseline(font, box.height), box.width * 0.6f, font, luil::solid_paint(context.palette.primary_foreground));
        x += drawn + 8.0f * scale;
        const SkFont small { ui_font(context, 10.5f) };
        const std::u8string count { number_text(count_) };
        const float width { luil::measure_text(count, small) + 10.0f * scale };
        const float height { 16.0f * scale };
        const luil::rect_f pill { x, box.y + (box.height - height) / 2.0f, width, height };
        fill_rounded(context.canvas, pill, height / 2.0f, context.palette.input_background);
        static_cast<void>(luil::draw_text_within(
            context.canvas, count, pill.x + 5.0f * scale, pill.y + luil::centered_text_baseline(small, height), width, small, luil::solid_paint(context.palette.secondary_foreground)));
        draw_children(context, interaction);
    }

    // ---------------------------------------------------------------- diff_row_element

    diff_row_element::diff_row_element(std::u8string owner, diff_line_view line)
        : ui_element { luil::ui_element_id { kind_diff_row, std::move(owner) } }
        , line_ { std::move(line) }
    {
        set_hit_opaque(false);
    }

    void diff_row_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    luil::access_info diff_row_element::accessibility() const
    {
        return { .role = luil::access_role::static_text, .name = line_.text };
    }

    void diff_row_element::draw(luil::draw_context& context, const luil::interaction_snapshot&) const
    {
        const luil::rect_f box { bounds() };
        const float scale { context.scale };
        const luil::ui_color_palette& palette { context.palette };
        const SkRect shape { SkRect::MakeXYWH(box.x, box.y, box.width, box.height) };
        if (line_.hunk_header)
            context.canvas.drawRect(shape, luil::solid_paint(luil::with_alpha(palette.accent, 0.08f)));
        else if (line_.kind == diff_line_kind::add)
            context.canvas.drawRect(shape, luil::solid_paint(diff_add_background(palette)));
        else if (line_.kind == diff_line_kind::del)
            context.canvas.drawRect(shape, luil::solid_paint(diff_delete_background(palette)));

        const SkFont font { code_font(context, 11.5f) };
        const float baseline { box.y + luil::centered_text_baseline(font, box.height) };
        const SkPaint dim { luil::solid_paint(palette.secondary_foreground) };
        const float number_width { 40.0f * scale };
        float x { box.x + 6.0f * scale };
        if (line_.hunk_header == false)
        {
            const auto draw_number = [&](const std::int32_t value) {
                if (value > 0)
                {
                    const std::u8string text { number_text(static_cast<std::uint64_t>(value)) };
                    const float width { luil::measure_text(text, font) };
                    static_cast<void>(luil::draw_text_within(context.canvas, text, x + number_width - width - 6.0f * scale, baseline, number_width, font, dim));
                }
                x += number_width;
            };
            draw_number(line_.old_line);
            draw_number(line_.new_line);
        }
        std::u8string text {};
        switch (line_.kind)
        {
        case diff_line_kind::add:
            text = u8"+";
            break;
        case diff_line_kind::del:
            text = u8"-";
            break;
        case diff_line_kind::context:
            text = u8" ";
            break;
        case diff_line_kind::no_newline:
            text = u8"\\";
            break;
        }
        if (line_.hunk_header)
            text.clear();
        text += line_.text;
        // 탭은 Skia가 그리지 못하므로 공백 넷으로 편다.
        std::u8string expanded {};
        for (const char8_t value : text)
            if (value == u8'\t')
                expanded += u8"    ";
            else if (value != u8'\r')
                expanded.push_back(value);
        static_cast<void>(luil::draw_text_within(
            context.canvas, expanded, x + 4.0f * scale, baseline, box.x + box.width - x - 8.0f * scale, font, line_.hunk_header ? dim : luil::solid_paint(palette.primary_foreground)));
    }

    // ---------------------------------------------------------------- log_row_element

    log_row_element::log_row_element(log_line_view line)
        : ui_element { luil::ui_element_id { kind_log_row, line.key } }
        , line_ { std::move(line) }
    {
        set_hit_opaque(false);
        set_tooltip(line_.text);
    }

    void log_row_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    luil::access_info log_row_element::accessibility() const
    {
        return { .role = luil::access_role::static_text, .name = line_.text };
    }

    void log_row_element::draw(luil::draw_context& context, const luil::interaction_snapshot&) const
    {
        const luil::rect_f box { bounds() };
        const float scale { context.scale };
        const luil::ui_color_palette& palette { context.palette };
        luil::ui_color color { line_.heading ? palette.primary_foreground : palette.secondary_foreground };
        if (line_.failed)
            color = palette.error_accent;
        if (line_.automatic)
            color = luil::with_alpha(color, 0.6f);
        const SkFont font { line_.heading ? ui_font(context, 12.0f, true) : code_font(context, 11.0f) };
        const float x { box.x + (line_.heading ? 10.0f : 26.0f) * scale };
        static_cast<void>(
            luil::draw_text_within(context.canvas, line_.text, x, box.y + luil::centered_text_baseline(font, box.height), box.x + box.width - x - 8.0f * scale, font, luil::solid_paint(color)));
    }

    // ---------------------------------------------------------------- start_row_element

    start_row_element::start_row_element(std::u8string owner, start_page_item item, const bool open, std::u8string when, luil::input_action activate, std::vector<file_row_button> buttons)
        : ui_element { luil::ui_element_id { kind_start_row, std::move(owner) } }
        , item_ { std::move(item) }
        , open_ { open }
        , when_ { std::move(when) }
    {
        set_tooltip(item_.path);
        set_cursor(luil::ui_cursor::hand);
        set_action(luil::ui_trigger::left_click, action_of(std::move(activate)));
        for (file_row_button& button : buttons)
        {
            auto element { make_icon_button(std::move(button.owner), button.glyph, std::move(button.tooltip), std::move(button.action), button.enabled, false, 14.0f) };
            element->set_tab_stop(false);
            add_child(std::move(element));
        }
    }

    void start_row_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
        const float size { 24.0f * context.scale };
        float right { context.slot.x + context.slot.width - 8.0f * context.scale };
        for (auto it { children().rbegin() }; it != children().rend(); ++it)
        {
            right -= size;
            (*it)->arrange(context.for_child({ right, context.slot.y + (context.slot.height - size) / 2.0f, size, size }));
            right -= 4.0f * context.scale;
        }
    }

    luil::access_info start_row_element::accessibility() const
    {
        return { .role = luil::access_role::button, .name = item_.display_name + (item_.exists ? std::u8string {} : std::u8string { u8" (경로를 찾을 수 없음)" }) };
    }

    void start_row_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        const luil::rect_f box { bounds() };
        const float scale { context.scale };
        const luil::ui_color_palette& palette { context.palette };
        fill_rounded(context.canvas, box, start_corner_radius * scale, palette.surface_background);
        luil::draw_hover_fill(context, box, id(), interaction, true, start_corner_radius);
        const luil::ui_color primary { item_.exists ? palette.primary_foreground : palette.disabled_foreground };
        const luil::ui_color accent { item_.exists ? palette.accent : palette.warning_accent };
        const float icon_box_size { start_icon_size * scale };
        const luil::rect_f icon { box.x + start_row_inset * scale, box.y + (box.height - icon_box_size) / 2.0f, icon_box_size, icon_box_size };
        fill_rounded(context.canvas, icon, start_corner_radius * scale, luil::with_alpha(accent, 0.10f));
        luil::draw_centered_glyph(context.canvas, item_.exists ? codicons::icon_repo : codicons::icon_warning, icon, icon_font(context, 16.0f), luil::solid_paint(accent));
        const float x { icon.x + icon_box_size + start_row_inset * scale };

        const float buttons { static_cast<float>(children().size()) * 28.0f * scale + 8.0f * scale };
        const float path_right { box.x + box.width - buttons - 8.0f * scale };
        float right { path_right };
        const SkFont small { ui_font(context, 11.0f) };
        const float text_top { box.y + (box.height - start_card_text_height * scale) / 2.0f };
        const float title_baseline { text_top + luil::centered_text_baseline(small, start_title_height * scale) };
        if (item_.exists == false)
        {
            const std::u8string warning { u8"경로를 찾을 수 없음" };
            const float width { luil::measure_text(warning, small) };
            static_cast<void>(luil::draw_text_within(context.canvas, warning, right - width, title_baseline, width, small, luil::solid_paint(palette.warning_accent)));
            right -= width + 12.0f * scale;
        }
        else if (when_.empty() == false && box.width >= 480.0f * scale)
        {
            const float width { luil::measure_text(when_, small) };
            static_cast<void>(luil::draw_text_within(context.canvas, when_, right - width, title_baseline, width, small, luil::solid_paint(palette.secondary_foreground)));
            right -= width + 12.0f * scale;
        }

        const SkFont name_font { ui_font(context, 13.0f, true) };
        const float drawn = luil::draw_text_within(
            context.canvas, item_.display_name, x, text_top + luil::centered_text_baseline(name_font, start_title_height * scale), std::max(right - x, 0.0f), name_font, luil::solid_paint(primary));
        if (open_)
        {
            const std::u8string badge { u8"열림" };
            const float width { luil::measure_text(badge, small) + 10.0f * scale };
            const float height { 16.0f * scale };
            const luil::rect_f pill { x + drawn + 8.0f * scale, text_top + (start_title_height * scale - height) / 2.0f, width, height };
            if (pill.x + pill.width < right)
            {
                fill_rounded(context.canvas, pill, 3.0f * scale, luil::with_alpha(palette.accent, 0.18f));
                static_cast<void>(luil::draw_text_within(
                    context.canvas, badge, pill.x + 5.0f * scale, pill.y + luil::centered_text_baseline(small, height), width, small, luil::solid_paint(palette.primary_foreground)));
            }
        }
        static_cast<void>(luil::draw_text_within(context.canvas, item_.path, x, text_top + start_title_height * scale + luil::centered_text_baseline(small, start_path_height * scale),
            std::max(path_right - x, 0.0f), small, luil::solid_paint(palette.secondary_foreground)));
        draw_children(context, interaction);
    }

    // ---------------------------------------------------------------- start_card_element

    start_card_element::start_card_element(std::u8string owner, const char32_t glyph, std::u8string title, std::u8string description, luil::input_action action, const bool enabled)
        : ui_element { luil::ui_element_id { kind_start_card, std::move(owner) } }
        , glyph_ { glyph }
        , title_ { std::move(title) }
        , description_ { std::move(description) }
    {
        set_tooltip(description_);
        set_enabled(enabled);
        set_cursor(luil::ui_cursor::hand);
        set_action(luil::ui_trigger::left_click, action_of(std::move(action)));
    }

    void start_card_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    luil::access_info start_card_element::accessibility() const
    {
        return { .role = luil::access_role::button, .name = title_ };
    }

    void start_card_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        const luil::rect_f box { bounds() };
        const float scale { context.scale };
        const luil::ui_color_palette& palette { context.palette };
        const float radius { start_corner_radius * scale };
        fill_rounded(context.canvas, box, radius, palette.surface_background);
        luil::draw_hover_fill(context, box, id(), interaction, enabled(), start_corner_radius);
        SkPaint border { luil::solid_paint(palette.divider) };
        border.setStyle(SkPaint::kStroke_Style);
        border.setStrokeWidth(scale);
        border.setAntiAlias(true);
        context.canvas.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(box.x + 0.5f * scale, box.y + 0.5f * scale, box.width - scale, box.height - scale), radius, radius), border);

        const float icon_box { 32.0f * scale };
        const luil::rect_f icon { box.x + 12.0f * scale, box.y + (box.height - icon_box) / 2.0f, icon_box, icon_box };
        fill_rounded(context.canvas, icon, 4.0f * scale, luil::with_alpha(palette.accent, 0.14f));
        luil::draw_centered_glyph(context.canvas, glyph_, icon, icon_font(context, 16.0f), luil::solid_paint(enabled() ? palette.accent : palette.disabled_foreground));

        const float x { icon.x + icon_box + 12.0f * scale };
        const float width { box.x + box.width - x - 10.0f * scale };
        const float text_top { box.y + (box.height - start_card_text_height * scale) / 2.0f };
        const SkFont title { ui_font(context, 13.0f, true) };
        static_cast<void>(luil::draw_text_within(context.canvas, title_, x, text_top + luil::centered_text_baseline(title, start_title_height * scale), width, title,
            luil::solid_paint(enabled() ? palette.primary_foreground : palette.disabled_foreground)));
        const SkFont small { ui_font(context, 11.0f) };
        static_cast<void>(luil::draw_text_within(context.canvas, description_, x, text_top + start_title_height * scale + luil::centered_text_baseline(small, start_path_height * scale), width, small,
            luil::solid_paint(palette.secondary_foreground)));
    }
} // namespace cyella::ui
