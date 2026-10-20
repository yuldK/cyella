#include "presentation/ui/diff_webview_theme.h"

#include "presentation/ui/ui_helpers.h"

#include <cstdint>
#include <cstdio>

namespace cyella::ui {
    namespace {
        // 색의 rgb를 두고 알파만 바꾼다.
        luil::ui_color with_alpha_byte(const luil::ui_color color, const std::uint8_t alpha) noexcept
        {
            return (color & 0x00FFFFFFU) | (static_cast<luil::ui_color>(alpha) << 24U);
        }
    } // namespace

    std::string css_color(const luil::ui_color color)
    {
        const unsigned alpha { (color >> 24U) & 0xFFU };
        char buffer[16] {};
        if (alpha == 0xFFU)
            std::snprintf(buffer, sizeof(buffer), "#%06x", static_cast<unsigned>(color & 0x00FFFFFFU));
        else
            std::snprintf(buffer, sizeof(buffer), "#%06x%02x", static_cast<unsigned>(color & 0x00FFFFFFU), alpha);
        return buffer;
    }

    diff_theme make_diff_theme(const luil::ui_color_palette& palette, const std::u8string_view ui_family, const std::u8string_view code_family)
    {
        const bool dark { luil::is_dark_background(palette.window_background) };
        diff_theme theme {};
        theme.mode = dark ? diff_theme_mode::dark : diff_theme_mode::light;

        auto& tokens { theme.tokens };
        tokens["--window-background"] = css_color(palette.window_background);
        tokens["--surface-background"] = css_color(palette.surface_background);
        tokens["--primary-foreground"] = css_color(palette.primary_foreground);
        tokens["--secondary-foreground"] = css_color(palette.secondary_foreground);
        tokens["--disabled-foreground"] = css_color(palette.disabled_foreground);
        tokens["--divider"] = css_color(palette.divider);
        tokens["--input-background"] = css_color(palette.input_background);
        tokens["--control-border"] = css_color(palette.control_border);
        tokens["--error-accent"] = css_color(palette.error_accent);
        tokens["--success-accent"] = css_color(success_color(palette));
        tokens["--warning-accent"] = css_color(palette.warning_accent);
        tokens["--button-hover-background"] = css_color(palette.button_hover_background);
        tokens["--tooltip-background"] = css_color(palette.tooltip_background);
        tokens["--content-shadow"] = css_color(palette.content_shadow);
        tokens["--accent"] = css_color(palette.accent);
        tokens["--accent-hover"] = css_color(palette.accent_hover);
        tokens["--accent-soft"] = css_color(palette.accent_soft);
        tokens["--accent-emphasis-foreground"] = css_color(palette.accent_emphasis_foreground);
        tokens["--accent-foreground"] = css_color(palette.accent_foreground);
        for (std::uint8_t lane { 0 }; lane < 8; ++lane)
            tokens["--lane-" + std::to_string(lane)] = css_color(lane_color(palette, lane));

        // diff 줄 바탕이다. 줄 전체는 옅게, 단어 단위 강조는 같은 색을 진하게 깐다 (viewer.css의 짝과 같은 비율).
        const luil::ui_color added { diff_add_background(palette) };
        const luil::ui_color deleted { diff_delete_background(palette) };
        tokens["--diff-add-background"] = css_color(added);
        tokens["--diff-add-strong"] = css_color(with_alpha_byte(added, dark ? 0x66 : 0x50));
        tokens["--diff-del-background"] = css_color(deleted);
        tokens["--diff-del-strong"] = css_color(with_alpha_byte(deleted, dark ? 0x66 : 0x48));
        tokens["--diff-hunk-background"] = css_color(with_alpha_byte(palette.primary_foreground, dark ? 0x0d : 0x0a));
        const auto font_stack = [](const std::u8string_view family, const std::string& fallback) {
            if (family.empty())
                return fallback;
            std::string quoted { "\"" };
            for (const char8_t value : family)
            {
                if (value < 0x20 || value == 0x7f)
                    continue;
                if (value == u8'"' || value == u8'\\')
                    quoted.push_back('\\');
                quoted.push_back(static_cast<char>(value));
            }
            return quoted + "\", " + fallback;
        };
        tokens["--ui-font"] = font_stack(ui_family, "\"Segoe UI\", \"Malgun Gothic\", system-ui, sans-serif");
        tokens["--code-font"] = font_stack(code_family, "\"Cascadia Mono\", Consolas, \"Malgun Gothic\", monospace");
        return theme;
    }
} // namespace cyella::ui
