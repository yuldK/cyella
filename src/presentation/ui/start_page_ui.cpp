#include "presentation/ui/app_symbol.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/list_rows.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui/ui_parts.h"
#include "presentation/ui_intents.h"

#include "domain/repository_path.h"
#include "domain/text_catalog.h"
#include "presentation/build_view_snapshot.h"
#include "presentation/settings/settings_intents.h"

#include "luil/ui/dialog_elements.h"
#include "luil/ui/scroll_area_element.h"
#include "luil/ui/stack_element.h"

#include <algorithm>
#include <chrono>

namespace cyella::ui {
    namespace {
        // 시작 페이지 본문의 최대 폭이다 (논리 픽셀). 넓은 창에서는 가운데에 선다.
        constexpr float start_content_width { 880.0f };
        constexpr float start_side_padding { 24.0f };
        constexpr float start_row_spacing { 6.0f };
        // 머리말 왼쪽 앱 심볼의 한 변이다 (논리 픽셀). 심볼 그림은 이 크기의 4배까지 흐려지지 않는다.
        constexpr float start_symbol_size { 40.0f };
        // 타이틀 그림이 있을 때의 머리말이다: 타이틀 높이와, 그 곁에서 가로 로고처럼 비슷한 키로 서는 심볼의 한 변이다 (논리 픽셀).
        constexpr float start_title_height { 64.0f };
        constexpr float start_titled_symbol_size { 72.0f };

        // `2026-10-20T09:30:12Z`를 유닉스 초로 읽는다. 읽지 못하면 0이다.
        std::int64_t parse_utc(const std::u8string_view text)
        {
            if (text.size() < 20)
                return 0;
            const auto digits = [&text](const std::size_t at, const std::size_t count) {
                int value { 0 };
                for (std::size_t index { at }; index < at + count; ++index)
                {
                    if (text[index] < u8'0' || text[index] > u8'9')
                        return -1;
                    value = value * 10 + (text[index] - u8'0');
                }
                return value;
            };
            const int year { digits(0, 4) };
            const int month { digits(5, 2) };
            const int day { digits(8, 2) };
            const int hour { digits(11, 2) };
            const int minute { digits(14, 2) };
            const int second { digits(17, 2) };
            if (year < 1970 || month < 1 || day < 1 || hour < 0 || minute < 0 || second < 0)
                return 0;
            const std::chrono::sys_days date { std::chrono::year { year } / std::chrono::month { static_cast<unsigned>(month) } / std::chrono::day { static_cast<unsigned>(day) } };
            return date.time_since_epoch().count() * 86400 + hour * 3600 + minute * 60 + second;
        }

        std::unique_ptr<luil::ui_element> make_start_row(const start_page_item& item, const bool open, const std::int64_t now)
        {
            std::vector<file_row_button> buttons {};
            const char32_t pin_glyph { item.pinned ? codicons::icon_pinned : codicons::icon_pin };
            buttons.push_back(make_row_button(u8"pin:" + item.path, pin_glyph, item.pinned ? tr(u8"start.unpin") : tr(u8"start.pin"), message(pin_recent_intent { item.path, item.pinned == false })));
            buttons.push_back({ .owner = u8"remove:" + item.path, .glyph = codicons::icon_close, .tooltip = tr(u8"start.remove_from_list"), .action = message(remove_recent_intent { item.path }) });
            const std::int64_t opened { parse_utc(item.opened_at) };
            std::u8string when { opened > 0 ? relative_time_text(now, opened) : std::u8string {} };
            // 없는 경로는 새 위치를 고르는 폴더 대화 상자를 바로 연다 (옛 경로가 명령 인자다).
            if (item.exists == false)
                buttons.insert(buttons.begin(),
                    make_row_button(
                        u8"locate:" + item.path, codicons::icon_folder_opened, tr(u8"start.locate_tooltip"), ui_command(static_cast<std::uint32_t>(ui_command_id::locate_folder), item.path)));
            luil::input_action activate = item.exists ? message(open_recent_intent { item.path }) : ui_command(static_cast<std::uint32_t>(ui_command_id::locate_folder), item.path);
            return std::make_unique<start_row_element>(u8"recent:" + item.path, item, open, std::move(when), std::move(activate), std::move(buttons));
        }
    } // namespace

