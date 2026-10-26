#include "domain/text_catalog.h"
#include "presentation/dialog_intents.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ref_chip_element.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui/ui_parts.h"

#include "luil/ui/check_element.h"
#include "luil/ui/choice_group_element.h"
#include "luil/ui/dialog_elements.h"
#include "luil/ui/modal_host_element.h"
#include "luil/ui/scroll_area_element.h"
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
        constexpr float form_button_gap { 8.0f };
        constexpr float form_footer_padding { 12.0f };
        constexpr float form_scroll_inset { 8.0f };

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

    std::unique_ptr<luil::ui_element> build_form_dialog(const form_view& form, const float logical_width, const float logical_height, feedback_sources& sources)
    {
        const float width { std::min(form_width, std::max(logical_width - 32.0f, 280.0f)) };

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
            caption.close_tooltip = tr(u8"form.close");
        }
        const float caption_height { luil::dialog_caption_element::height_for(caption) };
        column->add(std::make_unique<luil::dialog_caption_element>(std::move(caption)), caption_height);

        const luil::stack_config body_config { .spacing = form_spacing, .padding = luil::edge_insets::all(form_padding) };
        auto body { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, form.id + u8"-body" }, body_config) };
        form_body content { *body, width - form_padding * 2.0f - form_scroll_inset - luil::scroll_area_bar_width, form_padding * 2.0f - form_spacing, form.id, 0 };

        for (const form_note& note : form.notes)
            content.add_text(note.text, text_style { .size = note.code ? 11.0f : 12.0f, .color = note.warning ? warning_color : color_selector {}, .code = note.code });

        const auto add_choices = [&form, &content] {
            if (form.choices.empty())
                return;
            if (form.choices_label.empty() == false)
                content.add(make_text(form.id + u8"-choices-label", form.choices_label, text_style { .size = 11.0f, .color = dim_color }), form_label_height);
            luil::choice_group_config choices { .owner = form.id + u8"-choices", .style = luil::choice_style::radio, .name = form.choices_label };
            for (std::size_t index { 0 }; index < form.choices.size(); ++index)
                choices.items.push_back({ .value = number_text(index), .label = form.choices[index].label });
            choices.selected = number_text(form.chosen);
            choices.select = [](const std::u8string& value) { return message(form_choose_intent { index_of(value) }); };
            const float height { luil::choice_group_element::height_for(choices) };
            content.add(std::make_unique<luil::choice_group_element>(std::move(choices)), height);
        };
        // 선택지가 아래 칸의 뜻을 정하면(게시할 곳) 칸보다 먼저 보인다.
        if (form.choices_first)
            add_choices();

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

        if (form.choices_first == false)
            add_choices();

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
        float buttons_width { 0.0f };
        for (const form_button& button : form.buttons)
            buttons_width += std::max(72.0f, text_button_width(button.label) + 12.0f) + form_button_gap;
        const bool stacked { buttons_width - form_button_gap > width - form_padding * 2.0f };
        const luil::stack_config buttons_config { .direction = stacked ? luil::stack_direction::column : luil::stack_direction::row, .spacing = form_button_gap };
        auto buttons { std::make_unique<luil::stack_element>(std::move(buttons_id), buttons_config) };
        if (stacked == false)
            buttons->add_flexible_gap();
        for (const form_button& button : form.buttons)
        {
            const std::u8string owner { form.id + u8"-" + button.key };
            buttons->add(make_text_button(owner, button.label, visual_of(button.tone), message(form_button_intent { button.key }), button.enabled, 0, button.default_button),
                stacked ? form_button_height : std::max(72.0f, text_button_width(button.label) + 12.0f));
            if (button.default_button && focus_entry == luil::ui_element_id {})
                focus_entry = luil::ui_element_id { kind_text_button, owner };
        }
        config.focus_entry = focus_entry;
        const float count { static_cast<float>(form.buttons.size()) };
        const float buttons_height { stacked ? form_button_height * count + form_button_gap * std::max(count - 1.0f, 0.0f) : form_button_height };
        const float footer_height { buttons_height + form_footer_padding * 2.0f };
        config.content_height = std::min(caption_height + content.height + footer_height + 2.0f, std::max(logical_height - 32.0f, 160.0f));
        luil::scroll_area_config scroll {};
        scroll.owner = form.id + u8"-scroll";
        scroll.content_height = content.height;
        scroll.scroll_offset = form.scroll;
        scroll.scroll = [](const float delta) { return message(form_scroll_intent { .value = delta }); };
        scroll.scroll_to = [](const float offset) { return message(form_scroll_intent { .value = offset, .absolute = true }); };
        scroll.edges = luil::scroll_edges { .shadows = true };
        auto area { std::make_unique<luil::scroll_area_element>(std::move(scroll)) };
        area->set_content(std::move(body));
        sources.form = area.get();
        const luil::stack_config inset_config { .padding = luil::edge_insets { 0.0f, 0.0f, form_scroll_inset, 0.0f } };
        auto inset { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, form.id + u8"-scroll-inset" }, inset_config) };
        inset->add_flexible(std::move(area));
        column->add_flexible(std::move(inset));
        const luil::stack_config footer_config { .padding = luil::edge_insets { form_padding, form_footer_padding, form_padding, form_footer_padding } };
        auto footer { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, form.id + u8"-footer-content" }, footer_config) };
        footer->add_flexible(std::move(buttons));
        column->add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, form.id + u8"-footer" }, bar_style { .divider_top = true }, std::move(footer)), footer_height);
        auto host { std::make_unique<luil::modal_host_element>(std::move(config)) };
        host->set_content(std::move(column));
        return host;
    }
} // namespace cyella::ui
