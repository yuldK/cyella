#include "application/app_messages.h"
#include "domain/text_catalog.h"
#include "presentation/dialog_intents.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui/ui_parts.h"
#include "presentation/ui_intents.h"

#include "luil/ui/font_scale.h"
#include "luil/ui/stack_element.h"
#include "luil/ui/virtual_list_element.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"

#include <algorithm>
#include <memory>

namespace cyella::ui {
    namespace {
        constexpr float conflict_row_height { 44.0f };
        constexpr float conflict_stacked_row_height { 76.0f };
        constexpr float conflict_stacked_minimum_width { 720.0f };
        constexpr float conflict_footer_height { 48.0f };
        constexpr float conflict_button_height { 24.0f };
        constexpr float conflict_button_gap { 6.0f };
        constexpr float conflict_button_padding { 12.0f };

        const color_selector dim_color { [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } };

        // 충돌 파일 한 줄이다. 이름과 뜻을 왼쪽에, ours·theirs·해결 표시 단추를 오른쪽에 둔다.
        class conflict_row_element final : public luil::ui_element
        {
        public:
            conflict_row_element(conflict_file_view file, std::string repository_id, bool busy, const bool rebasing, const bool stacked)
                : ui_element { luil::ui_element_id { kind_conflict_row, u8"conflict-" + file.name + u8"-" + number_text(std::hash<std::string> {}(file.path) % 100000) } }
                , file_ { std::move(file) }
                , stacked_ { stacked }
            {
                set_hit_opaque(false);
                set_text_tooltip(*this, to_u8(file_.path));
                const std::u8string tag { to_u8(file_.path) };
                add_button(u8"conflict-ours:" + tag, rebasing ? tr(u8"conflict.ours_rebase") : tr(u8"conflict.ours"), luil::text_button_visual::normal,
                    message(submit_operation_intent { repository_id, resolve_file_request { .path = file_.path, .side = conflict_side::ours } }), busy, 96.0f,
                    rebasing ? tr(u8"conflict.ours_rebase_tooltip") : tr(u8"conflict.ours_tooltip"));
                add_button(u8"conflict-theirs:" + tag, rebasing ? tr(u8"conflict.theirs_rebase") : tr(u8"conflict.theirs"), luil::text_button_visual::normal,
                    message(submit_operation_intent { repository_id, resolve_file_request { .path = file_.path, .side = conflict_side::theirs } }), busy, 132.0f,
                    rebasing ? tr(u8"conflict.theirs_rebase_tooltip") : tr(u8"conflict.theirs_tooltip"));
                add_button(u8"conflict-resolved:" + tag, tr(u8"conflict.mark_resolved"), luil::text_button_visual::accent,
                    message(submit_operation_intent { repository_id, mark_resolved_request { .paths = { file_.path } } }), busy, 84.0f, tr(u8"conflict.mark_resolved_tooltip"));
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                const float scale { context.scale };
                float right { context.slot.x + context.slot.width - 12.0f * scale };
                const float height { conflict_button_height * scale };
                const float top { stacked_ ? context.slot.y + 42.0f * scale : context.slot.y + (context.slot.height - height) / 2.0f };
                for (auto it { children().rbegin() }; it != children().rend(); ++it)
                {
                    // 정해 둔 폭은 바닥이다. 글이 더 길면(영어, 큰 글꼴) 글 폭에 양쪽 여백을 더한 만큼 늘린다.
                    const std::size_t index { static_cast<std::size_t>(children().rend() - it) - 1 };
                    const float text_width { context.measure_text()(labels_[index], luil::scaled_font_size(12.0f, scale)) };
                    const float width { std::max(widths_[index] * scale, text_width + 2.0f * conflict_button_padding * scale) };
                    right -= width;
                    (*it)->arrange(context.for_child({ right, top, width, height }));
                    right -= conflict_button_gap * scale;
                }
                text_right_ = stacked_ ? context.slot.x + context.slot.width - 12.0f * scale : right;
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::static_text, .name = tr(u8"conflict.file_name", { { u8"path", file_.path } }) };
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const luil::rect_f box { bounds() };
                const float scale { context.scale };
                const luil::ui_color_palette& palette { context.palette };
                const float text_height { stacked_ ? 36.0f * scale : box.height };
                float x { box.x + 14.0f * scale };
                const float letter { 18.0f * scale };
                const luil::rect_f badge { x, box.y + (text_height - letter) / 2.0f, letter, letter };
                SkPaint fill { luil::solid_paint(luil::with_alpha(palette.error_accent, 0.18f)) };
                fill.setAntiAlias(true);
                context.canvas.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(badge.x, badge.y, badge.width, badge.height), 3.0f * scale, 3.0f * scale), fill);
                const SkFont letter_font { code_font(context, 10.5f) };
                const float letter_width { luil::measure_text(u8"U", letter_font) };
                static_cast<void>(luil::draw_text_within(context.canvas, u8"U", badge.x + (letter - letter_width) / 2.0f, badge.y + luil::centered_text_baseline(letter_font, letter), letter,
                    letter_font, luil::solid_paint(palette.error_accent)));
                x += letter + 10.0f * scale;
                const float right { std::max(text_right_ - 8.0f * scale, x) };
                const SkFont font { ui_font(context, 12.0f) };
                const float name_baseline { box.y + luil::centered_text_baseline(font, text_height) };
                const float drawn { luil::draw_text_within(context.canvas, file_.name, x, name_baseline, right - x, font, luil::solid_paint(palette.primary_foreground)) };
                const SkFont small { ui_font(context, 11.0f) };
                std::u8string detail { file_.folder };
                if (file_.meaning.empty() == false)
                    detail += (detail.empty() ? u8"" : u8"  ·  ") + file_.meaning;
                const float detail_x { x + drawn + 8.0f * scale };
                if (detail.empty() == false && right - detail_x > 0.0f)
                    static_cast<void>(luil::draw_text_within(
                        context.canvas, detail, detail_x, box.y + luil::centered_text_baseline(small, text_height), right - detail_x, small, luil::solid_paint(palette.secondary_foreground)));
                context.canvas.drawLine(box.x, box.y + box.height - scale, box.x + box.width, box.y + box.height - scale, luil::solid_paint(palette.divider));
                draw_children(context, interaction);
            }

        private:
            void add_button(std::u8string owner, std::u8string label, const luil::text_button_visual visual, luil::input_action action, const bool disabled, const float width, std::u8string tooltip)
            {
                labels_.push_back(label);
                auto button { make_text_button(std::move(owner), std::move(label), visual, std::move(action), disabled == false) };
                set_text_tooltip(*button, std::move(tooltip));
                button->set_tab_stop(true);
                widths_.push_back(width);
                add_child(std::move(button));
            }

            conflict_file_view file_ {};
            std::vector<std::u8string> labels_ {};
            std::vector<float> widths_ {};
            float text_right_ { 0.0f };
            bool stacked_ { false };
        };
    } // namespace

    std::unique_ptr<luil::ui_element> build_conflict_screen(const repository_view& repository, const conflict_view& conflict, const float width, const float scroll, feedback_sources& sources)
    {
        const std::string& repo { repository.repository_id };
        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"conflict-area" }, luil::stack_config {}) };

        const luil::stack_config header_config { .spacing = 4.0f, .padding = luil::edge_insets { 16.0f, 12.0f, 16.0f, 8.0f } };
        auto header { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"conflict-header" }, header_config) };
        float header_height { 20.0f };
        std::size_t header_lines { 0 };
        const auto add_header = [&](std::u8string owner, std::u8string text, const text_style style, const float line_height) {
            const float text_height { std::clamp(wrapped_text_height(text, std::max(width - 32.0f, 120.0f), style, line_height), line_height, line_height * 3.0f) };
            header->add(make_wrapped_text(std::move(owner), std::move(text), style, line_height), text_height);
            header_height += text_height + (header_lines++ > 0 ? 4.0f : 0.0f);
        };
        add_header(u8"conflict-title", conflict.title, text_style { .size = 14.0f, .bold = true }, 22.0f);
        add_header(u8"conflict-progress", conflict.progress, text_style { .size = 12.0f }, 18.0f);
        add_header(u8"conflict-detail", conflict.detail.empty() ? tr(u8"conflict.default_detail") : conflict.detail, text_style { .size = 11.0f, .color = dim_color }, 16.0f);
        const bool rebasing { conflict.kind == repository_operation_state::rebase };
        if (rebasing)
            add_header(u8"conflict-sides-help", tr(u8"conflict.rebase_sides_help"), text_style { .size = 11.0f, .color = dim_color }, 16.0f);
        // 인터랙티브 rebase는 멈춘 할 일을 한 줄 더 보인다.
        const bool has_note { conflict.todo_note.empty() == false };
        if (has_note)
            add_header(u8"conflict-todo", conflict.todo_note, text_style { .size = 11.0f, .color = dim_color }, 16.0f);
        column->add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"conflict-header-bar" }, bar_style { .divider_bottom = true }, std::move(header)), header_height);

        if (conflict.files.empty())
        {
            auto empty { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"conflict-empty" }, luil::stack_config { .padding = luil::edge_insets::all(16.0f) }) };
            empty->add_flexible_gap();
            empty->add(make_text(u8"conflict-resolved-note", tr(u8"conflict.all_resolved_note"), text_style { .size = 12.0f, .color = dim_color, .align = 1 }), 22.0f);
            empty->add_flexible_gap();
            column->add_flexible(std::move(empty));
        }
        else
        {
            auto files { std::make_shared<std::vector<conflict_file_view>>(conflict.files) };
            luil::virtual_list_config config {};
            config.owner = u8"conflict-files";
            const bool stacked { width < conflict_stacked_minimum_width };
            const float row_height { stacked ? conflict_stacked_row_height : conflict_row_height };
            for (std::size_t index { 0 }; index < files->size(); ++index)
                config.items.push_back({ .key = u8"c" + number_text(index), .label = (*files)[index].name, .height = row_height, .enabled = false });
            config.row_height = row_height;
            config.scroll_offset = scroll;
            const bool busy { conflict.busy };
            config.build_row = [files, repo, busy, rebasing, stacked](const luil::virtual_list_item&, const std::size_t index, bool) -> std::unique_ptr<luil::ui_element> {
                return std::make_unique<conflict_row_element>((*files)[index], repo, busy, rebasing, stacked);
            };
            config.scroll = [](const float delta) { return message(scroll_intent { .target = scroll_target::diff, .value = delta }); };
            config.scroll_to = [](const float offset) { return message(scroll_intent { .target = scroll_target::diff, .value = offset, .absolute = true }); };
            config.edges = luil::scroll_edges { .shadows = true };
            auto list { std::make_unique<luil::virtual_list_element>(std::move(config)) };
            list->set_access_name(tr(u8"conflict.file_list_name", { { u8"count", conflict.files.size() } }));
            sources.diff = list.get();
            column->add_flexible(std::move(list));
        }

        const luil::stack_config footer_config { .direction = luil::stack_direction::row, .spacing = 8.0f, .padding = luil::edge_insets { 16.0f, 8.0f, 16.0f, 8.0f } };
        auto footer { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"conflict-footer" }, footer_config) };
        std::u8string status { conflict.files.empty() ? tr(u8"conflict.status_none") : tr(u8"conflict.status_remaining", { { u8"count", conflict.files.size() } }) };
        if (conflict.message.empty() == false)
            status = conflict.message;
        text_style status_style { .size = 11.0f, .color = conflict.message.empty() ? dim_color : [](const luil::ui_color_palette& palette) { return palette.warning_accent; } };
        footer->add_flexible(make_text(u8"conflict-status", status, std::move(status_style)));
        footer->add(
            make_text_button(u8"conflict-abort", tr(u8"conflict.abort"), luil::text_button_visual::danger, message(conflict_action_intent { conflict_action::abort_sequence }), conflict.busy == false),
            76.0f);
        if (conflict.can_skip)
        {
            auto skip = make_text_button(
                u8"conflict-skip", tr(u8"conflict.skip"), luil::text_button_visual::normal, message(conflict_action_intent { conflict_action::skip_sequence }), conflict.busy == false);
            set_text_tooltip(*skip, tr(u8"conflict.skip_tooltip"));
            footer->add(std::move(skip), 96.0f);
        }
        auto next = make_text_button(
            u8"conflict-continue", tr(u8"conflict.continue"), luil::text_button_visual::accent, message(conflict_action_intent { conflict_action::continue_sequence }), conflict.can_continue, 0, true);
        set_text_tooltip(*next, conflict.can_continue ? tr(u8"conflict.continue_tooltip") : tr(u8"conflict.continue_disabled_tooltip"));
        footer->add(std::move(next), 96.0f);
        auto footer_column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"conflict-footer-content" }, luil::stack_config {}) };
        const float help_height { conflict.files.empty() ? 0.0f : 24.0f };
        if (help_height > 0.0f)
        {
            const luil::stack_config help_config { .padding = luil::edge_insets { 16.0f, 4.0f, 16.0f, 4.0f } };
            auto help { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"conflict-continue-help-row" }, help_config) };
            help->add(make_text(u8"conflict-continue-help", tr(u8"conflict.continue_help"), text_style { .size = 11.0f, .color = dim_color }), 16.0f);
            footer_column->add(std::move(help), help_height);
        }
        footer_column->add(std::move(footer), conflict_footer_height);
        column->add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"conflict-footer-bar" }, bar_style { .divider_top = true }, std::move(footer_column)),
            conflict_footer_height + help_height);

        return std::make_unique<bar_element>(
            luil::ui_element_id { kind_panel, u8"conflict-panel" }, bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.surface_background; } }, std::move(column));
    }
} // namespace cyella::ui
