#include "presentation/ui/build_ui_tree.h"

#include "application/app_messages.h"
#include "presentation/dialog_intents.h"
#include "presentation/palette/palette_intents.h"
#include "presentation/selector_intents.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/ui/app_symbol.h"
#include "presentation/ui/command_elements.h"
#include "presentation/ui/cyella_caption.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/list_rows.h"
#include "presentation/ui/palette_ui.h"
#include "presentation/ui/rebase/rebase_editor_ui.h"
#include "presentation/ui/ref_chip_element.h"
#include "presentation/ui/settings/settings_ui.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui/ui_parts.h"
#include "presentation/ui_intents.h"

#include "luil/ui/caption_element.h"
#include "luil/ui/dialog_elements.h"
#include "luil/ui/modal_host_element.h"
#include "luil/ui/panel_element.h"
#include "luil/ui/progress_element.h"
#include "luil/ui/split_handle_element.h"
#include "luil/ui/stack_element.h"
#include "luil/ui/tab_bar_element.h"
#include "luil/ui/ui_platform.h"
#include "luil/ui/virtual_list_element.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"

#include <algorithm>
#include <charconv>
#include <string>
#include <utility>

namespace cyella::ui {
    namespace {
        // 배너가 펼쳤을 때 보이는 [자세히] 줄 수와 한 줄 높이다 (논리 픽셀).
        constexpr std::size_t banner_visible_lines { 8 };
        constexpr float banner_line_height { 18.0f };
        constexpr float tab_width { 180.0f };
        constexpr float tab_row_button_width { 32.0f };
        constexpr float tool_button_width { 52.0f };
        constexpr float tool_button_compact_width { 36.0f };
        constexpr float clone_dialog_width { 480.0f };
        constexpr float clone_dialog_height { 330.0f };
        constexpr float log_header_height { 26.0f };

        std::u8string tab_key(const repository_tab_id id)
        {
            return u8"tab-" + number_text(id);
        }

        repository_tab_id tab_id_of(const std::u8string& key)
        {
            const std::string text { to_text(key) };
            repository_tab_id id { 0 };
            if (text.size() > 4)
                static_cast<void>(std::from_chars(text.data() + 4, text.data() + text.size(), id));
            return id;
        }

        char32_t tab_glyph(const tab_view& tab) noexcept
        {
            switch (tab.marker)
            {
            case tab_marker::needs_input:
                return codicons::icon_bell;
            case tab_marker::running:
                return codicons::icon_sync;
            case tab_marker::queued:
                return codicons::icon_watch;
            case tab_marker::changes:
                return codicons::icon_circle_filled;
            case tab_marker::none:
                break;
            }
            return tab.start ? codicons::icon_home : codicons::icon_repo;
        }

        std::u8string marker_text(const tab_marker marker)
        {
            switch (marker)
            {
            case tab_marker::needs_input:
                return u8" · 입력 필요";
            case tab_marker::running:
                return u8" · 진행 중";
            case tab_marker::queued:
                return u8" · 대기 중";
            case tab_marker::changes:
                return u8" · 변경 있음";
            case tab_marker::none:
                break;
            }
            return {};
        }

        std::u8string theme_tooltip(const theme_preference theme)
        {
            switch (theme)
            {
            case theme_preference::system:
                return u8"테마: 시스템 (누르면 밝게)";
            case theme_preference::light:
                return u8"테마: 밝게 (누르면 어둡게)";
            case theme_preference::dark:
                return u8"테마: 어둡게 (누르면 시스템)";
            }
            return u8"테마";
        }

        // 팔레트 단추의 툴팁이다. 지금 단축키(사용자가 바꿨을 수 있다)를 말한다.
        std::u8string palette_tooltip(const view_snapshot& view)
        {
            std::vector<key_chord> chords {};
            for (const shortcut_binding& binding : view.shortcuts)
                if (binding.command == "palette.open")
                    chords.push_back(binding.chord);
            const std::u8string keys { format_key_chord_list(chords) };
            return keys.empty() ? std::u8string { u8"명령 팔레트" } : u8"명령 팔레트 (" + keys + u8")";
        }

        // ---------------------------------------------------------------- 탭 줄

