#include "presentation/dialog_intents.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ref_chip_element.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui/ui_parts.h"

#include "luil/ui/check_element.h"
#include "luil/ui/choice_group_element.h"
#include "luil/ui/dialog_elements.h"
#include "luil/ui/modal_host_element.h"
#include "luil/ui/stack_element.h"

#include <algorithm>
#include <charconv>

namespace cyella::ui {
    namespace {
        constexpr float form_width { 480.0f };
        constexpr float form_padding { 16.0f };
        constexpr float form_spacing { 6.0f };
        constexpr float form_line_height { 17.0f };
        constexpr float form_field_height { 28.0f };
        constexpr float form_label_height { 16.0f };
        constexpr float form_button_height { 30.0f };

        const color_selector dim_color { [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } };
        const color_selector warning_color { [](const luil::ui_color_palette& palette) { return palette.warning_accent; } };
        const color_selector error_color { [](const luil::ui_color_palette& palette) { return palette.error_accent; } };

        luil::ui_element_kind field_kind(const std::size_t index) noexcept
        {
            switch (index)
            {
            case 0:
                return kind_form_input_0;
            case 1:
                return kind_form_input_1;
            default:
                break;
            }
            return kind_form_input_2;
        }

        luil::text_button_visual visual_of(const form_tone tone) noexcept
        {
            switch (tone)
            {
            case form_tone::accent:
                return luil::text_button_visual::accent;
            case form_tone::danger:
                return luil::text_button_visual::danger;
            case form_tone::normal:
                break;
            }
            return luil::text_button_visual::normal;
        }

        std::size_t index_of(const std::u8string& value)
        {
            std::size_t index { 0 };
            const std::string text { to_text(value) };
            static_cast<void>(std::from_chars(text.data(), text.data() + text.size(), index));
            return index;
        }

        // 글을 줄로 나눠 한 줄씩 담는다.
        struct form_body
        {
            luil::stack_element& stack;
            float inner_width { 0.0f };
            float height { 0.0f };
            std::u8string owner {};
            std::size_t counter { 0 };

            void add(std::unique_ptr<luil::ui_element> child, const float length)
            {
                stack.add(std::move(child), length);
                height += length + form_spacing;
            }

            void add_text(const std::u8string& text, const text_style& style)
            {
                const float size { style.code ? style.size * 1.1f : style.size };
                for (std::u8string& line : wrap_estimated(text, inner_width, size))
                    add(make_text(owner + u8"-text-" + number_text(counter++), std::move(line), style), form_line_height);
            }
        };
    } // namespace

