#include "presentation/ui/sidebar_extension_rows.h"

#include "presentation/ui/ui_helpers.h"

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

        // PR 행은 번호가 있으면 PR 아이콘, 동작 행은 동작을 닮은 아이콘이다.
        char32_t pull_request_row_glyph(const sidebar_row_view& row) noexcept
        {
            if (row.key == u8"pr:login")
                return codicons::icon_sign_in;
            if (row.key == u8"pr:refresh")
                return codicons::icon_refresh;
            if (row.key == u8"pr:create")
                return codicons::icon_add;
            if (row.key == u8"pr:closed")
                return codicons::icon_history;
            if (row.key == u8"pr:more")
                return codicons::icon_ellipsis;
            if (row.key == u8"pr:error")
                return codicons::icon_error;
            if (row.path.empty())
                return codicons::icon_info;
            return codicons::icon_git_pull_request;
        }

        char32_t row_glyph(const sidebar_row_view& row) noexcept
        {
            switch (row.item)
            {
            case sidebar_item_kind::submodule:
                return row.submodule == submodule_state::uninitialized ? codicons::icon_circle_outline : codicons::icon_file_submodule;
            case sidebar_item_kind::worktree:
                return row.worktree_current ? codicons::icon_check : codicons::icon_worktree;
            case sidebar_item_kind::lfs:
                return row.key == u8"lfs:status" ? codicons::icon_package : codicons::icon_file_binary;
            case sidebar_item_kind::pull_request:
                return pull_request_row_glyph(row);
            case sidebar_item_kind::ref:
                break;
            }
            return codicons::icon_git_branch;
        }

        luil::ui_color tone_color(const luil::ui_color_palette& palette, const badge_tone tone) noexcept
        {
            switch (tone)
            {
            case badge_tone::accent:
                return palette.accent;
            case badge_tone::warning:
                return status_color(palette, 'M');
            case badge_tone::danger:
                return palette.error_accent;
            case badge_tone::none:
                break;
            }
            return palette.secondary_foreground;
        }

        void fill_rounded(SkCanvas& canvas, const luil::rect_f& box, const float radius, const luil::ui_color color)
        {
            SkPaint paint { luil::solid_paint(color) };
            paint.setAntiAlias(true);
            canvas.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), radius, radius), paint);
        }
    } // namespace

    char32_t extension_section_glyph(const sidebar_section section) noexcept
    {
        switch (section)
        {
        case sidebar_section::submodules:
            return codicons::icon_file_submodule;
        case sidebar_section::worktrees:
            return codicons::icon_worktree;
        case sidebar_section::lfs:
            return codicons::icon_package;
        case sidebar_section::pull_requests:
            return codicons::icon_git_pull_request;
        case sidebar_section::local:
        case sidebar_section::remote:
        case sidebar_section::tags:
        case sidebar_section::stashes:
            break;
        }
        return codicons::icon_git_branch;
    }

    float draw_worktree_marker(luil::draw_context& context, const luil::rect_f& box, const float right)
    {
        const float scale { context.scale };
        luil::draw_centered_glyph(
            context.canvas, codicons::icon_worktree, { right - icon_size * scale, box.y, icon_size * scale, box.height }, icon_font(context, 12.0f), luil::solid_paint(context.palette.accent));
        return (icon_size + 6.0f) * scale;
    }

    void draw_extension_sidebar_row(luil::draw_context& context, const sidebar_row_view& row, const luil::rect_f& box)
    {
        const float scale { context.scale };
        const luil::ui_color_palette& palette { context.palette };
        const luil::ui_color dim { row.dimmed ? palette.disabled_foreground : palette.primary_foreground };
        float x { box.x + (row_padding + 20.0f + static_cast<float>(row.depth) * indent_step) * scale };
        const luil::ui_color icon_color { row.worktree_current ? palette.accent : row.dimmed ? palette.disabled_foreground : palette.secondary_foreground };
        luil::draw_centered_glyph(context.canvas, row_glyph(row), { x, box.y, icon_size * scale, box.height }, icon_font(context, 13.0f), luil::solid_paint(icon_color));
        x += (icon_size + 6.0f) * scale;

        float right { box.x + box.width - row_padding * scale };
        const SkFont small { ui_font(context, 11.0f) };
        if (row.badge.empty() == false)
        {
            const float text_width { luil::measure_text(row.badge, small) };
            const float pill_width { text_width + 10.0f * scale };
            const float pill_height { 16.0f * scale };
            const luil::rect_f pill { right - pill_width, box.y + (box.height - pill_height) / 2.0f, pill_width, pill_height };
            const luil::ui_color tone { tone_color(palette, row.tone) };
            fill_rounded(context.canvas, pill, pill_height / 2.0f, luil::with_alpha(tone, 0.16f));
            static_cast<void>(
                luil::draw_text_within(context.canvas, row.badge, pill.x + 5.0f * scale, pill.y + luil::centered_text_baseline(small, pill_height), pill_width, small, luil::solid_paint(tone)));
            right -= pill_width + 6.0f * scale;
        }

        const SkFont font { ui_font(context, 12.0f, row.worktree_current) };
        const float baseline { box.y + luil::centered_text_baseline(font, box.height) };
        const float drawn { luil::draw_text_within(context.canvas, row.label, x, baseline, std::max(right - x, 0.0f), font, luil::solid_paint(dim)) };
        if (row.detail.empty() == false)
        {
            const float detail_x { x + drawn + 8.0f * scale };
            if (right - detail_x > 0.0f)
                static_cast<void>(luil::draw_text_within(
                    context.canvas, row.detail, detail_x, box.y + luil::centered_text_baseline(small, box.height), right - detail_x, small, luil::solid_paint(palette.secondary_foreground)));
        }
    }
} // namespace cyella::ui
