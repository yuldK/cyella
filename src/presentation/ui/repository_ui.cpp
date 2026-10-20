#include "application/app_messages.h"
#include "presentation/build_view_snapshot.h"
#include "presentation/dialog_intents.h"
#include "presentation/extension_intents.h"
#include "presentation/ui/diff_webview.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/graph_row_element.h"
#include "presentation/ui/list_rows.h"
#include "presentation/ui/sidebar_extension_rows.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui/ui_parts.h"
#include "presentation/ui_intents.h"

#include "luil/ui/check_element.h"
#include "luil/ui/dialog_elements.h"
#include "luil/ui/panel_element.h"
#include "luil/ui/stack_element.h"
#include "luil/ui/virtual_list_element.h"

#include <algorithm>
#include <map>
#include <memory>
#include <utility>

namespace cyella::ui {
    namespace {
        constexpr float pane_header_height { 36.0f };
        constexpr float filter_row_height { 36.0f };
        constexpr float detail_line_height { 18.0f };
        constexpr std::size_t detail_body_line_limit { 10 };
        constexpr float commit_box_height { 172.0f };
        constexpr float amend_warning_extra { 44.0f };

        const color_selector dim_color { [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } };

        std::unique_ptr<luil::ui_element> surface(std::u8string owner, std::unique_ptr<luil::ui_element> content, const bool window = false)
        {
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, std::move(owner) },
                bar_style { .background = [window](const luil::ui_color_palette& palette) { return window ? palette.window_background : palette.surface_background; } }, std::move(content));
        }

        // 판 가운데의 안내 한 줄이다 (고른 것 없음, 읽는 중).
        std::unique_ptr<luil::ui_element> centered_message(std::u8string owner, std::u8string text, color_selector color = dim_color)
        {
            auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, owner + u8"-column" }, luil::stack_config { .padding = luil::edge_insets::all(16.0f) }) };
            column->add_flexible_gap();
            column->add(make_text(std::move(owner), std::move(text), text_style { .size = 12.0f, .color = std::move(color), .align = 1 }), 24.0f);
            column->add_flexible_gap();
            return column;
        }

        luil::input_action submit(const std::string& repository, git_operation operation)
        {
            return message(submit_operation_intent { repository, std::move(operation) });
        }

        // ---------------------------------------------------------------- 사이드바

        struct sidebar_index
        {
            std::map<std::u8string, sidebar_row_view> rows {};
        };

        // 사이드바 행을 우클릭했을 때의 메뉴 대상이다.
        std::optional<menu_target> sidebar_menu_target(const sidebar_row_view& row)
        {
            menu_target target {};
            target.name = row.ref_name;
            target.sha = row.target_sha;
            // submodule·worktree·LFS 행
            switch (row.item)
            {
            case sidebar_item_kind::submodule:
                target.kind = menu_target_kind::submodule;
                target.name = row.path;
                return target;
            case sidebar_item_kind::worktree:
                target.kind = menu_target_kind::worktree;
                target.name = row.path;
                return target;
            case sidebar_item_kind::lfs:
                if (row.path.empty())
                    return std::nullopt;
                target.kind = menu_target_kind::lfs_file;
                target.name = row.path;
                return target;
            case sidebar_item_kind::ref:
                break;
            }
            switch (row.kind)
            {
            case ref_kind::local:
                target.kind = menu_target_kind::local_branch;
                return target;
            case ref_kind::remote:
                target.kind = menu_target_kind::remote_branch;
                return target;
            case ref_kind::tag:
                target.kind = menu_target_kind::tag;
                return target;
            case ref_kind::stash:
                target.kind = menu_target_kind::stash;
                target.index = row.stash_index;
                return target;
            case ref_kind::head:
                break;
            }
            return std::nullopt;
        }

        luil::input_action sidebar_select_action(const sidebar_row_view& row)
        {
            if (row.header)
                return message(toggle_sidebar_section_intent { row.section });
            return message(select_sidebar_item_intent { row.key, row.group ? std::string {} : row.target_sha });
        }

        std::unique_ptr<luil::ui_element> make_sidebar_list(std::u8string owner, const std::vector<sidebar_row_view>& rows, const std::u8string& selected, const float scroll, const bool scrolls)
        {
            auto index { std::make_shared<sidebar_index>() };
            luil::virtual_list_config config {};
            config.owner = std::move(owner);
            for (const sidebar_row_view& row : rows)
            {
                config.items.push_back({ .key = row.key, .label = row.label, .height = row.header ? sidebar_header_row_height : sidebar_row_height });
                index->rows.emplace(row.key, row);
            }
            config.selected = selected;
            config.cursor = selected;
            config.row_height = sidebar_row_height;
            config.scroll_offset = scroll;
            config.build_row = [index](const luil::virtual_list_item& item, std::size_t, bool) -> std::unique_ptr<luil::ui_element> {
                const auto found { index->rows.find(item.key) };
                if (found == index->rows.end())
                    return nullptr;
                const sidebar_row_view& row { found->second };
                std::optional<luil::input_action> select {};
                std::optional<luil::input_action> activate {};
                std::optional<menu_target> menu {};
                if (row.header)
                {
                    select = sidebar_select_action(row);
                    if (row.section == sidebar_section::tags)
                        menu = menu_target { .kind = menu_target_kind::tags_section };
                    else if (row.section == sidebar_section::submodules)
                        menu = menu_target { .kind = menu_target_kind::submodules_section };
                    else if (row.section == sidebar_section::worktrees)
                        menu = menu_target { .kind = menu_target_kind::worktrees_section };
                    else if (row.section == sidebar_section::lfs)
                        menu = menu_target { .kind = menu_target_kind::lfs_section };
                }
                else if (row.group == false)
                {
                    select = sidebar_select_action(row);
                    activate = row.item == sidebar_item_kind::ref ? message(activate_ref_intent { row.kind, row.ref_name }) : message(activate_sidebar_item_intent { row.item, row.path });
                    menu = sidebar_menu_target(row);
                }
                return std::make_unique<sidebar_row_element>(row, std::move(select), std::move(activate), std::move(menu));
            };
            config.select = [index](const std::u8string& key) {
                const auto found { index->rows.find(key) };
                return found == index->rows.end() ? luil::input_action {} : sidebar_select_action(found->second);
            };
            // 키보드로 훑을 때는 머리 줄을 접지 않고 고르기만 한다.
            config.move = [index](const std::u8string& key) {
                const auto found { index->rows.find(key) };
                if (found == index->rows.end())
                    return luil::input_action {};
                return message(select_sidebar_item_intent { key, found->second.header || found->second.group ? std::string {} : found->second.target_sha });
            };
            config.activate = [index](const std::u8string& key) {
                const auto found { index->rows.find(key) };
                if (found == index->rows.end())
                    return luil::input_action {};
                const sidebar_row_view& row { found->second };
                if (row.header)
                    return message(toggle_sidebar_section_intent { row.section });
                if (row.group)
                    return luil::input_action {};
                if (row.item != sidebar_item_kind::ref)
                    return message(activate_sidebar_item_intent { row.item, row.path });
                return message(activate_ref_intent { row.kind, row.ref_name });
            };
            if (scrolls)
            {
                config.scroll = [](const float delta) { return message(scroll_intent { .target = scroll_target::sidebar, .value = delta }); };
                config.scroll_to = [](const float offset) { return message(scroll_intent { .target = scroll_target::sidebar, .value = offset, .absolute = true }); };
            }
            config.edges = luil::scroll_edges { .shadows = true };
            auto list { std::make_unique<luil::virtual_list_element>(std::move(config)) };
            list->set_access_name(u8"브랜치·태그 목록");
            return list;
        }

        std::u8string section_title(const sidebar_section section)
        {
            switch (section)
            {
            case sidebar_section::local:
                return u8"LOCAL";
            case sidebar_section::remote:
                return u8"REMOTE";
            case sidebar_section::tags:
                return u8"TAGS";
            case sidebar_section::stashes:
                return u8"STASHES";
            case sidebar_section::submodules:
                return u8"SUBMODULES";
            case sidebar_section::worktrees:
                return u8"WORKTREES";
            case sidebar_section::lfs:
                return u8"GIT LFS";
            }
            return {};
        }

        char32_t section_icon(const sidebar_section section)
        {
            switch (section)
            {
            case sidebar_section::local:
                return codicons::icon_git_branch;
            case sidebar_section::remote:
                return codicons::icon_cloud;
            case sidebar_section::tags:
                return codicons::icon_tag;
            case sidebar_section::stashes:
                return codicons::icon_archive;
            case sidebar_section::submodules:
            case sidebar_section::worktrees:
            case sidebar_section::lfs:
                return extension_section_glyph(section);
            }
            return codicons::icon_git_branch;
        }

        // ---------------------------------------------------------------- 상세

        std::unique_ptr<luil::ui_element> detail_header(std::u8string title, std::u8string tail, const bool drawer)
        {
            auto row = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"detail-header" },
                luil::stack_config { .direction = luil::stack_direction::row, .spacing = 6.0f, .padding = luil::edge_insets { 12.0f, 0.0f, 6.0f, 0.0f } });
            row->add(make_text(u8"detail-title", title, text_style { .size = 13.0f, .bold = true }), text_button_width(title));
            row->add_flexible(make_text(u8"detail-subtitle", std::move(tail), text_style { .size = 11.0f, .color = dim_color }));
            if (drawer)
                row->add(make_icon_button(u8"detail-close", codicons::icon_close, u8"상세 서랍 닫기", message(toggle_drawer_intent { false })), 28.0f);
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"detail-header-bar" }, bar_style { .divider_bottom = true }, std::move(row));
        }

        std::unique_ptr<luil::ui_element> make_file_list(std::u8string owner, std::vector<luil::virtual_list_item> items,
            std::shared_ptr<std::map<std::u8string, std::function<std::unique_ptr<luil::ui_element>()>>> builders, std::shared_ptr<std::map<std::u8string, diff_file_request>> requests,
            std::u8string selected, const float scroll, feedback_sources& sources)
        {
            luil::virtual_list_config config {};
            config.owner = std::move(owner);
            config.items = std::move(items);
            config.selected = selected;
            config.cursor = selected;
            config.row_height = file_row_height;
            config.scroll_offset = scroll;
            config.build_row = [builders](const luil::virtual_list_item& item, std::size_t, bool) -> std::unique_ptr<luil::ui_element> {
                const auto found { builders->find(item.key) };
                return found == builders->end() ? nullptr : found->second();
            };
            const auto select = [requests](const std::u8string& key) {
                const auto found { requests->find(key) };
                return found == requests->end() ? luil::input_action {} : message(select_file_intent { found->second });
            };
            config.select = select;
            config.move = select;
            config.scroll = [](const float delta) { return message(scroll_intent { .target = scroll_target::detail, .value = delta }); };
            config.scroll_to = [](const float offset) { return message(scroll_intent { .target = scroll_target::detail, .value = offset, .absolute = true }); };
            config.edges = luil::scroll_edges { .shadows = true };
            auto list { std::make_unique<luil::virtual_list_element>(std::move(config)) };
            list->set_access_name(u8"파일 목록");
            sources.detail = list.get();
            return list;
        }

        std::unique_ptr<luil::ui_element> build_commit_detail(const repository_view& repository, const commit_detail_view& commit, const bool drawer, feedback_sources& sources)
        {
            auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"commit-detail" }, luil::stack_config {}) };
            column->add(detail_header(u8"커밋", commit.short_sha, drawer), pane_header_height);

            auto meta { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"commit-meta" }, luil::stack_config { .padding = luil::edge_insets { 12.0f, 8.0f, 12.0f, 6.0f } }) };
            float meta_height { 14.0f };
            const auto add = [&meta, &meta_height](std::unique_ptr<luil::ui_element> child, const float length) {
                meta_height += length;
                meta->add(std::move(child), length);
            };
            add(make_text(u8"detail-subject", commit.subject.empty() ? std::u8string { u8"…" } : commit.subject, text_style { .size = 14.0f, .bold = true }), 24.0f);
            std::size_t line_index { 0 };
            std::size_t start { 0 };
            const std::u8string& body { commit.body };
            while (start < body.size() && line_index < detail_body_line_limit)
            {
                std::size_t end { body.find(u8'\n', start) };
                if (end == std::u8string::npos)
                    end = body.size();
                add(make_text(u8"detail-body-" + number_text(line_index), body.substr(start, end - start), text_style { .size = 12.0f }), 17.0f);
                ++line_index;
                start = end + 1;
            }
            if (commit.labels.empty() == false)
                add(std::make_unique<ref_label_strip_element>(u8"detail-labels", commit.labels, commit.lane_color), 26.0f);
            add(make_text(u8"detail-author", u8"작성자  " + commit.author, text_style { .size = 11.0f, .color = dim_color }), detail_line_height);
            add(make_text(u8"detail-date", u8"날짜  " + commit.date, text_style { .size = 11.0f, .color = dim_color }), detail_line_height);
            add(make_text(u8"detail-sha", u8"SHA  " + to_u8(commit.sha), text_style { .size = 11.0f, .color = dim_color, .code = true }), detail_line_height);
            std::u8string parents {};
            for (const std::u8string& parent : commit.parents)
                parents += (parents.empty() ? u8"" : u8", ") + parent;
            add(make_text(u8"detail-parents", u8"부모  " + (parents.empty() ? std::u8string { u8"(없음)" } : parents), text_style { .size = 11.0f, .color = dim_color, .code = true }),
                detail_line_height);
            column->add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"commit-meta-bar" }, bar_style { .divider_bottom = true }, std::move(meta)), meta_height);

            if (commit.loading)
            {
                column->add_flexible(centered_message(u8"detail-loading", u8"변경 파일을 읽는 중…"));
                return column;
            }
            column->add(make_text(u8"detail-files-title", u8"   변경 파일 " + number_text(commit.files.size()) + u8"개", text_style { .size = 11.0f, .color = dim_color, .bold = true }), 26.0f);
            auto builders { std::make_shared<std::map<std::u8string, std::function<std::unique_ptr<luil::ui_element>()>>>() };
            auto requests { std::make_shared<std::map<std::u8string, diff_file_request>>() };
            std::vector<luil::virtual_list_item> items {};
            std::u8string selected {};
            for (const file_row_view& row : commit.files)
            {
                items.push_back({ .key = row.key, .label = row.name });
                (*builders)[row.key] = [row] { return std::make_unique<file_row_element>(row, std::vector<file_row_button> {}); };
                (*requests)[row.key] = diff_file_request { .path = row.path, .view = diff_view::commit, .sha = commit.sha, .untracked = false };
                if (row.selected)
                    selected = row.key;
            }
            column->add_flexible(make_file_list(u8"commit-files", std::move(items), std::move(builders), std::move(requests), std::move(selected), repository.detail_scroll, sources));
            return column;
        }

        std::unique_ptr<luil::ui_element> build_working_area(const repository_view& repository, const working_area_view& working, const bool drawer, feedback_sources& sources)
        {
            const std::string& repo { repository.repository_id };
            const std::size_t total { working.conflicts.size() + working.unstaged.size() + working.staged.size() };
            auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"working-area" }, luil::stack_config {}) };
            column->add(detail_header(u8"작업 영역", number_text(total) + u8"개 변경 · " + working.branch, drawer), pane_header_height);

            auto builders { std::make_shared<std::map<std::u8string, std::function<std::unique_ptr<luil::ui_element>()>>>() };
            auto requests { std::make_shared<std::map<std::u8string, diff_file_request>>() };
            std::vector<luil::virtual_list_item> items {};
            std::u8string selected {};

            const auto add_group = [&](std::u8string key, std::u8string title, const std::vector<file_row_view>& rows, std::optional<file_row_button> button) {
                items.push_back({ .key = key, .label = title, .height = 28.0f, .enabled = false });
                (*builders)[key] = [key, title, count = rows.size(), button]() mutable { return std::make_unique<file_group_element>(u8"group-" + key, title, count, std::move(button)); };
            };
            const auto add_rows = [&](const std::vector<file_row_view>& rows, const diff_view view) {
                for (const file_row_view& row : rows)
                {
                    items.push_back({ .key = row.key, .label = row.name });
                    std::vector<file_row_button> buttons {};
                    if (view == diff_view::unstaged && row.conflicted == false)
                    {
                        buttons.push_back(make_row_button(u8"stage:" + to_u8(row.path), codicons::icon_add, u8"stage", submit(repo, stage_files_request { { row.path } })));
                        // 버리기는 확인 대화 상자를 거친다. 추적하지 않는 파일은 휴지통 단계가 따로 없어 버리지 않는다.
                        const bool discardable { row.untracked == false };
                        std::u8string tooltip { discardable ? u8"변경 버리기 (확인 후)" : u8"추적하지 않는 파일은 버릴 수 없습니다" };
                        buttons.push_back(make_row_button(u8"discard:" + to_u8(row.path), codicons::icon_discard, std::move(tooltip), message(discard_files_intent { { row.path } }), discardable));
                    }
                    if (view == diff_view::staged)
                        buttons.push_back(make_row_button(u8"unstage:" + to_u8(row.path), codicons::icon_remove, u8"unstage", submit(repo, unstage_files_request { { row.path } })));
                    const diff_file_request request { .path = row.path, .view = view, .sha = {}, .untracked = row.untracked };
                    menu_target target {};
                    target.kind = menu_target_kind::file;
                    target.name = row.path;
                    target.side = view == diff_view::staged ? change_side::staged : change_side::unstaged;
                    target.untracked = row.untracked;
                    target.conflicted = row.conflicted;
                    target.lfs = row.lfs;
                    (*builders)[row.key] = [row, buttons, request, target] { return std::make_unique<file_row_element>(row, buttons, message(select_file_intent { request }), target); };
                    (*requests)[row.key] = request;
                    if (row.selected)
                        selected = row.key;
                }
            };

            if (working.conflicts.empty() == false)
            {
                add_group(u8"g:conflicts", u8"충돌", working.conflicts, std::nullopt);
                add_rows(working.conflicts, diff_view::unstaged);
            }
            std::vector<std::string> unstaged_paths {};
            for (const file_row_view& row : working.unstaged)
                unstaged_paths.push_back(row.path);
            std::vector<std::string> staged_paths {};
            for (const file_row_view& row : working.staged)
                staged_paths.push_back(row.path);
            const bool any_unstaged { working.unstaged.empty() == false };
            add_group(u8"g:unstaged", u8"Unstaged 변경", working.unstaged,
                make_row_button(u8"stage-all", codicons::icon_add, u8"모두 stage", submit(repo, stage_files_request { unstaged_paths }), any_unstaged));
            add_rows(working.unstaged, diff_view::unstaged);
            const bool any_staged { working.staged.empty() == false };
            add_group(u8"g:staged", u8"Staged 변경", working.staged,
                make_row_button(u8"unstage-all", codicons::icon_remove, u8"모두 unstage", submit(repo, unstage_files_request { staged_paths }), any_staged));
            add_rows(working.staged, diff_view::staged);
            column->add_flexible(make_file_list(u8"working-files", std::move(items), std::move(builders), std::move(requests), std::move(selected), repository.detail_scroll, sources));

            // 커밋 상자 (아래 고정)
            auto box { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"commit-box" }, luil::stack_config { .spacing = 6.0f, .padding = luil::edge_insets::all(10.0f) }) };
            auto subject_row
                = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"commit-subject-row" }, luil::stack_config { .direction = luil::stack_direction::row, .spacing = 6.0f });
            subject_row->add_flexible(std::make_unique<luil::text_input_element>(
                luil::ui_element_id { kind_commit_subject_input, u8"commit-subject" }, working.subject, luil::text_input_config { .placeholder = u8"제목 (요약)" }));
            const std::size_t length { working.subject_length };
            // 제목 50자를 넘으면 경고색, 72자를 넘으면 오류색이다.
            text_style count_style { .size = 11.0f, .align = 2 };
            count_style.color = [length](const luil::ui_color_palette& palette) {
                if (length > 72)
                    return palette.error_accent;
                return length > 50 ? palette.warning_accent : palette.secondary_foreground;
            };
            subject_row->add(make_text(u8"commit-subject-count", number_text(length) + u8"/72", std::move(count_style)), 44.0f);
            box->add(std::move(subject_row), 28.0f);
            box->add(std::make_unique<luil::text_input_element>(
                         luil::ui_element_id { kind_commit_body_input, u8"commit-body" }, working.body, luil::text_input_config { .placeholder = u8"본문 (선택) — 무엇을 왜 바꿨는지" }),
                28.0f);
            luil::check_config amend { .owner = u8"commit-amend", .label = u8"직전 커밋 수정(amend)", .checked = working.amend, .toggle = action_of(message(toggle_amend_intent {})) };
            box->add(std::make_unique<luil::check_element>(std::move(amend)), luil::check_row_height);
            if (working.amend_warning)
            {
                const color_selector warning { [](const luil::ui_color_palette& palette) { return palette.warning_accent; } };
                box->add(make_text(u8"commit-amend-warning", u8"이미 push한 커밋입니다.", text_style { .size = 11.0f, .color = warning, .bold = true }), 16.0f);
                box->add(make_text(u8"commit-amend-warning-detail", u8"고쳐 쓰면 강제 push(--force-with-lease)가 필요합니다.", text_style { .size = 11.0f, .color = warning }), 16.0f);
            }
            box->add(
                make_text_button(u8"commit-button", working.commit_label, luil::text_button_visual::accent, message(commit_intent {}), working.can_commit, codicons::icon_git_commit, true), 30.0f);
            box->add(make_text(u8"commit-blocked", working.blocked_reason, text_style { .size = 11.0f, .color = dim_color }), 16.0f);
            column->add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"commit-box-bar" }, bar_style { .divider_top = true }, std::move(box)),
                commit_box_height + (working.amend_warning ? amend_warning_extra : 0.0f));
            return column;
        }
    } // namespace

    std::unique_ptr<luil::ui_element> build_sidebar(const repository_view& repository, feedback_sources& sources)
    {
        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"sidebar" }, luil::stack_config {}) };
        auto header = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"sidebar-header" },
            luil::stack_config { .direction = luil::stack_direction::row, .spacing = 6.0f, .padding = luil::edge_insets { 10.0f, 0.0f, 8.0f, 0.0f } });
        header->add(std::make_unique<icon_element>(luil::ui_element_id { kind_label, u8"sidebar-repo-icon" }, codicons::icon_repo, color_selector {}), 18.0f);
        header->add_flexible(make_text(u8"sidebar-repo-name", repository.display_name, text_style { .size = 12.0f, .bold = true }));
        column->add(std::move(header), 32.0f);

        auto filter_row
            = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"sidebar-filter-row" }, luil::stack_config { .padding = luil::edge_insets { 8.0f, 2.0f, 8.0f, 6.0f } });
        luil::text_input_config filter_config {
            .placeholder = u8"브랜치·태그 거르기",
            .leading_glyph = codicons::icon_search,
            .clear = action_of(message(edit_text_intent { luil::text_edit_request { .target = to_luil_target(text_target::sidebar_filter), .command = luil::text::text_edit_command::clear } })),
            .clear_tooltip = u8"지우기",
        };
        filter_row->add_flexible(
            std::make_unique<luil::text_input_element>(luil::ui_element_id { kind_sidebar_filter_input, u8"sidebar-filter" }, repository.sidebar_filter, std::move(filter_config)));
        column->add(std::move(filter_row), filter_row_height);

        auto list { make_sidebar_list(u8"sidebar-list", repository.sidebar, repository.selected_sidebar_key, repository.sidebar_scroll, true) };
        sources.sidebar = static_cast<const luil::virtual_list_element*>(list.get());
        column->add_flexible(std::move(list));
        return std::make_unique<bar_element>(
            luil::ui_element_id { kind_panel, u8"sidebar-panel" }, bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.window_background; } }, std::move(column));
    }

    std::unique_ptr<luil::ui_element> build_rail(const repository_view& repository)
    {
        auto column
            = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"rail" }, luil::stack_config { .spacing = 2.0f, .padding = luil::edge_insets { 2.0f, 6.0f, 2.0f, 6.0f } });
        for (const sidebar_section section :
            { sidebar_section::local, sidebar_section::remote, sidebar_section::tags, sidebar_section::stashes, sidebar_section::submodules, sidebar_section::worktrees })
        {
            const std::u8string title { section_title(section) };
            const std::u8string count { number_text(repository.section_counts[static_cast<std::size_t>(section)]) };
            column->add(
                std::make_unique<tool_button_element>(u8"rail-" + title, section_icon(section), count, title + u8" " + count + u8"개", true, true, message(open_rail_section_intent { section })),
                44.0f);
        }
        return std::make_unique<bar_element>(
            luil::ui_element_id { kind_panel, u8"rail-panel" }, bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.window_background; } }, std::move(column));
    }

    std::unique_ptr<luil::ui_element> build_rail_popup(const repository_view& repository, const sidebar_section section)
    {
        std::vector<sidebar_row_view> rows {};
        for (const sidebar_row_view& row : repository.sidebar)
            if (row.section == section && row.header == false)
                rows.push_back(row);
        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"rail-popup-column" }, luil::stack_config {}) };
        auto header = std::make_unique<luil::stack_element>(
            luil::ui_element_id { kind_layout, u8"rail-popup-header" }, luil::stack_config { .direction = luil::stack_direction::row, .padding = luil::edge_insets { 10.0f, 0.0f, 4.0f, 0.0f } });
        header->add_flexible(make_text(u8"rail-popup-title", section_title(section) + u8"  " + number_text(rows.size()), text_style { .size = 11.0f, .color = dim_color, .bold = true }));
        header->add(make_icon_button(u8"rail-popup-close", codicons::icon_close, u8"닫기", message(open_rail_section_intent { section })), 28.0f);
        column->add(std::move(header), 30.0f);
        if (rows.empty())
            column->add_flexible(centered_message(u8"rail-popup-empty", u8"항목이 없습니다."));
        else
            column->add_flexible(make_sidebar_list(u8"rail-popup-list", rows, repository.selected_sidebar_key, 0.0f, false));
        auto panel = std::make_unique<luil::panel_element>(luil::ui_element_id { kind_panel, u8"rail-popup" }, floating_panel_config(6.0f, 0.35f));
        panel->set_content(std::move(column));
        panel->set_hit_opaque(true);
        return panel;
    }

    std::unique_ptr<luil::ui_element> build_graph_area(const view_snapshot& view, const repository_view& repository, feedback_sources& sources)
    {
        if (repository.diff.has_value())
            return build_diff_area(view, repository, sources);

        // rebase·merge·cherry-pick·revert가 멈춰 있으면 그래프 자리를 닫을 수 없는 충돌 해결 화면이 덮는다.
        if (repository.conflict.has_value())
            return build_conflict_screen(repository, *repository.conflict, repository.conflict_scroll, sources);

        const graph_view& graph { repository.graph };
        const graph_columns columns { make_graph_columns(graph.lane_count, graph.compact_columns) };
        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"graph-area" }, luil::stack_config {}) };
        column->add(std::make_unique<graph_header_element>(columns), 26.0f);
        if (repository.loaded == false || graph.items == nullptr || graph.items->empty())
        {
            if (repository.load_error.empty() == false)
                column->add_flexible(
                    centered_message(u8"graph-error", u8"저장소를 읽지 못했습니다: " + repository.load_error, [](const luil::ui_color_palette& palette) { return palette.error_accent; }));
            else
                column->add_flexible(centered_message(u8"graph-loading", repository.loaded ? std::u8string { u8"커밋이 없습니다." } : std::u8string { u8"저장소를 읽는 중…" }));
            return surface(u8"graph-panel", std::move(column));
        }

        luil::virtual_list_config config {};
        config.owner = u8"graph";
        // 모델은 cache된 벡터의 복사다. 키가 15자라 짧은 문자열 최적화 안에 들어 할당 없이 끝난다.
        config.items = *graph.items;
        config.selected = graph.selected_key;
        config.cursor = graph.selected_key;
        config.row_height = graph_row_height;
        config.scroll_offset = graph.scroll;
        config.overscan = 4;
        const std::vector<graph_row>* const rows { graph.rows };
        const std::vector<commit_info>* const commits { graph.commits };
        const std::shared_ptr<const ref_label_map> labels { graph.labels };
        const std::uint16_t wip_lane { graph.has_wip && rows->empty() == false ? rows->front().lane : std::uint16_t { 0 } };
        // 세션의 행을 가리키는 포인터는 이 tree를 짓는 동안(`make_arranged_tree`)만 쓰인다. 행 element는 값을 복사해 간다.
        config.build_row = [rows, commits, labels, columns, wip_lane, graph_state = graph](const luil::virtual_list_item&, const std::size_t index, bool) -> std::unique_ptr<luil::ui_element> {
            if (index >= rows->size() || index >= commits->size())
                return nullptr;
            const commit_info& commit { (*commits)[index] };
            graph_row_data data {};
            data.row = (*rows)[index];
            data.index = index;
            data.wip = graph_state.has_wip && index == 0;
            data.wip_count = graph_state.wip_count;
            data.has_wip = graph_state.has_wip;
            data.head_row_index = graph_state.head_row_index;
            data.wip_lane = wip_lane;
            data.head = commit.sha == graph_state.head_sha;
            data.row_key = graph_item_key(commit.sha);
            data.sha = commit.sha;
            if (data.wip == false)
            {
                data.subject = to_u8(commit.subject);
                data.author = to_u8(commit.author_name);
                data.date = relative_time_text(graph_state.now, commit.author_time);
                if (labels != nullptr)
                    if (const auto found { labels->find(commit.sha) }; found != labels->end())
                        data.labels = found->second;
            }
            return std::make_unique<graph_row_element>(u8"commit-" + graph_item_key(commit.sha), std::move(data), columns);
        };
        const auto select = [](const std::u8string& key) { return message(select_commit_intent { key }); };
        config.select = select;
        config.move = select;
        config.scroll = [](const float delta) { return message(scroll_intent { .target = scroll_target::graph, .value = delta }); };
        config.scroll_to = [](const float offset) { return message(scroll_intent { .target = scroll_target::graph, .value = offset, .absolute = true }); };
        auto list { std::make_unique<luil::virtual_list_element>(std::move(config)) };
        list->set_access_name(u8"커밋 그래프 " + number_text(graph.items->size()) + u8"행");
        sources.graph = list.get();
        column->add_flexible(std::move(list));
        static_cast<void>(view);
        return surface(u8"graph-panel", std::move(column));
    }

    std::unique_ptr<luil::ui_element> build_detail(const view_snapshot& view, const repository_view& repository, const bool drawer, feedback_sources& sources)
    {
        static_cast<void>(view);
        std::unique_ptr<luil::ui_element> content {};
        if (repository.detail == detail_mode::commit && repository.commit.has_value())
            content = build_commit_detail(repository, *repository.commit, drawer, sources);
        else if (repository.detail == detail_mode::working && repository.working.has_value())
            content = build_working_area(repository, *repository.working, drawer, sources);
        else
            content = centered_message(u8"detail-empty", u8"커밋이나 WIP 행을 고르면 여기에 상세가 보입니다.");
        return std::make_unique<bar_element>(
            luil::ui_element_id { kind_panel, u8"detail-panel" }, bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.surface_background; } }, std::move(content));
    }
} // namespace cyella::ui