    std::unique_ptr<luil::ui_element> build_form_dialog(const form_view& form, const float logical_width, const float logical_height)
    {
        const float width { std::min(form_width, std::max(logical_width - 32.0f, 280.0f)) };
        static_cast<void>(logical_height);

        luil::modal_host_config config {};
        config.owner = form.id;
        config.name = form.title;
        const bool dismissible { form.dismiss_key.empty() == false };
        if (dismissible)
            config.dismiss = action_of(message(form_button_intent { form.dismiss_key }));
        config.surface = floating_panel_config(8.0f, 0.4f);
        config.content_width = width;

        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, form.id + u8"-column" }, luil::stack_config {}) };
        luil::dialog_caption_config caption { .owner = form.id, .title = form.title };
        if (dismissible)
        {
            caption.close = action_of(message(form_button_intent { form.dismiss_key }));
            caption.close_tooltip = u8"닫기";
        }
        const float caption_height { luil::dialog_caption_element::height_for(caption) };
        column->add(std::make_unique<luil::dialog_caption_element>(std::move(caption)), caption_height);

        const luil::stack_config body_config { .spacing = form_spacing, .padding = luil::edge_insets::all(form_padding) };
        auto body { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, form.id + u8"-body" }, body_config) };
        form_body content { *body, width - form_padding * 2.0f, form_padding * 2.0f - form_spacing, form.id, 0 };

        for (const form_note& note : form.notes)
            content.add_text(note.text, text_style { .size = note.code ? 11.0f : 12.0f, .color = note.warning ? warning_color : color_selector {}, .code = note.code });

        luil::ui_element_id focus_entry {};
        for (std::size_t index { 0 }; index < form.fields.size(); ++index)
        {
            const form_field& field { form.fields[index] };
            const std::u8string owner { form.id + u8"-field-" + number_text(index) };
            content.add(make_text(owner + u8"-label", field.label, text_style { .size = 11.0f, .color = dim_color }), form_label_height);
            // 라벨은 별도 글 요소라 입력 칸의 UIA 이름에 들지 않는다(이름이 안내 글 하나뿐이다: luil 입력 칸의 이름은 `placeholder`이고
            // `set_access_name`은 이를 덮지 못한다). 안내 글 앞에 라벨을 붙여 보조 기술과 UIA 스크립트가 칸을 라벨로 찾게 한다.
            std::u8string hint { field.label };
            if (field.placeholder.empty() == false && field.placeholder != field.label)
                hint += u8" · " + field.placeholder;
            luil::text_input_config input { .placeholder = std::move(hint) };
            input.secret = field.secret;
            input.reveal = field.reveal;
            content.add(std::make_unique<luil::text_input_element>(luil::ui_element_id { field_kind(index), owner }, field.view, std::move(input)), form_field_height);
            if (field.focus && focus_entry == luil::ui_element_id {})
                focus_entry = luil::ui_element_id { field_kind(index), owner };
        }

        if (form.choices.empty() == false)
        {
            if (form.choices_label.empty() == false)
                content.add(make_text(form.id + u8"-choices-label", form.choices_label, text_style { .size = 11.0f, .color = dim_color }), form_label_height);
            luil::choice_group_config choices { .owner = form.id + u8"-choices", .style = luil::choice_style::radio, .name = form.choices_label };
            for (std::size_t index { 0 }; index < form.choices.size(); ++index)
                choices.items.push_back({ .value = number_text(index), .label = form.choices[index].label });
            choices.selected = number_text(form.chosen);
            choices.select = [](const std::u8string& value) { return message(form_choose_intent { index_of(value) }); };
            const float height { luil::choice_group_element::height_for(choices) };
            content.add(std::make_unique<luil::choice_group_element>(std::move(choices)), height);
        }

        for (std::size_t index { 0 }; index < form.checks.size(); ++index)
        {
            const form_check& check { form.checks[index] };
            luil::check_config config_check { .owner = form.id + u8"-check-" + number_text(index), .label = check.label, .checked = check.checked };
            config_check.toggle = action_of(message(form_check_intent { index }));
            auto element { std::make_unique<luil::check_element>(std::move(config_check)) };
            element->set_enabled(check.enabled);
            content.add(std::move(element), luil::check_row_height);
        }

        for (const form_note& warning : form.warnings)
            content.add_text(warning.text, text_style { .size = 11.0f, .color = warning.warning ? warning_color : dim_color });
        if (form.preview.empty() == false)
            content.add_text(form.preview, text_style { .size = 11.0f, .color = dim_color, .code = true });
        if (form.error.empty() == false)
            content.add_text(form.error, text_style { .size = 11.0f, .color = error_color });

        luil::ui_element_id buttons_id { kind_layout, form.id + u8"-buttons" };
        const luil::stack_config buttons_config { .direction = luil::stack_direction::row, .spacing = 8.0f };
        auto buttons { std::make_unique<luil::stack_element>(std::move(buttons_id), buttons_config) };
        buttons->add_flexible_gap();
        for (const form_button& button : form.buttons)
        {
            const std::u8string owner { form.id + u8"-" + button.key };
            buttons->add(make_text_button(owner, button.label, visual_of(button.tone), message(form_button_intent { button.key }), button.enabled, 0, button.default_button),
                std::max(72.0f, text_button_width(button.label) + 12.0f));
            if (button.default_button && focus_entry == luil::ui_element_id {})
                focus_entry = luil::ui_element_id { kind_text_button, owner };
        }
        content.add(std::move(buttons), form_button_height);

        config.focus_entry = focus_entry;
        config.content_height = caption_height + content.height + 2.0f;
        column->add_flexible(std::move(body));
        auto host { std::make_unique<luil::modal_host_element>(std::move(config)) };
        host->set_content(std::move(column));
        return host;
    }
} // namespace cyella::ui