    std::unique_ptr<luil::ui_element> build_start_page(const view_snapshot& view, const start_page_snapshot& start, feedback_sources& sources)
    {
        const float side { std::max(start_side_padding, (view.logical_width - start_content_width) / 2.0f) };
        const color_selector dim { [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } };
        const std::int64_t now { view.now };

        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"start-page" }, luil::stack_config { .padding = luil::edge_insets { side, 24.0f, side, 24.0f } }) };
        float height { 48.0f };
        const auto add = [&column, &height](std::unique_ptr<luil::ui_element> child, const float length) {
            height += length;
            column->add(std::move(child), length);
        };
        const auto gap = [&column, &height](const float length) {
            height += length;
            column->add_gap(length);
        };

        // 머리말이다. 앱 심볼이 있으면 제목 묶음 왼쪽에 세운다.
        auto heading { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"start-heading" }, luil::stack_config {}) };
        float heading_height { 0.0f };
        const auto add_heading = [&heading, &heading_height](std::unique_ptr<luil::ui_element> child, const float length) {
            heading_height += length;
            heading->add(std::move(child), length);
        };
        const bool titled { app_title_available() };
        if (titled)
        {
            // 타이틀 그림이 "시작" 글자 제목을 대신한다. 폭은 그림의 비율로 정하고 왼쪽에 붙인다.
            auto title { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"start-title-row" }, luil::stack_config { .direction = luil::stack_direction::row }) };
            title->add(make_app_title(u8"start-title", u8"cyella"), luil::stack_item { .length = start_title_height * app_title_aspect(), .cross_length = start_title_height });
            title->add_flexible_gap();
            add_heading(std::move(title), start_title_height);
        }
        else
            add_heading(make_text(u8"start-title", tr(u8"start.title"), text_style { .size = 20.0f, .bold = true }), 30.0f);
        add_heading(make_text(u8"start-subtitle", tr(u8"start.subtitle"), text_style { .size = 12.0f, .color = dim }), 20.0f);
        if (start.git_message.empty() == false)
            add_heading(make_text(u8"start-git", start.git_message, text_style { .size = 12.0f, .color = [](const luil::ui_color_palette& palette) { return palette.error_accent; } }), 20.0f);
        if (app_symbol().valid())
        {
            luil::stack_config row_config { .direction = luil::stack_direction::row, .spacing = 16.0f };
            row_config.cross_alignment = luil::stack_alignment::center;
            auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"start-heading-row" }, row_config) };
            const float symbol_size { titled ? start_titled_symbol_size : start_symbol_size };
            row->add(make_app_symbol(u8"start-symbol"), luil::stack_item { .length = symbol_size, .cross_length = symbol_size });
            row->add(std::move(heading), luil::stack_item { .length = 0.0f, .weight = 1.0f, .cross_length = heading_height });
            add(std::move(row), std::max(heading_height, symbol_size));
        }
        else
            add(std::move(heading), heading_height);
        gap(14.0f);

        const bool git_usable { start.git_message.empty() };
        const bool stacked { view.logical_width < 760.0f };
        const float card_height { stacked ? 60.0f : 72.0f };
        const luil::stack_config actions_config { .direction = stacked ? luil::stack_direction::column : luil::stack_direction::row, .spacing = 10.0f };
        auto actions { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"start-actions" }, actions_config) };
        const auto add_action = [&](std::unique_ptr<luil::ui_element> card) {
            if (stacked)
                actions->add(std::move(card), card_height);
            else
                actions->add_flexible(std::move(card));
        };
        add_action(std::make_unique<start_card_element>(
            u8"start-open", codicons::icon_folder_opened, tr(u8"start.open_title"), tr(u8"start.open_description"), ui_command(static_cast<std::uint32_t>(ui_command_id::open_folder)), git_usable));
        add_action(std::make_unique<start_card_element>(u8"start-clone", codicons::icon_repo_clone, u8"Clone", tr(u8"start.clone_description"), message(open_clone_dialog_intent {}), git_usable));
        add_action(std::make_unique<start_card_element>(
            u8"start-init", codicons::icon_add, u8"Init", tr(u8"start.init_description"), ui_command(static_cast<std::uint32_t>(ui_command_id::init_folder)), git_usable));
        add(std::move(actions), stacked ? card_height * 3.0f + 20.0f : card_height);
        if (git_usable == false)
        {
            gap(8.0f);
            add(make_text_button(u8"start-git-settings", tr(u8"start.open_git_settings"), luil::text_button_visual::normal, message(open_settings_intent {}), true, codicons::icon_settings_gear),
                30.0f);
        }
        if (start.setup_progress.empty() == false)
        {
            gap(8.0f);
            add(make_text(u8"start-setup-progress", start.setup_progress, text_style { .size = 11.0f, .color = [](const luil::ui_color_palette& palette) { return palette.accent; } }), 18.0f);
        }
        gap(28.0f);

        auto search_row
            = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"start-search-row" }, luil::stack_config { .direction = luil::stack_direction::row, .spacing = 8.0f });
        luil::text_input_config search_config {
            .placeholder = tr(u8"start.search_placeholder"),
            .leading_glyph = codicons::icon_search,
            .clear = action_of(message(edit_text_intent { luil::text_edit_request { .target = to_luil_target(text_target::start_search), .command = luil::text::text_edit_command::clear } })),
            .clear_tooltip = tr(u8"start.search_clear"),
        };
        search_row->add_flexible(std::make_unique<luil::text_input_element>(luil::ui_element_id { kind_start_search_input, u8"start-search" }, start.search, std::move(search_config)));
        search_row->add(make_text(u8"start-count", tr(u8"start.result_count", { { u8"count", start.list.items.size() } }), text_style { .size = 11.0f, .color = dim, .align = 2 }), 40.0f);
        add(std::move(search_row), 28.0f);
        gap(12.0f);

        std::size_t pinned { 0 };
        for (const start_page_item& item : start.list.items)
            pinned += item.pinned ? 1u : 0u;
        const auto add_section = [&](const std::u8string& owner, const std::u8string& title, const bool pinned_section) {
            const std::size_t count { pinned_section ? pinned : start.list.items.size() - pinned };
            if (count == 0)
                return;
            add(make_text(owner, title + u8"  " + number_text(count), text_style { .size = 11.0f, .color = dim, .bold = true }), 24.0f);
            for (const start_page_item& item : start.list.items)
            {
                if (item.pinned != pinned_section)
                    continue;
                add(make_start_row(item, start.open_keys.contains(repository_path_key(item.path)), now), start_row_height);
                gap(start_row_spacing);
            }
            gap(10.0f);
        };
        add_section(u8"start-pinned", tr(u8"start.section_pinned"), true);
        add_section(u8"start-recent", tr(u8"start.section_recent"), false);
        if (start.list.items.empty())
            add(make_text(u8"start-empty", start.list.total_count == 0 ? tr(u8"start.empty_no_recent") : tr(u8"start.empty_no_match"), text_style { .size = 12.0f, .color = dim }), 24.0f);

        luil::scroll_area_config config {};
        config.edges.shadows = true;
        config.owner = u8"start-scroll";
        config.content_height = height;
        config.scroll_offset = start.scroll;
        config.scroll = [](const float delta) { return message(scroll_intent { .target = scroll_target::start_page, .value = delta }); };
        config.scroll_to = [](const float offset) { return message(scroll_intent { .target = scroll_target::start_page, .value = offset, .absolute = true }); };
        auto area { std::make_unique<luil::scroll_area_element>(std::move(config)) };
        area->set_content(std::move(column));
        sources.start = area.get();
        return area;
    }
} // namespace cyella::ui