        std::unique_ptr<luil::ui_element> build_tab_row(const view_snapshot& view)
        {
            luil::tab_bar_config config {};
            config.owner = u8"tabs";
            config.selected = tab_key(view.selected_tab);
            config.tab_width = tab_width;
            config.scroll_offset = view.tab_scroll;
            const bool single_start { view.tabs.size() == 1 && view.tabs.front().start };
            for (const tab_view& tab : view.tabs)
            {
                // 탭 표시는 아이콘 자리에 하나만 선다. 이름 뒤 꼬리표는 rebase·merge 중일 때만이다.
                std::u8string label { tab.label };
                if (tab.in_progress.empty() == false)
                    label += u8" · " + tab.in_progress;
                config.items.push_back({ .key = tab_key(tab.id), .label = std::move(label), .icon = tab_glyph(tab), .closable = single_start == false });
            }
            config.select = [](const std::u8string& key) { return message(select_tab_intent { tab_id_of(key) }); };
            config.close = [](const std::u8string& key) { return message(close_tab_intent { tab_id_of(key) }); };
            config.reorder = [](const std::u8string& moved, const std::u8string& target) { return message(reorder_tab_intent { tab_id_of(moved), tab_id_of(target) }); };
            config.scroll = [](const float delta) { return message(scroll_tabs_intent { delta }); };
            auto tab_bar { std::make_unique<luil::tab_bar_element>(std::move(config)) };
            std::u8string names {};
            for (const tab_view& tab : view.tabs)
                names += (names.empty() ? u8"" : u8", ") + tab.label + marker_text(tab.marker);
            tab_bar->set_access_name(u8"탭: " + names);

            auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"tab-row" }, luil::stack_config { .direction = luil::stack_direction::row }) };
            row->add_flexible(std::move(tab_bar));
            row->add(make_icon_button(u8"new-tab", codicons::icon_add, u8"새 탭 (시작 페이지)", message(new_tab_intent {})), tab_row_button_width);
            row->add(make_icon_button(u8"command-palette", codicons::icon_terminal, palette_tooltip(view), message(open_palette_intent {}), true, view.palette.has_value()), tab_row_button_width);
            row->add(make_icon_button(u8"theme", codicons::icon_color_mode, theme_tooltip(view.theme), message(cycle_theme_intent {})), tab_row_button_width);
            row->add(make_icon_button(u8"settings", codicons::icon_settings_gear, u8"환경 설정", message(open_settings_intent {})), tab_row_button_width);
            row->add_gap(4.0f);
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"tab-row-bar" },
                bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.window_background; }, .divider_bottom = true }, std::move(row));
        }

        // ---------------------------------------------------------------- 툴바

        // 저장소·브랜치 선택 상자다. 누르면 그 상자 바로 아래에 거르는 칸이 있는 목록이 펼쳐진다 (`open_selector_intent`).
        class selector_element final : public luil::ui_element
        {
        public:
            selector_element(std::u8string owner, const selector_kind kind, const char32_t glyph, std::u8string caption, std::u8string value, std::u8string tail, const bool open)
                : ui_element { luil::ui_element_id { kind_selector, std::move(owner) } }
                , glyph_ { glyph }
                , caption_ { std::move(caption) }
                , value_ { std::move(value) }
                , tail_ { std::move(tail) }
                , open_ { open }
            {
                set_tooltip(caption_ + u8": " + value_ + u8" (눌러서 고르기)");
                set_cursor(luil::ui_cursor::hand);
                // 목록의 자리는 이 상자의 왼쪽 아래다. 배치가 끝난 뒤의 자리를 눌렀을 때 읽는다 (물리 픽셀).
                const auto open_list = [this, kind](const luil::ui_action_context&) -> std::vector<luil::input_action> {
                    const luil::rect_f box { bounds() };
                    return { message(open_selector_intent { kind, box.x, box.y + box.height - 5.0f, box.width }) };
                };
                set_action(luil::ui_trigger::left_click, open_list);
                // 보조 기술(UIA Invoke)도 같은 길로 목록을 연다.
                set_access_handler([open_list](const luil::access_request&) -> std::optional<std::vector<luil::input_action>> { return open_list(luil::ui_action_context {}); });
            }

            void arrange(const luil::arrange_context& context) override
            {
                set_bounds(context.slot);
            }

            [[nodiscard]] luil::access_info accessibility() const override
            {
                return { .role = luil::access_role::combo_box, .name = caption_ + u8" " + value_, .expanded = open_ };
            }

            void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override
            {
                const luil::rect_f box { bounds() };
                const float scale { context.scale };
                const luil::rect_f inner { box.x, box.y + 5.0f * scale, box.width, box.height - 10.0f * scale };
                luil::draw_hover_fill(context, inner, id(), interaction, true, context.metrics.control_corner_radius);
                const luil::ui_color_palette& palette { context.palette };
                float x { box.x + 6.0f * scale };
                luil::draw_centered_glyph(context.canvas, glyph_, { x, box.y, 16.0f * scale, box.height }, icon_font(context, 15.0f), luil::solid_paint(palette.primary_foreground));
                x += 22.0f * scale;
                const float chevron { 16.0f * scale };
                const float right { box.x + box.width - chevron - 6.0f * scale };
                const float half { box.height / 2.0f };
                const SkFont small { ui_font(context, 10.0f) };
                static_cast<void>(luil::draw_text_within(
                    context.canvas, caption_, x, box.y + 4.0f * scale + luil::centered_text_baseline(small, half), right - x, small, luil::solid_paint(palette.secondary_foreground)));
                const SkFont font { ui_font(context, 12.0f, true) };
                const float drawn = luil::draw_text_within(
                    context.canvas, value_, x, box.y + half - 3.0f * scale + luil::centered_text_baseline(font, half), right - x, font, luil::solid_paint(palette.primary_foreground));
                if (tail_.empty() == false && x + drawn + 6.0f * scale < right)
                    static_cast<void>(luil::draw_text_within(context.canvas, tail_, x + drawn + 6.0f * scale, box.y + half - 3.0f * scale + luil::centered_text_baseline(small, half),
                        right - x - drawn - 6.0f * scale, small, luil::solid_paint(palette.secondary_foreground)));
                luil::draw_centered_glyph(
                    context.canvas, codicons::icon_chevron_down, { right, box.y, chevron, box.height }, icon_font(context, 12.0f), luil::solid_paint(palette.secondary_foreground));
            }

        private:
            char32_t glyph_ { 0 };
            std::u8string caption_ {};
            std::u8string value_ {};
            std::u8string tail_ {};
            bool open_ { false };
        };

        // 툴바 묶음 사이의 세로 구분선이다.
        class separator_element final : public luil::ui_element
        {
        public:
            explicit separator_element(std::u8string owner)
                : ui_element { luil::ui_element_id { kind_layout, std::move(owner) } }
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
                const float height { box.height * 0.5f };
                context.canvas.drawRect(SkRect::MakeXYWH(box.x + box.width / 2.0f, box.y + (box.height - height) / 2.0f, context.scale, height), luil::solid_paint(context.palette.divider));
            }
        };

        std::unique_ptr<luil::ui_element> build_toolbar(const view_snapshot& view)
        {
            const toolbar_view& toolbar { view.toolbar };
            auto row = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"toolbar" },
                luil::stack_config { .direction = luil::stack_direction::row, .spacing = 2.0f, .padding = luil::edge_insets { 8.0f, 0.0f, 8.0f, 0.0f } });
            if (toolbar.show_repository_selector)
                row->add(std::make_unique<selector_element>(u8"repository-selector", selector_kind::repository, codicons::icon_repo, u8"저장소", toolbar.repository_name, std::u8string {},
                             view.selector.has_value() && view.selector->kind == selector_kind::repository),
                    150.0f);
            std::u8string tail {};
            if (toolbar.ahead > 0)
                tail += u8"↑" + number_text(toolbar.ahead);
            if (toolbar.behind > 0)
                tail += (tail.empty() ? u8"" : u8" ") + std::u8string { u8"↓" } + number_text(toolbar.behind);
            row->add(std::make_unique<selector_element>(u8"branch-selector", selector_kind::branch, codicons::icon_git_branch, u8"브랜치", toolbar.branch_text, std::move(tail),
                         view.selector.has_value() && view.selector->kind == selector_kind::branch),
                200.0f);
            row->add(std::make_unique<separator_element>(u8"toolbar-separator-1"), 13.0f);

            const float width { toolbar.show_labels ? tool_button_width : tool_button_compact_width };
            const auto tool = [&](std::u8string owner, const char32_t glyph, std::u8string label, std::u8string tooltip, const bool enabled, const toolbar_action action) {
                row->add(std::make_unique<tool_button_element>(std::move(owner), glyph, std::move(label), std::move(tooltip), toolbar.show_labels, enabled, message(toolbar_intent { action })), width);
            };
            tool(u8"fetch", codicons::icon_repo_fetch, u8"Fetch", u8"Fetch: 원격의 새 커밋과 ref를 받는다", toolbar.fetch_enabled, toolbar_action::fetch);
            tool(u8"pull", codicons::icon_repo_pull, u8"Pull", u8"Pull (--ff-only)", toolbar.pull_enabled, toolbar_action::pull);
            tool(u8"push", codicons::icon_repo_push, u8"Push",
                toolbar.push_needs_dialog ? std::u8string { u8"Push: upstream이 없어 원격을 골라 --set-upstream으로 보냅니다" }
                                          : u8"Push" + (toolbar.ahead > 0 ? u8" (↑" + number_text(toolbar.ahead) + u8")" : std::u8string {}),
                toolbar.push_enabled, toolbar_action::push);
            row->add(std::make_unique<separator_element>(u8"toolbar-separator-2"), 13.0f);
            tool(u8"branch", codicons::icon_git_branch, u8"Branch", u8"새 브랜치: 이름과 만들 위치를 정한다", toolbar.branch_enabled, toolbar_action::branch);
            tool(u8"stash", codicons::icon_git_stash, u8"Stash", u8"Stash: 변경을 stash에 넣는다 (추적하지 않는 파일 포함)", toolbar.stash_enabled, toolbar_action::stash);
            tool(u8"pop", codicons::icon_git_stash_pop, u8"Pop", u8"Pop: 맨 위 stash를 꺼낸다", toolbar.pop_enabled, toolbar_action::pop);
            row->add_flexible_gap();
            if (toolbar.busy_text.empty() == false)
            {
                row->add(std::make_unique<spinner_element>(luil::ui_element_id { kind_spinner, u8"toolbar-busy-spinner" }, codicons::icon_sync, color_selector {}), 20.0f);
                row->add(make_text(u8"toolbar-busy", toolbar.busy_text, text_style { .size = 11.0f, .color = [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } }),
                    std::min(260.0f, std::max(view.logical_width * 0.2f, 80.0f)));
            }
            if (toolbar.show_detail_toggle)
                row->add(make_icon_button(u8"detail-toggle", codicons::icon_layout_sidebar_right, toolbar.drawer_open ? u8"상세 패널 닫기" : u8"상세 패널 열기",
                             message(toggle_drawer_intent { toolbar.drawer_open == false }), true, toolbar.drawer_open, 16.0f),
                    36.0f);
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"toolbar-bar" },
                bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.window_background; }, .divider_bottom = true }, std::move(row));
        }

        // ---------------------------------------------------------------- 배너

        float banner_height(const std::optional<banner_view>& banner) noexcept
        {
            if (banner.has_value() == false)
                return 0.0f;
            if (banner->expanded == false || banner->details.empty())
                return shell_banner_height;
            const std::size_t lines { std::min(banner->details.size(), banner_visible_lines) + (banner->details.size() > banner_visible_lines ? 1u : 0u) };
            return shell_banner_height + static_cast<float>(lines) * banner_line_height + 8.0f;
        }

        std::unique_ptr<luil::ui_element> build_banner(const banner_view& banner)
        {
            auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"banner-column" }, luil::stack_config {}) };
            auto row = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"banner-row" },
                luil::stack_config { .direction = luil::stack_direction::row, .spacing = 6.0f, .padding = luil::edge_insets { 10.0f, 0.0f, 6.0f, 0.0f } });
            const bool error { banner.kind == banner_kind::failure };
            const bool warning { banner.warning };
            const char32_t glyph { error ? codicons::icon_error : warning ? codicons::icon_warning : codicons::icon_info };
            const color_selector tone { [error, warning](const luil::ui_color_palette& palette) { return error ? palette.error_accent : warning ? palette.warning_accent : palette.accent; } };
            row->add(std::make_unique<icon_element>(luil::ui_element_id { kind_label, u8"banner-icon" }, glyph, tone), 20.0f);
            row->add(make_text(u8"banner-title", banner.title, text_style { .size = 12.0f, .color = tone, .bold = true }), text_button_width(banner.title) + 8.0f);
            row->add_flexible(make_text(u8"banner-summary", banner.summary, text_style { .size = 12.0f }));
            for (const banner_action_view& action : banner.actions)
                row->add(make_text_button(u8"banner-action-" + action.key, action.label, luil::text_button_visual::accent, message(banner_action_intent { action.key, banner.id })),
                    text_button_width(action.label) + 24.0f);
            if (banner.details.empty() == false)
                row->add(make_text_button(u8"banner-details", banner.expanded ? u8"접기" : u8"자세히", luil::text_button_visual::normal, message(toggle_banner_details_intent {})), 64.0f);
            const luil::input_action close { banner.kind == banner_kind::notice ? message(dismiss_notice_intent {}) : message(dismiss_banner_intent { banner.id }) };
            row->add(make_icon_button(u8"banner-close", codicons::icon_close, u8"배너 닫기", close), 28.0f);
            column->add(std::move(row), shell_banner_height);
            if (banner.expanded)
            {
                auto details = std::make_unique<luil::stack_element>(
                    luil::ui_element_id { kind_layout, u8"banner-details-list" }, luil::stack_config { .padding = luil::edge_insets { 36.0f, 0.0f, 12.0f, 8.0f } });
                const std::size_t shown { std::min(banner.details.size(), banner_visible_lines) };
                for (std::size_t index { 0 }; index < shown; ++index)
                    details->add(make_text(u8"banner-detail-" + number_text(index), banner.details[index],
                                     text_style { .size = 11.0f, .color = [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; }, .code = true }),
                        banner_line_height);
                if (banner.details.size() > shown)
                    details->add(make_text(u8"banner-detail-more", u8"외 " + number_text(banner.details.size() - shown) + u8"줄 (작업 로그에서 전부 볼 수 있습니다)",
                                     text_style { .size = 11.0f, .color = [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } }),
                        banner_line_height);
                column->add_flexible(std::move(details));
            }
            bar_style style {};
            style.background = [error, warning](const luil::ui_color_palette& palette) {
                if (error)
                    return palette.notice_background;
                return luil::with_alpha(warning ? palette.warning_accent : palette.accent, 0.14f);
            };
            style.divider_bottom = true;
            return std::make_unique<bar_element>(luil::ui_element_id { kind_banner, u8"banner" }, std::move(style), std::move(column));
        }

        // ---------------------------------------------------------------- 상태 줄

        std::unique_ptr<luil::ui_element> build_status(const view_snapshot& view)
        {
            const status_view& status { view.status };
            luil::stack_config row_config { .direction = luil::stack_direction::row, .spacing = 6.0f, .padding = luil::edge_insets { 8.0f, 0.0f, 4.0f, 0.0f } };
            row_config.cross_alignment = luil::stack_alignment::center;
            auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"status-row" }, row_config) };
            const color_selector dim { [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } };
            if (status.active.has_value())
            {
                const active_operation_view& active { *status.active };
                row->add(
                    std::make_unique<spinner_element>(luil::ui_element_id { kind_spinner, u8"status-spinner" }, active.queued ? codicons::icon_watch : codicons::icon_sync, color_selector {}), 18.0f);
                row->add_flexible(make_text(u8"status-operation", active.text, text_style { .size = 11.0f }));
                if (active.fraction.has_value())
                    row->add(std::make_unique<luil::progress_element>(
                                 luil::ui_element_id { kind_progress, u8"status-progress" }, luil::progress_config { .value = static_cast<float>(*active.fraction), .thickness = 4.0f }),
                        { .length = 90.0f, .cross_length = 4.0f });
                if (active.cancellable)
                    row->add(
                        make_text_button(u8"status-cancel", u8"취소", luil::text_button_visual::normal, message(cancel_operation_intent { active.id })), { .length = 48.0f, .cross_length = 20.0f });
            }
            else
            {
                const bool failed { status.last_failed };
                const bool waiting { status.last_needs_input };
                text_style icon_style { .size = 12.0f, .bold = waiting };
                icon_style.color = [failed, waiting](const luil::ui_color_palette& palette) {
                    if (failed)
                        return palette.error_accent;
                    return waiting ? palette.warning_accent : success_color(palette);
                };
                const std::u8string icon { failed ? u8"✗" : waiting ? u8"!" : u8"✓" };
                if (status.last_result.empty() == false)
                    row->add(make_text(u8"status-result-icon", icon, std::move(icon_style)), 14.0f);
                row->add_flexible(make_text(u8"status-result", status.last_result.empty() ? std::u8string { u8"준비됨" } : status.last_result, text_style { .size = 11.0f, .color = dim }));
            }
            if (status.queued_count > 0)
                row->add(make_text(u8"status-queued", u8"쓰기 대기 " + number_text(status.queued_count), text_style { .size = 11.0f, .color = dim }), 80.0f);
            // 이 저장소가 커밋에 쓰는 작성자와 출처다. 누르면 프로필을 고르는 팔레트가 열린다.
            if (status.identity_visible && view.logical_width >= medium_layout_minimum_width)
            {
                const float width { std::min({ 460.0f, view.logical_width * 0.3f, estimate_text_width(status.identity_text, 11.0f) + 44.0f }) };
                const luil::text_button_visual visual { status.identity_warning ? luil::text_button_visual::danger : luil::text_button_visual::normal };
                auto identity { make_text_button(u8"status-identity", status.identity_text, visual, message(open_palette_intent { palette_mode::profiles }), true, codicons::icon_person) };
                identity->set_tooltip(status.identity_tooltip);
                row->add(std::move(identity), { .length = width, .cross_length = 20.0f });
            }
            if (status.repository && view.logical_width >= medium_layout_minimum_width)
            {
                row->add(make_text(u8"status-branch", status.branch_text, text_style { .size = 11.0f, .color = dim, .align = 2 }), 150.0f);
                row->add(make_text(u8"status-ahead-behind", u8"↑" + number_text(status.ahead) + u8" ↓" + number_text(status.behind), text_style { .size = 11.0f, .color = dim }), 56.0f);
                row->add(make_text(u8"status-changes", u8"변경 " + number_text(status.changes), text_style { .size = 11.0f, .color = dim }), 60.0f);
            }
            row->add(make_text_button(u8"operation-log-toggle", u8"작업 로그", status.log_open ? luil::text_button_visual::accent : luil::text_button_visual::normal,
                         message(toggle_operation_log_intent {}), true, codicons::icon_output),
                { .length = 92.0f, .cross_length = 20.0f });
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"status-bar" },
                bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.window_background; }, .divider_top = true }, std::move(row));
        }

        // ---------------------------------------------------------------- 작업 로그

        std::unique_ptr<luil::ui_element> build_log(const view_snapshot& view, feedback_sources& sources)
        {
            auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"operation-log" }, luil::stack_config {}) };
            auto header = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"operation-log-header" },
                luil::stack_config { .direction = luil::stack_direction::row, .padding = luil::edge_insets { 10.0f, 0.0f, 4.0f, 0.0f } });
            header->add_flexible(make_text(u8"operation-log-title", u8"작업 로그 · 실행한 git 명령줄", text_style { .size = 12.0f, .bold = true }));
            header->add(make_icon_button(u8"operation-log-close", codicons::icon_close, u8"작업 로그 닫기", message(toggle_operation_log_intent {})), 28.0f);
            column->add(std::move(header), log_header_height);

            auto lines { std::make_shared<std::vector<log_line_view>>(view.log) };
            luil::virtual_list_config config {};
            config.owner = u8"operation-log-list";
            for (const log_line_view& line : *lines)
                config.items.push_back({ .key = line.key, .label = line.text });
            config.row_height = log_row_height;
            config.scroll_offset = view.log_scroll;
            config.build_row
                = [lines](const luil::virtual_list_item&, const std::size_t index, bool) -> std::unique_ptr<luil::ui_element> { return std::make_unique<log_row_element>((*lines)[index]); };
            config.scroll = [](const float delta) { return message(scroll_intent { .target = scroll_target::operation_log, .value = delta }); };
            config.scroll_to = [](const float offset) { return message(scroll_intent { .target = scroll_target::operation_log, .value = offset, .absolute = true }); };
            config.edges = luil::scroll_edges { .shadows = true };
            auto list { std::make_unique<luil::virtual_list_element>(std::move(config)) };
            list->set_access_name(u8"작업 로그");
            sources.log = list.get();
            column->add_flexible(std::move(list));
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"operation-log-panel" },
                bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.surface_background; }, .divider_top = true }, std::move(column));
        }

        // ---------------------------------------------------------------- Clone 대화 상자

        std::unique_ptr<luil::ui_element> build_clone_dialog(const clone_dialog_view& dialog)
        {
            luil::modal_host_config config {};
            config.owner = u8"clone-dialog";
            config.name = u8"저장소 Clone";
            config.dismiss = action_of(message(close_clone_dialog_intent {}));
            config.focus_entry = luil::ui_element_id { kind_clone_url_input, u8"clone-url" };
            config.surface = floating_panel_config(8.0f, 0.4f);
            config.content_width = clone_dialog_width;
            config.content_height = clone_dialog_height;
            auto host { std::make_unique<luil::modal_host_element>(std::move(config)) };

            auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"clone-column" }, luil::stack_config {}) };
            luil::dialog_caption_config caption {
                .owner = u8"clone", .title = u8"저장소 Clone", .close = action_of(message(close_clone_dialog_intent {})), .close_tooltip = u8"닫기 (clone은 계속됩니다)"
            };
            const float caption_height { luil::dialog_caption_element::height_for(caption) };
            column->add(std::make_unique<luil::dialog_caption_element>(std::move(caption)), caption_height);

            auto body { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"clone-body" }, luil::stack_config { .spacing = 6.0f, .padding = luil::edge_insets::all(16.0f) }) };
            const color_selector dim { [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } };
            body->add(make_text(u8"clone-url-label", u8"원격 URL", text_style { .size = 11.0f, .color = dim }), 16.0f);
            luil::text_input_config url_config { .placeholder = u8"https://github.com/owner/repo.git 또는 git@host:owner/repo.git" };
            body->add(std::make_unique<luil::text_input_element>(luil::ui_element_id { kind_clone_url_input, u8"clone-url" }, dialog.url, std::move(url_config)), 28.0f);
            body->add(make_text(u8"clone-path-label", u8"받을 위치 (새 폴더)", text_style { .size = 11.0f, .color = dim }), 16.0f);
            auto path_row
                = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"clone-path-row" }, luil::stack_config { .direction = luil::stack_direction::row, .spacing = 6.0f });
            path_row->add_flexible(
                std::make_unique<luil::text_input_element>(luil::ui_element_id { kind_clone_path_input, u8"clone-path" }, dialog.path, luil::text_input_config { .placeholder = u8"E:\\work\\repo" }));
            path_row->add(make_text_button(u8"clone-browse", u8"찾아보기…", luil::text_button_visual::normal, ui_command(static_cast<std::uint32_t>(ui_command_id::clone_target_folder))), 86.0f);
            body->add(std::move(path_row), 28.0f);
            const std::u8string preview { u8"git clone --progress -- " + std::u8string { dialog.url.text } + u8" " + std::u8string { dialog.path.text } };
            body->add(make_text(u8"clone-command", preview, text_style { .size = 11.0f, .color = dim, .code = true }), 18.0f);
            if (dialog.running)
            {
                body->add(make_text(u8"clone-phase", dialog.phase, text_style { .size = 11.0f }), 18.0f);
                body->add(std::make_unique<luil::progress_element>(
                              luil::ui_element_id { kind_progress, u8"clone-progress" }, luil::progress_config { .value = static_cast<float>(dialog.fraction.value_or(0.0)), .thickness = 4.0f }),
                    6.0f);
            }
            else if (dialog.error.empty() == false)
                body->add(make_text(u8"clone-error", dialog.error, text_style { .size = 11.0f, .color = [](const luil::ui_color_palette& palette) { return palette.error_accent; } }), 36.0f);
            body->add_flexible_gap();
            auto buttons
                = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"clone-buttons" }, luil::stack_config { .direction = luil::stack_direction::row, .spacing = 8.0f });
            buttons->add_flexible_gap();
            if (dialog.running)
                buttons->add(make_text_button(u8"clone-cancel-operation", u8"Clone 취소", luil::text_button_visual::danger, message(cancel_operation_intent { dialog.operation })), 96.0f);
            buttons->add(make_text_button(u8"clone-close", dialog.running ? u8"닫기 (계속 진행)" : u8"취소", luil::text_button_visual::normal, message(close_clone_dialog_intent {})),
                dialog.running ? 120.0f : 72.0f);
            buttons->add(make_text_button(u8"clone-submit", u8"Clone", luil::text_button_visual::accent, message(submit_clone_intent {}), dialog.can_submit, codicons::icon_repo_clone, true), 92.0f);
            body->add(std::move(buttons), 30.0f);
            column->add_flexible(std::move(body));
            host->set_content(std::move(column));
            return host;
        }

        // 사이드바(왼쪽 판)나 상세(오른쪽 판)의 폭을 끄는 손잡이다. +는 언제나 그 판이 넓어지는 쪽이다.
        std::unique_ptr<luil::ui_element> make_splitter(const bool sidebar)
        {
            luil::split_handle_config config {};
            config.axis = luil::split_axis::horizontal;
            config.grows = sidebar ? luil::split_grows::toward_end : luil::split_grows::toward_start;
            config.resize = [sidebar](const float delta) { return message(resize_pane_intent { .sidebar = sidebar, .delta = delta }); };
            return std::make_unique<luil::split_handle_element>(luil::ui_element_id { kind_splitter, sidebar ? u8"sidebar-splitter" : u8"detail-splitter" }, std::move(config));
        }

        // 판 둘레를 바탕색으로 칠하고 왼쪽에 1px 선과 그림자를 두는 서랍 틀이다.
        std::unique_ptr<luil::ui_element> drawer_frame(std::unique_ptr<luil::ui_element> content)
        {
            auto panel { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_panel, u8"detail-drawer" }, floating_panel_config(0.0f, 0.35f)) };
            panel->set_content(std::move(content));
            panel->set_hit_opaque(true);
            return panel;
        }
    } // namespace

    built_tree build_ui_tree(const view_snapshot& view)
    {
        const float scale { view.scale > 0.0f ? view.scale : 1.0f };
        const luil::caption_config caption_config { cyella_caption() };
        const bool window_caption { luil::current_ui_platform().window_caption };
        const float caption_height { window_caption ? luil::caption_element::height_for(caption_config) : 0.0f };

        const shell_layout_input input {
            .width = view.logical_width,
            .height = view.logical_height,
            .caption_height = caption_height,
            .repository = view.repository.has_value(),
            .banner_height = banner_height(view.banner),
            .log_open = view.log_open,
            .mode = view.mode,
            .sidebar_width = view.sidebar_width,
            .detail_width = view.detail_width,
            .drawer_open = view.drawer_open,
        };
        const shell_rects rects { compute_shell_layout(input) };

        feedback_sources sources {};
        auto root { std::make_unique<absolute_element>(luil::ui_element_id { luil::ui_element_kind::root }) };
        if (view.repository.has_value())
        {
            const repository_view& repository { *view.repository };
            root->place(build_graph_area(view, repository, rects.graph.width, sources), rects.graph);
            if (view.mode == layout_mode::narrow)
                root->place(build_rail(repository), rects.sidebar);
            else
            {
                root->place(build_sidebar(repository, sources), rects.sidebar);
                root->place(make_splitter(true), rects.sidebar_splitter);
            }
            if (rects.detail_is_drawer == false && rects.detail.empty() == false)
            {
                root->place(build_detail(view, repository, false, rects.detail.width, rects.detail.height, sources), rects.detail);
                root->place(make_splitter(false), rects.detail_splitter);
            }
            else if (rects.detail_is_drawer)
                root->place(drawer_frame(build_detail(view, repository, true, rects.detail.width, rects.detail.height, sources)), rects.detail);
            if (view.mode == layout_mode::narrow && repository.rail_section.has_value())
            {
                const float height { std::min(rects.body.height - 8.0f, 420.0f) };
                root->place(build_rail_popup(repository, *repository.rail_section), { rects.sidebar.x + rects.sidebar.width + 4.0f, rects.body.y + 4.0f, 260.0f, height });
            }
            root->place(build_toolbar(view), rects.toolbar);
        }
        else if (view.start.has_value())
            root->place(build_start_page(view, *view.start, sources), rects.body);

        if (view.banner.has_value())
            root->place(build_banner(*view.banner), rects.banner);
        if (view.log_open)
            root->place(build_log(view, sources), rects.log);
        root->place(build_status(view), rects.status);
        root->place(build_tab_row(view), rects.tab_row);
        if (window_caption)
        {
            root->place(std::make_unique<luil::caption_element>(caption_config), { 0.0f, 0.0f, view.logical_width, caption_height });
            // 앱 심볼은 캡션의 아이콘 칸 위에 얹는다.
            if (app_symbol().valid())
                root->place(make_app_symbol(u8"caption-symbol"), caption_icon_rect(caption_config));
        }
        // 끄는 동안의 문구는 대화 상자 아래, 나머지 화면 위에 그린다.
        if (view.repository.has_value() && view.repository->drop_hints != nullptr)
            root->place(build_drag_hint_overlay(view.repository->drop_hints), { 0.0f, 0.0f, view.logical_width, view.logical_height });
        // 대화 상자는 마지막 자식이라 모든 것 위에 그려지고 누름도 먼저 받는다.
        if (view.clone.has_value())
            root->place(build_clone_dialog(*view.clone), { 0.0f, 0.0f, view.logical_width, view.logical_height });
        if (view.settings.has_value())
            root->place(build_settings_dialog(*view.settings, view.logical_width, view.logical_height, sources), { 0.0f, 0.0f, view.logical_width, view.logical_height });
        // 인터랙티브 rebase 편집기다. 인증 질문·확인 상자는 그 위에 선다.
        if (view.rebase_editor.has_value())
            root->place(build_rebase_editor(*view.rebase_editor, view.logical_width, view.logical_height), { 0.0f, 0.0f, view.logical_width, view.logical_height });
        // 명령 팔레트는 설정 화면 위, 확인·인증 상자 아래에 선다.
        if (view.palette.has_value())
            root->place(build_palette_dialog(*view.palette, view.logical_width, view.logical_height), { 0.0f, 0.0f, view.logical_width, view.logical_height });
        if (view.form.has_value())
            root->place(build_form_dialog(*view.form, view.logical_width, view.logical_height), { 0.0f, 0.0f, view.logical_width, view.logical_height });
        // 키 처리가 읽는 단축키 표다. 보이지 않는 1x1 표식이다 (빈 사각형은 놓지 않으므로 크기를 준다).
        root->place(std::make_unique<shortcut_table_element>(view.shortcuts), { 0.0f, 0.0f, 1.0f, 1.0f });

        built_tree built {};
        if (view.menu.has_value())
            built.popups.push_back(build_menu_popup(*view.menu, scale));
        if (view.selector.has_value())
            built.popups.push_back(build_selector_popup(*view.selector, scale));
        built.tree = std::make_shared<const luil::ui_tree>(luil::make_arranged_tree(std::move(root), { 0.0f, 0.0f, view.width, view.height }, scale));
        if (sources.graph != nullptr)
        {
            built.feedback.graph_maximum = sources.graph->metrics().maximum_scroll;
            built.feedback.graph_viewport = sources.graph->metrics().viewport_height;
        }
        if (sources.sidebar != nullptr)
            built.feedback.sidebar_maximum = sources.sidebar->metrics().maximum_scroll;
        if (sources.detail != nullptr)
            built.feedback.detail_maximum = sources.detail->metrics().maximum_scroll;
        if (sources.commit_meta != nullptr)
            built.feedback.commit_meta_maximum = sources.commit_meta->metrics().maximum_scroll;
        if (sources.diff != nullptr)
            built.feedback.diff_maximum = sources.diff->metrics().maximum_scroll;
        if (sources.log != nullptr)
            built.feedback.log_maximum = sources.log->metrics().maximum_scroll;
        if (sources.start != nullptr)
            built.feedback.start_maximum = sources.start->metrics().maximum_scroll;
        if (sources.settings != nullptr)
            built.feedback.settings_maximum = sources.settings->metrics().maximum_scroll;
        return built;
    }
} // namespace cyella::ui
