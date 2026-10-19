#include "application/app_messages.h"
#include "presentation/dialog_intents.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui/ui_parts.h"
#include "presentation/ui_intents.h"

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
        constexpr float conflict_row_height { 36.0f };
        constexpr float conflict_header_height { 84.0f };
        constexpr float conflict_footer_height { 48.0f };
        constexpr float conflict_button_height { 24.0f };
        constexpr float conflict_button_gap { 6.0f };

        const color_selector dim_color { [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } };

        // 충돌 파일 한 줄이다. 이름과 뜻을 왼쪽에, ours·theirs·해결 표시 단추를 오른쪽에 둔다.
        class conflict_row_element final : public luil::ui_element
        {
        public:
            conflict_row_element(conflict_file_view file, std::string repository_id, bool busy)
                : ui_element { luil::ui_element_id { kind_conflict_row, u8"conflict-" + file.name + u8"-" + number_text(std::hash<std::string> {}(file.path) % 100000) } }
                , file_ { std::move(file) }
            {
                set_hit_opaque(false);
                set_tooltip(to_u8(file_.path));
                const std::u8string tag { to_u8(file_.path) };
                add_button(u8"conflict-ours:" + tag, u8"현재(ours)", luil::text_button_visual::normal,
                    message(submit_operation_intent { repository_id, resolve_file_request { .path = file_.path, .side = conflict_side::ours } }), busy, 84.0f,
                    u8"이 파일을 현재 쪽(ours)의 내용으로 정하고 해결로 표시합니다");
                add_button(u8"conflict-theirs:" + tag, u8"들어오는(theirs)", luil::text_button_visual::normal,
                    message(submit_operation_intent { repository_id, resolve_file_request { .path = file_.path, .side = conflict_side::theirs } }), busy, 116.0f,
                    u8"이 파일을 들어오는 쪽(theirs)의 내용으로 정하고 해결로 표시합니다");
                add_button(u8"conflict-resolved:" + tag, u8"해결 표시", luil::text_button_visual::accent,
                    message(submit_operation_intent { repository_id, mark_resolved_request { .paths = { file_.path } } }), busy, 84.0f, u8"직접 고친 파일을 해결로 표시(git add)합니다");
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
                const float scale { context.scale };
                float right { context.slot.x + context.slot.width - 12.0f * scale };
                const float height { conflict_button_height * scale };
                const float top { context.slot.y + (context.slot.height - height) / 2.0f };
                for (auto it { children().rbegin() }; it != children().rend(); ++it)
                {
                    const float width { widths_[static_cast<std::size_t>(children().rend() - it) - 1] * scale };
                    right -= width;
                    (*it)->arrange(context.for_child({ right, top, width, height }));
                    right -= conflict_button_gap * scale;
                }
                text_right_ = right;
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::static_text, .name = u8"충돌 파일 " + to_u8(file_.path) };
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const luil::rect_f box { bounds() };
                const float scale { context.scale };
                const luil::ui_color_palette& palette { context.palette };
                float x { box.x + 14.0f * scale };
                const float letter { 18.0f * scale };
                const luil::rect_f badge { x, box.y + (box.height - letter) / 2.0f, letter, letter };
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
                const float name_baseline { box.y + luil::centered_text_baseline(font, box.height) };
                const float drawn { luil::draw_text_within(context.canvas, file_.name, x, name_baseline, right - x, font, luil::solid_paint(palette.primary_foreground)) };
                const SkFont small { ui_font(context, 11.0f) };
                std::u8string detail { file_.folder };
                if (file_.meaning.empty() == false)
                    detail += (detail.empty() ? u8"" : u8"  ·  ") + file_.meaning;
                const float detail_x { x + drawn + 8.0f * scale };
                if (detail.empty() == false && right - detail_x > 0.0f)
                    static_cast<void>(luil::draw_text_within(
                        context.canvas, detail, detail_x, box.y + luil::centered_text_baseline(small, box.height), right - detail_x, small, luil::solid_paint(palette.secondary_foreground)));
                draw_children(context, interaction);
            }

        private:
            void add_button(std::u8string owner, std::u8string label, const luil::text_button_visual visual, luil::input_action action, const bool disabled, const float width, std::u8string tooltip)
            {
                auto button { make_text_button(std::move(owner), std::move(label), visual, std::move(action), disabled == false) };
                button->set_tooltip(std::move(tooltip));
                button->set_tab_stop(false);
                widths_.push_back(width);
                add_child(std::move(button));
            }

            conflict_file_view file_ {};
            std::vector<float> widths_ {};
            float text_right_ { 0.0f };
        };
    } // namespace

    std::unique_ptr<luil::ui_element> build_conflict_screen(const repository_view& repository, const conflict_view& conflict, const float scroll, feedback_sources& sources)
    {
        const std::string& repo { repository.repository_id };
        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"conflict-area" }, luil::stack_config {}) };

        const luil::stack_config header_config { .spacing = 4.0f, .padding = luil::edge_insets { 16.0f, 12.0f, 16.0f, 8.0f } };
        auto header { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"conflict-header" }, header_config) };
        header->add(make_text(u8"conflict-title", conflict.title, text_style { .size = 14.0f, .bold = true }), 22.0f);
        header->add(make_text(u8"conflict-progress", conflict.progress, text_style { .size = 12.0f }), 18.0f);
        header->add(make_text(u8"conflict-detail", conflict.detail.empty() ? std::u8string { u8"충돌은 닫을 수 없습니다. 끝내려면 계속하거나 중단하세요." } : conflict.detail,
                        text_style { .size = 11.0f, .color = dim_color }),
            16.0f);
        column->add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"conflict-header-bar" }, bar_style { .divider_bottom = true }, std::move(header)), conflict_header_height);

        if (conflict.files.empty())
        {
            auto empty { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"conflict-empty" }, luil::stack_config { .padding = luil::edge_insets::all(16.0f) }) };
            empty->add_flexible_gap();
            empty->add(make_text(u8"conflict-resolved-note", u8"충돌을 모두 풀었습니다. 계속을 눌러 이어 가세요.", text_style { .size = 12.0f, .color = dim_color, .align = 1 }), 22.0f);
            empty->add_flexible_gap();
            column->add_flexible(std::move(empty));
        }
        else
        {
            auto files { std::make_shared<std::vector<conflict_file_view>>(conflict.files) };
            luil::virtual_list_config config {};
            config.owner = u8"conflict-files";
            for (std::size_t index { 0 }; index < files->size(); ++index)
                config.items.push_back({ .key = u8"c" + number_text(index), .label = (*files)[index].name, .height = conflict_row_height, .enabled = false });
            config.row_height = conflict_row_height;
            config.scroll_offset = scroll;
            const bool busy { conflict.busy };
            config.build_row = [files, repo, busy](const luil::virtual_list_item&, const std::size_t index, bool) -> std::unique_ptr<luil::ui_element> {
                return std::make_unique<conflict_row_element>((*files)[index], repo, busy);
            };
            config.scroll = [](const float delta) { return message(scroll_intent { .target = scroll_target::diff, .value = delta }); };
            config.scroll_to = [](const float offset) { return message(scroll_intent { .target = scroll_target::diff, .value = offset, .absolute = true }); };
            config.edges = luil::scroll_edges { .shadows = true };
            auto list { std::make_unique<luil::virtual_list_element>(std::move(config)) };
            list->set_access_name(u8"충돌 파일 " + number_text(conflict.files.size()) + u8"개");
            sources.diff = list.get();
            column->add_flexible(std::move(list));
        }

        const luil::stack_config footer_config { .direction = luil::stack_direction::row, .spacing = 8.0f, .padding = luil::edge_insets { 16.0f, 8.0f, 16.0f, 8.0f } };
        auto footer { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"conflict-footer" }, footer_config) };
        std::u8string status { conflict.files.empty() ? std::u8string { u8"충돌 없음" } : u8"충돌 " + number_text(conflict.files.size()) + u8"개 남음" };
        if (conflict.message.empty() == false)
            status = conflict.message;
        text_style status_style { .size = 11.0f, .color = conflict.message.empty() ? dim_color : [](const luil::ui_color_palette& palette) { return palette.warning_accent; } };
        footer->add_flexible(make_text(u8"conflict-status", status, std::move(status_style)));
        footer->add(
            make_text_button(u8"conflict-abort", u8"중단", luil::text_button_visual::danger, message(conflict_action_intent { conflict_action::abort_sequence }), conflict.busy == false), 76.0f);
        if (conflict.can_skip)
            footer->add(
                make_text_button(u8"conflict-skip", u8"건너뛰기", luil::text_button_visual::normal, message(conflict_action_intent { conflict_action::skip_sequence }), conflict.busy == false), 96.0f);
        footer->add(
            make_text_button(u8"conflict-continue", u8"계속", luil::text_button_visual::accent, message(conflict_action_intent { conflict_action::continue_sequence }), conflict.can_continue, 0, true),
            96.0f);
        column->add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"conflict-footer-bar" }, bar_style { .divider_top = true }, std::move(footer)), conflict_footer_height);

        return std::make_unique<bar_element>(
            luil::ui_element_id { kind_panel, u8"conflict-panel" }, bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.surface_background; } }, std::move(column));
    }
} // namespace cyella::ui
