#pragma once

#include "presentation/settings/settings_view.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"

#include "luil/ui/dialog_elements.h"
#include "luil/ui/stack_element.h"

#include <memory>
#include <string>
#include <utility>

namespace cyella::ui::pages {
    // 환경 설정 쪽들이 같이 쓰는 치수와 조립기다. `settings_ui.cpp`의 같은 이름 조각과 값이 같다 (그 파일의 조각은 익명 namespace라 새 쪽이 빌려 쓰지 못한다).
    inline constexpr float section_height { 30.0f };
    inline constexpr float line_height { 18.0f };
    inline constexpr float label_height { 17.0f };
    inline constexpr float input_height { 28.0f };
    inline constexpr float button_height { 26.0f };
    inline constexpr float row_height { 30.0f };
    inline constexpr float small_gap { 6.0f };
    inline constexpr float large_gap { 14.0f };

    inline const color_selector dim { [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } };

    // 환경 설정 화면의 `tone`(좋음·주의·나쁨)을 색으로 바꾼다 (서명 쪽이 쓴다).
    inline color_selector tone_color(const tone value)
    {
        return [value](const luil::ui_color_palette& palette) {
            switch (value)
            {
            case tone::good:
                return success_color(palette);
            case tone::warning:
                return palette.warning_accent;
            case tone::bad:
                return palette.error_accent;
            case tone::neutral:
                break;
            }
            return palette.secondary_foreground;
        };
    }

    inline color_selector warning_color()
    {
        return [](const luil::ui_color_palette& palette) { return palette.warning_accent; };
    }

    inline color_selector error_color()
    {
        return [](const luil::ui_color_palette& palette) { return palette.error_accent; };
    }

    // 쪽 하나의 내용을 쌓는다. 측정 단계가 없어서 담을 때 높이를 함께 더한다.
    class page_column
    {
    public:
        explicit page_column(std::u8string owner)
            : column_ { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, std::move(owner) }, luil::stack_config { .padding = luil::edge_insets { 0.0f, 0.0f, 0.0f, 0.0f } }) }
        {}

        void add(std::unique_ptr<luil::ui_element> child, const float height)
        {
            column_->add(std::move(child), height);
            height_ += height;
        }

        void gap(const float height)
        {
            column_->add_gap(height);
            height_ += height;
        }

        [[nodiscard]] float height() const noexcept
        {
            return height_;
        }

        [[nodiscard]] std::unique_ptr<luil::ui_element> take()
        {
            return std::move(column_);
        }

    private:
        std::unique_ptr<luil::stack_element> column_;
        float height_ { 0.0f };
    };

    inline std::unique_ptr<luil::stack_element> make_row(std::u8string owner, const float spacing = small_gap)
    {
        luil::stack_config config { .direction = luil::stack_direction::row, .spacing = spacing };
        config.cross_alignment = luil::stack_alignment::center;
        return std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, std::move(owner) }, config);
    }

    inline std::unique_ptr<luil::ui_element> section_title(std::u8string owner, std::u8string text)
    {
        return make_text(std::move(owner), std::move(text), text_style { .size = 13.0f, .bold = true });
    }

    inline std::unique_ptr<luil::ui_element> help_line(std::u8string owner, std::u8string text, color_selector color = dim)
    {
        return make_text(std::move(owner), std::move(text), text_style { .size = 11.0f, .color = std::move(color) });
    }

    inline std::unique_ptr<luil::ui_element> field_label(std::u8string owner, std::u8string text)
    {
        return make_text(std::move(owner), std::move(text), text_style { .size = 11.0f, .color = dim, .bold = true });
    }

    inline std::unique_ptr<luil::ui_element> code_line(std::u8string owner, std::u8string text)
    {
        return make_text(std::move(owner), std::move(text), text_style { .size = 11.0f, .color = dim, .code = true });
    }

    inline std::unique_ptr<luil::ui_element> input_of(
        const luil::ui_element_kind kind, std::u8string owner, const luil::text_input_view& view, std::u8string placeholder, std::u8string access_name = {})
    {
        auto input { std::make_unique<luil::text_input_element>(luil::ui_element_id { kind, std::move(owner) }, view, luil::text_input_config { .placeholder = std::move(placeholder) }) };
        if (access_name.empty() == false)
            input->set_access_name(std::move(access_name));
        return input;
    }

    inline std::unique_ptr<luil::ui_element> button_of(
        std::u8string owner, std::u8string text, luil::input_action action, const bool enabled = true, const luil::text_button_visual visual = luil::text_button_visual::normal)
    {
        return make_text_button(std::move(owner), std::move(text), visual, std::move(action), enabled);
    }

    inline std::u8string key_of(const std::string& command)
    {
        return std::u8string { command.begin(), command.end() };
    }
} // namespace cyella::ui::pages
