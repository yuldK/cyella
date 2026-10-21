#include "presentation/ui/rebase/rebase_editor_ui.h"

#include "presentation/rebase/rebase_editor_intents.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/rebase/multiline_text_element.h"
#include "presentation/ui/segmented_control.h"
#include "presentation/ui/ui_helpers.h"

#include "luil/ui/check_element.h"
#include "luil/ui/choice_group_element.h"
#include "luil/ui/dialog_elements.h"
#include "luil/ui/modal_host_element.h"
#include "luil/ui/scroll_area_element.h"
#include "luil/ui/stack_element.h"
#include "luil/ui/virtual_list_element.h"

#include <algorithm>
#include <array>
#include <utility>

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"

namespace cyella::ui {
    namespace {
        constexpr float dialog_maximum_width { 1080.0f };
        constexpr float dialog_maximum_height { 760.0f };
        constexpr float dialog_margin { 16.0f };
        constexpr float side_width { 380.0f };
        constexpr float padding { 16.0f };
        constexpr float line_height { 18.0f };
        constexpr float warning_height { 20.0f };
        constexpr float row_height { 28.0f };
        constexpr float input_height { 28.0f };
        constexpr float body_height { 150.0f };
        constexpr float button_height { 26.0f };
        constexpr float preview_row_height { 22.0f };
        constexpr float footer_height { 56.0f };
        constexpr float compact_footer_height { 88.0f };
        constexpr float stacked_width { 820.0f };
        constexpr float stacked_list_height { 140.0f };
        constexpr float gap { 8.0f };

        const color_selector dim { [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } };
        const color_selector warning_color { [](const luil::ui_color_palette& palette) { return palette.warning_accent; } };
        const color_selector error_color { [](const luil::ui_color_palette& palette) { return palette.error_accent; } };

        constexpr std::array<rebase_action, 6> actions {
            rebase_action::pick,
            rebase_action::reword,
            rebase_action::squash,
            rebase_action::fixup,
            rebase_action::fixup_keep_message,
            rebase_action::drop,
        };

        char32_t action_glyph(const rebase_action action) noexcept
        {
            switch (action)
            {
            case rebase_action::pick:
                return codicons::icon_git_commit;
            case rebase_action::reword:
                return codicons::icon_edit;
            case rebase_action::squash:
                return codicons::icon_combine;
            case rebase_action::fixup:
                return codicons::icon_fold_up;
            case rebase_action::fixup_keep_message:
                return codicons::icon_replace;
            case rebase_action::drop:
                return codicons::icon_trash;
            }
            return codicons::icon_git_commit;
        }

        std::u8string action_value(const rebase_action action)
        {
            return std::u8string { rebase_action_label(action) };
        }

        std::optional<rebase_action> action_of_value(const std::u8string_view value)
        {
            for (const rebase_action action : actions)
                if (rebase_action_label(action) == value)
                    return action;
            return std::nullopt;
        }

