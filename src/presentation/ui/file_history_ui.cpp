#include "presentation/ui/file_history_ui.h"

#include "domain/text_catalog.h"
#include "presentation/graph_geometry.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui/ui_parts.h"
#include "presentation/ui_intents.h"

#include "luil/ui/stack_element.h"
#include "luil/ui/virtual_list_element.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace cyella::ui {
    namespace {
        // 한 행은 두 줄이다: 위에 제목과 날짜, 아래에 짧은 SHA·작성자·이름 변경이다.
        constexpr float history_row_height { 44.0f };
        constexpr float history_row_padding { 10.0f };
        constexpr float history_status_width { 18.0f };
        constexpr float history_date_width { 84.0f };
        constexpr float history_first_line { 15.0f };
        constexpr float history_second_line { 31.0f };

        const color_selector dim_color { [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } };

        std::u8string short_sha(const std::string& sha)
        {
            return to_u8(std::string_view { sha }.substr(0, std::min<std::size_t>(sha.size(), 7)));
        }

        // 이 커밋이 파일에 한 일을 한 글자로 말한다. 보조 기술과 툴팁에는 낱말로 간다.
        std::u8string status_word(const char status)
        {
            switch (status)
            {
            case 'A':
                return tr(u8"file_history.status_added");
            case 'D':
                return tr(u8"file_history.status_deleted");
            case 'R':
                return tr(u8"file_history.status_renamed");
            case 'C':
                return tr(u8"file_history.status_copied");
            default:
                return tr(u8"file_history.status_modified");
            }
        }

        // 파일 이력 목록의 한 줄이다. 고르기는 목록이 맡으므로 그리기만 한다 (hit를 받지 않는다).
        class file_history_row_element final : public luil::ui_element
        {
        public:
            file_history_row_element(std::u8string owner, file_history_row_view row, const bool selected)
                : ui_element { luil::ui_element_id { kind_file_history_row, std::move(owner) } }
                , row_ { std::move(row) }
                , selected_ { selected }
            {
                set_hit_opaque(false);
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot&) const override
            {
                const luil::rect_f box { bounds() };
                const float scale { context.scale };
                const luil::ui_color_palette& palette { context.palette };
                if (selected_)
                    context.canvas.drawRect(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), luil::solid_paint(palette.row_selection_background));

                const SkFont title_font { ui_font(context, 12.5f) };
                const SkFont meta_font { ui_font(context, 11.0f) };
                const SkFont sha_font { code_font(context, 11.0f) };
                const SkPaint primary { luil::solid_paint(palette.primary_foreground) };
                const SkPaint dim { luil::solid_paint(palette.secondary_foreground) };

                float x { box.x + history_row_padding * scale };
                const float right { box.x + box.width - history_row_padding * scale };
                // 추가·삭제·이름 변경은 눈에 띄어야 이력의 마디가 보인다. 수정은 흐리게 둔다.
                luil::ui_color status_color { palette.secondary_foreground };
                if (row_.status == 'D')
                    status_color = palette.error_accent;
                else if (row_.status != 'M')
                    status_color = palette.accent;
                // 고른 행의 바탕은 강조색 계열이라 강조색 글자가 묻힌다.
                if (selected_ && row_.status != 'D')
                    status_color = palette.primary_foreground;
                const std::u8string status { static_cast<char8_t>(row_.status) };
                static_cast<void>(luil::draw_text_within(context.canvas, status, x, box.y + history_first_line * scale + luil::centered_text_baseline(title_font, 0.0f), history_status_width * scale,
                    title_font, luil::solid_paint(status_color)));
                x += history_status_width * scale;

                const float date_width { std::min(history_date_width * scale, std::max(0.0f, right - x) * 0.4f) };
                const float date_text_width { std::min(luil::measure_text(row_.date, meta_font), date_width) };
                static_cast<void>(luil::draw_text_within(context.canvas, row_.date, right - date_text_width, box.y + history_first_line * scale + luil::centered_text_baseline(meta_font, 0.0f), date_width,
                    meta_font, dim));
                static_cast<void>(luil::draw_text_within(context.canvas, row_.subject, x, box.y + history_first_line * scale + luil::centered_text_baseline(title_font, 0.0f),
                    std::max(0.0f, right - date_width - 8.0f * scale - x), title_font, primary));

                const float second { box.y + history_second_line * scale };
                const std::u8string sha { short_sha(row_.sha) };
                static_cast<void>(luil::draw_text_within(context.canvas, sha, x, second + luil::centered_text_baseline(sha_font, 0.0f), std::max(0.0f, right - x), sha_font, dim));
                const float after_sha { x + luil::measure_text(sha, sha_font) + 8.0f * scale };
                std::u8string tail { row_.author };
                if (row_.rename_note.empty() == false)
                    tail += u8"  ·  " + row_.rename_note;
                static_cast<void>(luil::draw_text_within(context.canvas, tail, after_sha, second + luil::centered_text_baseline(meta_font, 0.0f), std::max(0.0f, right - after_sha), meta_font, dim));
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                std::u8string name { short_sha(row_.sha) + u8" " + row_.subject + u8", " + row_.author + u8", " + row_.date_absolute + u8", " + status_word(row_.status) };
                if (row_.rename_note.empty() == false)
                    name += u8", " + row_.rename_note;
                return { .role = luil::access_role::static_text, .name = std::move(name) };
            }

        private:
            file_history_row_view row_ {};
            bool selected_ { false };
        };

        std::unique_ptr<luil::ui_element> history_note(std::u8string owner, std::u8string text)
        {
            auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, owner + u8"-column" }, luil::stack_config { .padding = luil::edge_insets::all(16.0f) }) };
            column->add_flexible_gap();
            column->add(make_text(std::move(owner), std::move(text), text_style { .size = 12.0f, .color = dim_color, .align = 1 }), 24.0f);
            column->add_flexible_gap();
            return column;
        }
    } // namespace

    std::unique_ptr<luil::ui_element> build_file_history_list(const file_history_view& history, feedback_sources& sources)
    {
        if (history.note.empty() == false)
            return history_note(u8"file-history-note", history.note);

        auto rows { std::make_shared<std::vector<file_history_row_view>>(history.rows) };
        luil::virtual_list_config config {};
        config.edges.shadows = true;
        config.owner = u8"file-history";
        config.items.reserve(rows->size());
        for (const file_history_row_view& row : *rows)
            config.items.push_back({ .key = graph_item_key(row.sha), .label = row.subject });
        config.selected = history.selected.empty() ? std::u8string {} : graph_item_key(history.selected);
        config.cursor = config.selected;
        config.row_height = history_row_height;
        config.scroll_offset = history.scroll;
        config.overscan = 4;
        config.build_row = [rows](const luil::virtual_list_item& item, const std::size_t index, const bool selected) -> std::unique_ptr<luil::ui_element> {
            if (index >= rows->size())
                return nullptr;
            // 행에 툴팁을 달지 않는다. 달면 포인터의 hit를 행이 받아 누름이 목록에 닿지 않는다 (마우스로 고를 수 없게 된다).
            return std::make_unique<file_history_row_element>(u8"history-" + item.key, (*rows)[index], selected);
        };
        // 화살표로 옮기는 것이 곧 고르기다. 고르면 판 오른쪽이 그 커밋에서의 변경 내용으로 바뀐다.
        config.select = [](const std::u8string& key) { return message(select_file_history_intent { key }); };
        config.scroll = [](const float delta) { return message(scroll_intent { .target = scroll_target::file_history, .value = delta }); };
        config.scroll_to = [](const float offset) { return message(scroll_intent { .target = scroll_target::file_history, .value = offset, .absolute = true }); };
        auto list { std::make_unique<luil::virtual_list_element>(std::move(config)) };
        const std::size_t count { rows->size() };
        list->set_access_name(count == 1 ? tr(u8"file_history.access_name_one", { { u8"count", count } }) : tr(u8"file_history.access_name", { { u8"count", count } }));
        sources.file_history = list.get();
        if (history.truncated == false)
            return list;

        // 읽은 것보다 이력이 더 있다. 목록 아래에 한 줄로 알린다.
        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"file-history-column" }, luil::stack_config {}) };
        column->add_flexible(std::move(list));
        column->add(make_text(u8"file-history-truncated", tr(u8"file_history.truncated", { { u8"count", count } }), text_style { .size = 11.0f, .color = dim_color, .align = 1 }), 24.0f);
        return column;
    }
} // namespace cyella::ui