        std::unique_ptr<luil::stack_element> make_column(std::u8string owner, const luil::stack_config config = {})
        {
            return std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, std::move(owner) }, config);
        }

        std::unique_ptr<luil::stack_element> make_row(std::u8string owner, const float spacing = gap)
        {
            luil::stack_config config { .direction = luil::stack_direction::row, .spacing = spacing };
            config.cross_alignment = luil::stack_alignment::center;
            return std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, std::move(owner) }, config);
        }

        // 쌓은 높이를 함께 세는 세로 묶음이다. 측정 단계가 없어 흘리는 창에 내용 높이를 알려야 한다.
        class measured_column
        {
        public:
            explicit measured_column(std::u8string owner)
                : column_ { make_column(std::move(owner)) }
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

            [[nodiscard]] std::unique_ptr<luil::stack_element> take()
            {
                return std::move(column_);
            }

        private:
            std::unique_ptr<luil::stack_element> column_;
            float height_ { 0.0f };
        };

        std::unique_ptr<luil::ui_element> warning_line(std::u8string owner, std::u8string text, const color_selector& color)
        {
            auto row { make_row(owner + u8"-row", 6.0f) };
            row->add(std::make_unique<icon_element>(luil::ui_element_id { kind_label, owner + u8"-icon" }, codicons::icon_warning, color), 16.0f);
            auto label { make_text(std::move(owner), text, text_style { .size = 11.5f, .color = color }) };
            label->set_tooltip(std::move(text));
            row->add_flexible(std::move(label));
            return row;
        }

        // ---------------------------------------------------------------- 목록

        luil::ui_color action_color(const luil::ui_color_palette& palette, const rebase_action action) noexcept
        {
            switch (action)
            {
            case rebase_action::pick:
                return palette.secondary_foreground;
            case rebase_action::reword:
                return palette.accent;
            case rebase_action::squash:
            case rebase_action::fixup:
            case rebase_action::fixup_keep_message:
                return palette.warning_accent;
            case rebase_action::drop:
                return palette.error_accent;
            }
            return palette.secondary_foreground;
        }

        // 할 일 한 줄의 내용이다: 잡는 손잡이, 번호, 동작 알약, SHA, 제목, "원격에 있음". 누름·끌기·고르기는 목록 행이 받는다.
        class rebase_row_element final : public luil::ui_element
        {
        public:
            explicit rebase_row_element(rebase_row_view row)
                : ui_element { luil::ui_element_id { kind_label, row.key + u8"-content" } }
                , row_ { std::move(row) }
            {
                set_hit_opaque(false);
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                // 이름은 목록 행(`virtual_list_item::label`)이 말한다. 내용은 구조다.
                return {};
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                static_cast<void>(interaction);
                const luil::rect_f box { bounds() };
                const float scale { context.scale };
                const luil::ui_color_palette& palette { context.palette };
                const SkPaint dim_paint { luil::solid_paint(palette.secondary_foreground) };
                float x { box.x + 6.0f * scale };
                luil::draw_centered_glyph(context.canvas, codicons::icon_gripper, { x, box.y, 14.0f * scale, box.height }, icon_font(context, 13.0f), dim_paint);
                x += 18.0f * scale;
                if (row_.melded)
                    x += 18.0f * scale;
                const SkFont small { ui_font(context, 11.0f) };
                static_cast<void>(luil::draw_text_within(context.canvas, number_text(row_.number), x, box.y + luil::centered_text_baseline(small, box.height), 22.0f * scale, small, dim_paint));
                x += 24.0f * scale;

                // 동작 알약이다. 색이 동작의 종류를 말한다 (합치기는 경고색, 지우기는 오류색).
                const luil::ui_color tone { action_color(palette, row_.action) };
                const float pill_width { 70.0f * scale };
                const float pill_height { 18.0f * scale };
                const SkRect pill { SkRect::MakeXYWH(x, box.y + (box.height - pill_height) / 2.0f, pill_width, pill_height) };
                SkPaint fill { luil::solid_paint(luil::with_alpha(tone, 0.16f)) };
                fill.setAntiAlias(true);
                context.canvas.drawRRect(SkRRect::MakeRectXY(pill, 4.0f * scale, 4.0f * scale), fill);
                luil::draw_centered_glyph(context.canvas, action_glyph(row_.action), { x + 4.0f * scale, box.y, 14.0f * scale, box.height }, icon_font(context, 12.0f), luil::solid_paint(tone));
                const SkFont pill_font { ui_font(context, 11.0f, true) };
                static_cast<void>(luil::draw_text_within(context.canvas, std::u8string { rebase_action_label(row_.action) }, x + 20.0f * scale,
                    box.y + luil::centered_text_baseline(pill_font, box.height), pill_width - 22.0f * scale, pill_font, luil::solid_paint(tone)));
                x += pill_width + 10.0f * scale;

                const SkFont code { code_font(context, 11.0f) };
                static_cast<void>(luil::draw_text_within(context.canvas, row_.short_sha, x, box.y + luil::centered_text_baseline(code, box.height), 60.0f * scale, code, dim_paint));
                x += 62.0f * scale;

                float right { box.x + box.width - 8.0f * scale };
                const SkFont badge_font { ui_font(context, 10.5f) };
                if (row_.pushed)
                {
                    const std::u8string badge { u8"원격에 있음" };
                    const float width { luil::measure_text(badge, badge_font) };
                    right -= width;
                    static_cast<void>(luil::draw_text_within(
                        context.canvas, badge, right, box.y + luil::centered_text_baseline(badge_font, box.height), width + 2.0f * scale, badge_font, luil::solid_paint(palette.warning_accent)));
                    right -= 10.0f * scale;
                }
                const SkFont font { ui_font(context, 12.0f) };
                const bool dropped { row_.action == rebase_action::drop };
                const SkPaint subject_paint { luil::solid_paint(dropped ? palette.disabled_foreground : palette.primary_foreground) };
                const float drawn { luil::draw_text_within(context.canvas, row_.subject, x, box.y + luil::centered_text_baseline(font, box.height), std::max(right - x, 0.0f), font, subject_paint) };
                // 지우는 줄은 제목에 줄을 긋는다.
                if (dropped && drawn > 0.0f)
                    context.canvas.drawRect(SkRect::MakeXYWH(x, box.y + box.height / 2.0f, drawn, 1.0f * scale), subject_paint);
            }

        private:
            rebase_row_view row_ {};
        };

        // p·r·s·f·d 한 글자다. 목록의 글자 탐색 자리에서 받는다 (Windows는 수식 키 없는 글자를 키가 아니라 문자로만 보낸다).
        std::optional<rebase_action> shortcut_action(const std::u8string_view query) noexcept
        {
            // 질의는 시간 안에 친 글자가 이어 붙은 것이다. 마지막 글자가 이번에 누른 키다.
            if (query.empty() || static_cast<unsigned>(query.back()) >= 0x80u)
                return std::nullopt;
            return rebase_action_for_key(static_cast<char32_t>(query.back()));
        }

        std::unique_ptr<luil::ui_element> build_list(const rebase_editor_view& view)
        {
            auto rows { std::make_shared<std::vector<rebase_row_view>>(view.rows) };
            luil::virtual_list_config config {};
            config.owner = u8"rebase-todo";
            config.selected = view.selected_key;
            config.cursor = view.selected_key;
            config.row_height = row_height;
            config.scroll_offset = view.list_scroll;
            for (const rebase_row_view& row : view.rows)
                config.items.push_back({ .key = row.key, .label = row.label });
            config.build_row
                = [rows](const luil::virtual_list_item&, const std::size_t index, bool) -> std::unique_ptr<luil::ui_element> { return std::make_unique<rebase_row_element>((*rows)[index]); };
            // 커서가 곧 고른 줄이다. 화살표로 옮기면 오른쪽 판도 따라온다.
            config.select = [](const std::u8string& key) { return message(rebase_select_intent { key }); };
            config.move = [](const std::u8string& key) { return message(rebase_select_intent { key }); };
            config.activate = [](const std::u8string& key) { return message(rebase_select_intent { key }); };
            // 끌어서 순서를 바꾼다. 같은 목록의 다른 줄에만 놓는다.
            const luil::ui_element_id list_id { luil::ui_element_kind::virtual_list, u8"rebase-todo" };
            config.drag = [rows](const std::u8string& key, const luil::ui_action_context&) {
                luil::drag_payload payload {};
                payload.dragged_owner = key;
                for (const rebase_row_view& row : *rows)
                    if (row.key == key)
                        payload.label = row.label;
                return payload;
            };
            config.accepts_drop
                = [list_id](const std::u8string& key, const luil::drag_payload& payload) { return payload.files.empty() && payload.container == list_id && payload.carries(list_id, key) == false; };
            config.drop = [](const std::u8string& key, const luil::drag_payload& payload, const luil::ui_action_context&) { return message(rebase_reorder_intent { payload.dragged_owner, key }); };
            config.scroll = [](const float delta) { return message(rebase_scroll_intent { .side = false, .value = delta, .absolute = false }); };
            config.scroll_to = [](const float offset) { return message(rebase_scroll_intent { .side = false, .value = offset, .absolute = true }); };
            config.edges = luil::scroll_edges { .shadows = true };
            auto list { std::make_unique<luil::virtual_list_element>(std::move(config)) };
            list->set_access_name(u8"할 일 목록 · 커밋 " + number_text(view.rows.size()) + u8"개");
            // 목록의 글자 탐색 대신 동작 단축키다. 줄 이름은 번호로 시작하므로 글자로 줄을 찾을 일이 없다.
            luil::key_search_target search {};
            search.on_search = [](const std::u8string_view query, bool) -> std::optional<std::vector<luil::input_action>> {
                const std::optional<rebase_action> action { shortcut_action(query) };
                if (action.has_value() == false)
                    return std::nullopt;
                return std::vector<luil::input_action> { message(rebase_set_action_intent { .row_key = {}, .action = *action }) };
            };
            list->set_key_search_target(std::move(search));
            return list;
        }

        // ---------------------------------------------------------------- 오른쪽 판

        std::unique_ptr<luil::ui_element> build_side_content(const rebase_editor_view& view, float& height)
        {
            measured_column page { u8"rebase-side-page" };
            if (view.has_selection)
            {
                page.add(make_text(u8"rebase-selected-caption", u8"고른 커밋", text_style { .size = 11.0f, .color = dim, .bold = true }), line_height);
                page.add(make_text(u8"rebase-selected", view.selected_title, text_style { .size = 12.5f, .bold = true }), 22.0f);
                page.gap(4.0f);

                luil::choice_group_config choice {};
                choice.owner = u8"rebase-action";
                choice.style = luil::choice_style::toggle;
                choice.name = u8"동작";
                for (const rebase_action action : actions)
                    choice.items.push_back({ action_value(action), action_value(action) });
                choice.selected = action_value(view.selected_action);
                const bool can_meld { view.can_meld };
                choice.select = [can_meld](const std::u8string& value) {
                    const std::optional<rebase_action> action { action_of_value(value) };
                    // 합칠 앞 커밋이 없으면 controller도 거절하지만, 고른 표시가 잠깐 바뀌지 않게 여기서도 거른다.
                    if (action.has_value() == false || (is_meld_action(*action) && can_meld == false))
                        return message(rebase_select_intent {});
                    return message(rebase_set_action_intent { .row_key = {}, .action = *action });
                };
                auto row { make_row(u8"rebase-action-row", 0.0f) };
                row->add(make_segmented_control(std::move(choice)), { .length = 0.0f, .weight = 1.0f, .cross_length = segmented_control_height });
                page.add(std::move(row), input_height);
                page.add(make_text(u8"rebase-action-keys", u8"키: p pick · r reword · s squash · f fixup · d drop · Alt+↑↓ 옮기기", text_style { .size = 10.5f, .color = dim }), line_height);
                // 끌기와 Alt+↑↓의 자리를 단추로도 둔다. 마우스 없이, 보조 기술로도 순서를 바꾼다.
                auto move_row { make_row(u8"rebase-move-row", 6.0f) };
                move_row->add(make_text_button(u8"rebase-move-up", u8"위로", luil::text_button_visual::normal, message(rebase_move_intent { .row_key = {}, .delta = -1 }), view.can_move_up,
                                  codicons::icon_arrow_up),
                    { .length = 76.0f, .cross_length = 24.0f });
                move_row->add(make_text_button(u8"rebase-move-down", u8"아래로", luil::text_button_visual::normal, message(rebase_move_intent { .row_key = {}, .delta = 1 }), view.can_move_down,
                                  codicons::icon_arrow_down),
                    { .length = 86.0f, .cross_length = 24.0f });
                move_row->add_flexible_gap();
                page.add(std::move(move_row), 28.0f);
                page.gap(10.0f);

                if (view.message_editable)
                {
                    page.add(make_text(u8"rebase-message-caption", view.message_caption, text_style { .size = 11.0f, .color = dim, .bold = true }), line_height);
                    page.add(std::make_unique<luil::text_input_element>(
                                 luil::ui_element_id { kind_rebase_subject_input, u8"rebase-subject" }, view.subject, luil::text_input_config { .placeholder = u8"제목 (첫 줄)" }),
                        input_height);
                    page.gap(6.0f);
                    page.add(
                        std::make_unique<multiline_text_element>(luil::ui_element_id { kind_rebase_body_input, u8"rebase-body" }, view.body, u8"본문 (Enter로 줄바꿈)", u8"메시지 본문"), body_height);
                    page.gap(4.0f);
                    auto actions_row { make_row(u8"rebase-message-actions") };
                    actions_row->add_flexible(make_text(u8"rebase-message-note", view.message_note, text_style { .size = 10.5f, .color = dim }));
                    actions_row->add(make_text_button(u8"rebase-message-reset", u8"되돌리기", luil::text_button_visual::normal, message(rebase_reset_message_intent {}), view.message_edited),
                        { .length = 72.0f, .cross_length = 22.0f });
                    page.add(std::move(actions_row), 26.0f);
                }
                else
                    page.add(make_text(u8"rebase-message-note", view.message_note, text_style { .size = 11.0f, .color = dim }), line_height);
                page.gap(12.0f);
            }

            page.add(make_text(u8"rebase-preview-caption", view.preview_caption, text_style { .size = 11.0f, .color = dim, .bold = true }), line_height);
            std::size_t index { 0 };
            for (const rebase_preview_view& result : view.preview)
            {
                auto row { make_row(result.key + u8"-row", 6.0f) };
                const color_selector dot { result.changed ? color_selector { [](const luil::ui_color_palette& palette) { return palette.accent; } } : dim };
                row->add(std::make_unique<icon_element>(luil::ui_element_id { kind_label, result.key + u8"-dot" }, codicons::icon_git_commit, dot, 13.0f), 16.0f);
                row->add_flexible(make_text(result.key, number_text(++index) + u8". " + result.subject, text_style { .size = 11.5f }));
                if (result.detail.empty() == false)
                    row->add(make_text(result.key + u8"-detail", result.detail, text_style { .size = 10.5f, .color = dim, .align = 2 }), 130.0f);
                page.add(std::move(row), preview_row_height);
            }

            height = page.height() + padding;
            const float content_height { page.height() };
            auto padded { make_column(u8"rebase-side-padding", luil::stack_config { .padding = luil::edge_insets { padding, 10.0f, padding, 0.0f } }) };
            padded->add(page.take(), content_height);
            return padded;
        }

        std::unique_ptr<luil::ui_element> scroll_side(const rebase_editor_view& view, std::unique_ptr<luil::ui_element> content, const float content_height, const float viewport_height)
        {
            const float maximum { std::max(content_height - viewport_height, 0.0f) };
            const float offset { std::clamp(view.side_scroll, 0.0f, maximum) };
            luil::scroll_area_config scroll {};
            scroll.owner = u8"rebase-side";
            scroll.content_height = content_height;
            scroll.scroll_offset = offset;
            scroll.scroll = [offset, maximum](const float delta) { return message(rebase_scroll_intent { .side = true, .value = std::clamp(offset + delta, 0.0f, maximum), .absolute = true }); };
            scroll.scroll_to = [](const float value) { return message(rebase_scroll_intent { .side = true, .value = value, .absolute = true }); };
            scroll.edges = luil::scroll_edges { .shadows = true };
            auto area { std::make_unique<luil::scroll_area_element>(std::move(scroll)) };
            area->set_content(std::move(content));
            return area;
        }

        // ---------------------------------------------------------------- 아래 막대

        bool separate_footer_options(const rebase_editor_view& view, const float width)
        {
            return width < stacked_width && view.phase == rebase_editor_phase::editing && (view.show_autosquash || view.show_autostash);
        }

        std::unique_ptr<luil::ui_element> build_footer(const rebase_editor_view& view, const float width)
        {
            auto row { make_row(u8"rebase-footer") };
            const bool separate { separate_footer_options(view, width) };
            auto options { make_row(u8"rebase-footer-options") };
            luil::stack_element& option_row { separate ? *options : *row };
            const bool editing { view.phase == rebase_editor_phase::editing };
            if (editing && view.show_autosquash)
            {
                luil::check_config check { .owner = u8"rebase-autosquash", .label = u8"fixup!·squash! 정렬", .checked = view.autosquash };
                check.toggle = action_of(message(rebase_toggle_intent { rebase_option::autosquash }));
                check.tooltip = u8"`fixup! 제목` 커밋을 대상 커밋 바로 뒤로 옮기고 fixup·squash로 둡니다 (git --autosquash와 같은 규칙).";
                option_row.add(std::make_unique<luil::check_element>(std::move(check)), 176.0f);
            }
            if (editing && view.show_autostash)
            {
                luil::check_config check { .owner = u8"rebase-autostash", .label = u8"작업 변경 자동 보관", .checked = view.autostash };
                check.toggle = action_of(message(rebase_toggle_intent { rebase_option::autostash }));
                check.tooltip = u8"작업 트리 변경을 임시 보관하고 rebase 뒤에 다시 적용합니다 (--autostash).";
                option_row.add(std::make_unique<luil::check_element>(std::move(check)), 196.0f);
            }
            auto status { make_column(u8"rebase-status-column", luil::stack_config { .spacing = 0.0f }) };
            status->add(make_text(u8"rebase-summary", view.summary, text_style { .size = 12.0f, .bold = true, .align = 2 }), 18.0f);
            const bool problem { view.problem.empty() == false };
            const std::u8string note { problem ? view.problem : view.status_text };
            auto note_label { make_text(u8"rebase-problem", note, text_style { .size = 11.0f, .color = problem ? (view.can_start ? warning_color : error_color) : dim, .align = 2 }) };
            note_label->set_tooltip(note);
            status->add(std::move(note_label), 16.0f);
            row->add_flexible(std::move(status));
            row->add(make_text_button(u8"rebase-cancel", editing ? u8"취소" : u8"닫기", luil::text_button_visual::normal, message(close_rebase_editor_intent {})),
                { .length = 72.0f, .cross_length = button_height });
            if (view.phase == rebase_editor_phase::editing || view.phase == rebase_editor_phase::running)
                row->add(make_text_button(u8"rebase-start", view.phase == rebase_editor_phase::running ? u8"실행 중…" : u8"rebase 시작", luil::text_button_visual::accent,
                             message(rebase_start_intent {}), view.can_start, codicons::icon_play),
                    { .length = 118.0f, .cross_length = button_height });
            luil::stack_config config { .spacing = separate ? gap : 0.0f, .padding = luil::edge_insets { padding, 10.0f, padding, 10.0f } };
            auto padded { make_column(u8"rebase-footer-padding", config) };
            if (separate)
                padded->add(std::move(options), luil::check_row_height);
            padded->add_flexible(std::move(row));
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"rebase-footer-bar" }, bar_style { .divider_top = true }, std::move(padded));
        }
    } // namespace

    std::unique_ptr<luil::ui_element> build_rebase_editor(const rebase_editor_view& view, const float logical_width, const float logical_height)
    {
        const float width { std::clamp(logical_width - dialog_margin * 2.0f, 0.0f, dialog_maximum_width) };
        const float height { std::clamp(logical_height - dialog_margin * 2.0f, 0.0f, dialog_maximum_height) };
        const float footer { separate_footer_options(view, width) ? compact_footer_height : footer_height };

        luil::modal_host_config config {};
        config.owner = u8"rebase-editor";
        config.name = u8"인터랙티브 rebase";
        config.dismiss = action_of(message(close_rebase_editor_intent {}));
        config.focus_entry = luil::ui_element_id { luil::ui_element_kind::virtual_list, u8"rebase-todo" };
        config.surface = floating_panel_config(8.0f, 0.4f);
        config.content_width = width;
        config.content_height = height;
        auto host { std::make_unique<luil::modal_host_element>(std::move(config)) };

        auto column { make_column(u8"rebase-column") };
        luil::dialog_caption_config caption { .owner = u8"rebase", .title = view.title, .close = action_of(message(close_rebase_editor_intent {})), .close_tooltip = u8"닫기" };
        const float caption_height { luil::dialog_caption_element::height_for(caption) };
        column->add(std::make_unique<luil::dialog_caption_element>(std::move(caption)), caption_height);

        if (view.phase == rebase_editor_phase::loading || view.phase == rebase_editor_phase::load_failed)
        {
            auto body { make_column(u8"rebase-waiting", luil::stack_config { .padding = luil::edge_insets::all(padding) }) };
            body->add_flexible_gap();
            const bool failed { view.phase == rebase_editor_phase::load_failed };
            body->add(make_text(u8"rebase-waiting-text", view.status_text, text_style { .size = 12.5f, .color = failed ? error_color : dim, .align = 1 }), 24.0f);
            body->add_flexible_gap();
            column->add_flexible(std::move(body));
            column->add(build_footer(view, width), footer);
            host->set_content(std::move(column));
            return host;
        }

        // ---- 머리: 범위와 경고
        measured_column header { u8"rebase-header" };
        auto subtitle { make_text(u8"rebase-subtitle", view.subtitle, text_style { .size = 12.0f }) };
        subtitle->set_tooltip(view.subtitle);
        header.add(std::move(subtitle), line_height + 2.0f);
        std::size_t warning_index { 0 };
        for (const std::u8string& warning : view.warnings)
            header.add(warning_line(u8"rebase-warning-" + number_text(warning_index++), warning, warning_color), warning_height);
        if (view.needs_flatten_confirm)
        {
            luil::check_config check { .owner = u8"rebase-flatten", .label = u8"병합 커밋을 빼고 한 줄로 펴도 됩니다", .checked = view.flatten_confirmed };
            check.toggle = action_of(message(rebase_toggle_intent { rebase_option::flatten_merges }));
            header.add(std::make_unique<luil::check_element>(std::move(check)), luil::check_row_height);
        }
        const float header_height { header.height() };
        auto header_padded { make_column(u8"rebase-header-padding", luil::stack_config { .padding = luil::edge_insets { padding, 8.0f, padding, 8.0f } }) };
        header_padded->add(header.take(), header_height);
        column->add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"rebase-header-bar" }, bar_style { .divider_bottom = true }, std::move(header_padded)), header_height + 16.0f);

        // ---- 몸통: 목록 | 고른 줄·미리보기
        auto body { make_row(u8"rebase-body-row", 0.0f) };

        auto left { make_column(u8"rebase-left", luil::stack_config { .spacing = 4.0f, .padding = luil::edge_insets { 8.0f, 8.0f, 8.0f, 8.0f } }) };
        left->add(make_text(u8"rebase-list-hint", u8"위가 먼저 적용됩니다 · 끌어서 순서 바꾸기", text_style { .size = 10.5f, .color = dim }), 16.0f);
        left->add_flexible(build_list(view));
        float side_height { 0.0f };
        auto side { build_side_content(view, side_height) };
        const float viewport { std::max(height - caption_height - header_height - 16.0f - footer, 0.0f) };
        if (width < stacked_width)
        {
            // 좁은 판은 전체 폭의 목록과 편집기를 위아래로 놓고, 본문만 흘린다.
            auto stacked { make_column(u8"rebase-body-stacked") };
            stacked->add(std::move(left), stacked_list_height);
            stacked->add(std::move(side), side_height);
            column->add_flexible(scroll_side(view, std::move(stacked), stacked_list_height + side_height, viewport));
        }
        else
        {
            body->add_flexible(std::move(left));
            body->add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"rebase-side-bar" },
                          bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.surface_background; } }, scroll_side(view, std::move(side), side_height, viewport)),
                side_width);
            column->add_flexible(std::move(body));
        }
        column->add(build_footer(view, width), footer);
        host->set_content(std::move(column));
        return host;
    }
} // namespace cyella::ui
