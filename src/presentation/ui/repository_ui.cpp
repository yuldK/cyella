#include "application/app_messages.h"
#include "domain/text_catalog.h"
#include "presentation/build_view_snapshot.h"
#include "presentation/dialog_intents.h"
#include "presentation/extension_intents.h"
#include "presentation/rebase/rebase_editor_intents.h"
#include "presentation/signing_intents.h"
#include "presentation/text_wrap.h"
#include "presentation/ui/diff_webview.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/graph_row_element.h"
#include "presentation/ui/hosting_ui.h"
#include "presentation/ui/list_rows.h"
#include "presentation/ui/rebase/multiline_text_element.h"
#include "presentation/ui/segmented_control.h"
#include "presentation/ui/sidebar_extension_rows.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui/ui_parts.h"
#include "presentation/ui_intents.h"

#include "luil/ui/check_element.h"
#include "luil/ui/dialog_elements.h"
#include "luil/ui/list_element.h"
#include "luil/ui/panel_element.h"
#include "luil/ui/scroll_area_element.h"
#include "luil/ui/stack_element.h"
#include "luil/ui/virtual_list_element.h"

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <utility>

namespace cyella::ui {
    namespace {
        constexpr float pane_header_height { 36.0f };
        constexpr float filter_row_height { 36.0f };
        // 커밋 상세의 치수다. 라벨 칸은 "부모 (병합)"이 들어가는 폭이다.
        constexpr float detail_padding { 14.0f };
        constexpr float detail_label_width { 64.0f };
        constexpr float detail_row_height { 22.0f };
        constexpr float subject_line_height { 20.0f };
        constexpr float body_line_height { 18.0f };
        constexpr float files_header_height { 30.0f };
        // 커밋 메시지 카드의 칸 안 여백과 바닥줄(작성자·시각) 높이다.
        constexpr float message_card_padding { 10.0f };
        constexpr float message_footer_height { 30.0f };
        // 메시지·메타 칸이 판 높이에서 가질 수 있는 몫이다. 넘치면 그 칸만 흘리고 파일 목록 자리를 지킨다.
        constexpr float commit_meta_share { 0.55f };
        constexpr float commit_meta_minimum { 160.0f };
        constexpr float commit_box_minimum { 280.0f };
        constexpr float commit_box_maximum { 360.0f };
        constexpr float amend_warning_extra { 44.0f };
        // 커밋 상자에서 amend 토글 옆에 서는 서명 토글의 폭이다.
        constexpr float sign_toggle_width { 76.0f };

        const color_selector dim_color { [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } };

        // 서명 배지의 색이다.
        color_selector signature_color(const signature_tone tone)
        {
            return [tone](const luil::ui_color_palette& palette) {
                switch (tone)
                {
                case signature_tone::good:
                    return success_color(palette);
                case signature_tone::warning:
                    return palette.warning_accent;
                case signature_tone::bad:
                    return palette.error_accent;
                case signature_tone::neutral:
                    break;
                }
                return palette.secondary_foreground;
            };
        }

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
            case sidebar_item_kind::pull_request:
                return std::nullopt;
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

        std::u8string section_title(sidebar_section section);

        std::unique_ptr<luil::ui_element> make_sidebar_list(std::u8string owner, const std::vector<sidebar_row_view>& rows, const std::u8string& selected, const float scroll, const bool scrolls)
        {
            auto index { std::make_shared<sidebar_index>() };
            luil::virtual_list_config config {};
            config.owner = std::move(owner);
            for (const sidebar_row_view& row : rows)
            {
                sidebar_row_view shown { row };
                if (shown.header)
                    shown.label = section_title(shown.section);
                config.items.push_back({ .key = shown.key, .label = shown.label, .height = shown.header ? sidebar_header_row_height : sidebar_row_height });
                index->rows.emplace(row.key, std::move(shown));
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
            list->set_access_name(tr(u8"repo.sidebar.list_access_name"));
            return list;
        }

        std::u8string section_title(const sidebar_section section)
        {
            switch (section)
            {
            case sidebar_section::local:
                return tr(u8"repo.section.local");
            case sidebar_section::remote:
                return tr(u8"repo.section.remote");
            case sidebar_section::tags:
                return tr(u8"repo.section.tags");
            case sidebar_section::stashes:
                return tr(u8"repo.section.stashes");
            case sidebar_section::submodules:
                return tr(u8"repo.section.submodules");
            case sidebar_section::worktrees:
                return tr(u8"repo.section.worktrees");
            case sidebar_section::lfs:
                return u8"GIT LFS";
            case sidebar_section::pull_requests:
                return u8"PR · MR";
            }
            return {};
        }

        // 레일 단추의 UI Automation owner 이름이다. 화면 언어에 따라 바뀌지 않도록 처음의 한국어 구역 제목을 그대로 고정해 둔다.
        std::u8string_view section_owner(const sidebar_section section) noexcept
        {
            switch (section)
            {
            case sidebar_section::local:
                return u8"로컬 브랜치";
            case sidebar_section::remote:
                return u8"원격 브랜치";
            case sidebar_section::tags:
                return u8"태그";
            case sidebar_section::stashes:
                return u8"스태시";
            case sidebar_section::submodules:
                return u8"서브모듈";
            case sidebar_section::worktrees:
                return u8"워크트리";
            case sidebar_section::lfs:
                return u8"GIT LFS";
            case sidebar_section::pull_requests:
                return u8"PR · MR";
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
            case sidebar_section::pull_requests:
                return extension_section_glyph(section);
            }
            return codicons::icon_git_branch;
        }

        // ---------------------------------------------------------------- 상세

        // 상세 판의 머리다. `tail`은 제목 옆의 흐린 글이고 `code`면 고정폭이다 (커밋 SHA). `extra`는 닫기 단추 앞에 서는 단추다.
        std::unique_ptr<luil::ui_element> detail_header(std::u8string title, std::u8string tail, const bool drawer, const bool code = false, std::unique_ptr<luil::ui_element> extra = nullptr)
        {
            auto row = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"detail-header" },
                luil::stack_config { .direction = luil::stack_direction::row, .spacing = 6.0f, .padding = luil::edge_insets { 14.0f, 0.0f, 6.0f, 0.0f } });
            row->add(make_text(u8"detail-title", title, text_style { .size = 13.0f, .bold = true }), text_button_width(title));
            row->add_flexible(make_text(u8"detail-subtitle", std::move(tail), text_style { .size = code ? 12.0f : 11.0f, .color = dim_color, .code = code }));
            if (extra != nullptr)
                row->add(std::move(extra), 28.0f);
            if (drawer)
                row->add(make_icon_button(u8"detail-close", codicons::icon_close, tr(u8"repo.detail.close_drawer"), message(toggle_drawer_intent { false })), 28.0f);
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"detail-header-bar" }, bar_style { .divider_bottom = true }, std::move(row));
        }

        // 커밋 상세의 라벨·값 한 줄이다. 라벨은 흐린 고정 폭 칸이고 값이 나머지를 갖는다.
        std::unique_ptr<luil::ui_element> detail_field(const std::u8string& owner, std::u8string label, std::unique_ptr<luil::ui_element> value)
        {
            auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, owner + u8"-row" }, luil::stack_config { .direction = luil::stack_direction::row }) };
            row->add(make_text(owner + u8"-label", std::move(label), text_style { .size = 11.0f, .color = dim_color }), detail_label_width);
            row->add_flexible(std::move(value));
            return row;
        }

        // 커밋 메시지 카드다: 제목 머리 · 본문 · 작성 정보 바닥이 한 테두리 안에 선다.
        //  - 커밋 메시지는 제목과 본문으로 나뉜 한 글이라 한 카드에 둔다. 바닥줄은 "누가 언제 썼나"다: 왼쪽에 작성자(사진·이름, 올리면 메일),
        //    오른쪽 끝에 시각.
        //  - `width`는 카드 바깥 폭이다 (논리 픽셀). 줄 나누기 짐작은 테두리와 칸 여백을 뺀 폭으로 한다.
        std::unique_ptr<luil::ui_element> build_message_card(const commit_detail_view& commit, const float width, float& height)
        {
            const float text_width { std::max(width - 2.0f - message_card_padding * 2.0f, 0.0f) };
            std::vector<card_element::section> sections {};

            const std::u8string subject { commit.subject.empty() ? std::u8string { u8"…" } : commit.subject };
            const text_style subject_style { .size = 13.5f, .bold = true };
            const float subject_height { std::max(wrapped_text_height(subject, text_width, subject_style, subject_line_height), subject_line_height) };
            {
                auto head = std::make_unique<luil::stack_element>(
                    luil::ui_element_id { kind_layout, u8"detail-subject-cell" }, luil::stack_config { .padding = luil::edge_insets { message_card_padding, 7.0f, message_card_padding, 7.0f } });
                head->add(make_wrapped_text(u8"detail-subject", subject, subject_style, subject_line_height), subject_height);
                sections.push_back({ .content = std::move(head), .height = subject_height + 14.0f, .tinted = true });
            }

            std::u8string_view body { commit.body };
            while (body.empty() == false && (body.back() == u8'\n' || body.back() == u8'\r' || body.back() == u8' '))
                body.remove_suffix(1);
            if (body.empty() == false)
            {
                const text_style body_style { .size = 12.0f, .color = dim_color };
                const float body_height { wrapped_text_height(body, text_width, body_style, body_line_height) };
                auto cell = std::make_unique<luil::stack_element>(
                    luil::ui_element_id { kind_layout, u8"detail-body-cell" }, luil::stack_config { .padding = luil::edge_insets { message_card_padding, 8.0f, message_card_padding, 8.0f } });
                cell->add(make_wrapped_text(u8"detail-body", std::u8string { body }, body_style, body_line_height), body_height);
                sections.push_back({ .content = std::move(cell), .height = body_height + 16.0f, .tinted = false });
            }

            // 바닥줄: 작성자는 왼쪽, 시각은 오른쪽 끝이다. 폭이 모자라면 상대 시각부터 빼고(절대 시각의 툴팁에 남는다) 그다음 이름을 줄인다.
            {
                auto foot = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"detail-message-footer" },
                    luil::stack_config { .direction = luil::stack_direction::row, .spacing = 6.0f, .padding = luil::edge_insets { message_card_padding, 0.0f, message_card_padding, 0.0f } });
                const std::u8string relative { commit.date.empty() ? std::u8string {} : u8"· " + commit.date };
                const float when_width { estimate_text_width(commit.date_absolute, 11.0f, false) + 2.0f };
                float relative_width { relative.empty() ? 0.0f : estimate_text_width(relative, 11.0f, false) + 2.0f };
                float author_width { author_element::width_for(commit.author_name) };
                if (author_width + when_width + relative_width + 18.0f > text_width)
                    relative_width = 0.0f;
                author_width = std::max(std::min(author_width, text_width - when_width - relative_width - 18.0f), 40.0f);
                foot->add(std::make_unique<author_element>(u8"detail-author", commit.author_name, commit.author_email, commit.author_avatar), author_width);
                foot->add_flexible_gap();
                auto when { make_text(u8"detail-date", commit.date_absolute, text_style { .size = 11.0f, .align = 2 }) };
                if (relative_width <= 0.0f && commit.date.empty() == false)
                    when->set_tooltip(commit.date_absolute + u8" · " + commit.date);
                foot->add(std::move(when), when_width);
                if (relative_width > 0.0f)
                    foot->add(make_text(u8"detail-date-relative", relative, text_style { .size = 11.0f, .color = dim_color, .align = 2 }), relative_width);
                sections.push_back({ .content = std::move(foot), .height = message_footer_height, .tinted = true });
            }

            height = card_element::height_for(sections);
            return std::make_unique<card_element>(u8"commit-message-card", tr(u8"repo.commit_message"), std::move(sections));
        }

        // [이후 커밋 N개 편집…]이다. 커밋 우클릭 메뉴의 "여기서부터 인터랙티브 rebase…"와 같은 일이고, 판 폭 전체로 서서 글이 잘리지 않는다.
        std::unique_ptr<luil::ui_element> build_rebase_button(const repository_view& repository, const commit_detail_view& commit)
        {
            const std::uint32_t above { commit.commits_above.value_or(0) };
            std::u8string label { tr(u8"repo.rebase.edit_later") };
            std::u8string tooltip {};
            if (above > 0 && commit.on_current_branch)
            {
                label = above == 1 ? tr(u8"repo.rebase.edit_later_count_one", { { u8"count", above } }) : tr(u8"repo.rebase.edit_later_count", { { u8"count", above } });
                tooltip = tr(u8"repo.rebase.edit_later_tooltip", { { u8"count", above } });
            }
            else if (above > 0)
            {
                // 다른 갈래의 커밋이다. 현재 브랜치에만 있는 커밋을 이 커밋 위로 옮겨 쌓으며 편집한다.
                label = tr(u8"repo.rebase.edit_current", { { u8"count", above } });
                tooltip = tr(u8"repo.rebase.edit_current_tooltip", { { u8"count", above } });
            }
            else if (commit.sha == repository.graph.head_sha)
                tooltip = tr(u8"repo.rebase.head_tooltip");
            else
                tooltip = tr(u8"repo.rebase.nothing_tooltip");
            auto button = make_text_button(
                u8"detail-interactive-rebase", label, luil::text_button_visual::normal, message(open_interactive_rebase_here_intent { commit.sha }), above > 0, codicons::icon_list_ordered);
            button->set_tooltip(std::move(tooltip));
            return button;
        }

        std::unique_ptr<luil::ui_element> build_commit_meta(const repository_view& repository, const commit_detail_view& commit, const float text_width, float& height)
        {
            auto meta = std::make_unique<luil::stack_element>(
                luil::ui_element_id { kind_layout, u8"commit-meta" }, luil::stack_config { .padding = luil::edge_insets { detail_padding, 12.0f, detail_padding, 12.0f } });
            height = 24.0f;
            const auto add = [&meta, &height](std::unique_ptr<luil::ui_element> child, const float length) {
                height += length;
                meta->add(std::move(child), length);
            };
            const auto gap = [&meta, &height](const float length) {
                height += length;
                meta->add_gap(length);
            };

            // 메시지 카드 → 라벨 → 라벨·값 표 → [이후 커밋 편집…] 순서다.
            float card_height { 0.0f };
            auto card { build_message_card(commit, text_width, card_height) };
            add(std::move(card), card_height);
            if (commit.labels.empty() == false)
            {
                gap(10.0f);
                add(std::make_unique<ref_label_strip_element>(u8"detail-labels", commit.labels, commit.lane_color), 22.0f);
            }
            gap(10.0f);

            const float value_width { std::max(text_width - detail_label_width, 0.0f) };
            add(detail_field(u8"detail-sha", u8"SHA", make_text(u8"detail-sha", to_u8(commit.sha), text_style { .size = 11.5f, .code = true })), detail_row_height);
            // 부모는 누르면 그 커밋으로 간다.
            {
                auto parents
                    = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"detail-parents" }, luil::stack_config { .direction = luil::stack_direction::row, .spacing = 10.0f });
                if (commit.parent_shas.empty())
                    parents->add_flexible(make_text(u8"detail-parents-none", tr(u8"repo.detail.no_parents"), text_style { .size = 12.0f, .color = dim_color }));
                for (std::size_t index { 0 }; index < commit.parent_shas.size() && index < commit.parents.size(); ++index)
                {
                    auto link = make_text_button(
                        u8"detail-parent-" + number_text(index), commit.parents[index], luil::text_button_visual::link, message(select_commit_intent { graph_item_key(commit.parent_shas[index]) }));
                    link->set_tooltip(tr(u8"repo.detail.go_to_parent", { { u8"sha", commit.parent_shas[index] } }));
                    parents->add(std::move(link), { .length = estimate_text_width(commit.parents[index], 12.0f, false) + 8.0f, .cross_length = 18.0f });
                }
                if (commit.parent_shas.empty() == false)
                    parents->add_flexible_gap();
                add(detail_field(u8"detail-parents", commit.parent_shas.size() > 1 ? tr(u8"repo.detail.parents_merge") : tr(u8"repo.detail.parent"), std::move(parents)), detail_row_height);
            }
            // 서명 배지다. 서명 검증은 파일 목록과 따로 읽는다.
            {
                const signature_view& signature { commit.signature };
                add(detail_field(u8"detail-signature", tr(u8"repo.detail.signature"),
                        make_text(u8"detail-signature", signature.badge,
                            text_style { .size = 12.0f, .color = signature_color(signature.tone), .bold = signature.known && signature.status != signature_status::none })),
                    detail_row_height);
                if (signature.signer.empty() == false)
                    add(detail_field(u8"detail-signature-signer", tr(u8"repo.detail.signer"), make_text(u8"detail-signature-signer", signature.signer, text_style { .size = 12.0f })),
                        detail_row_height);
                if (signature.key.empty() == false)
                    add(detail_field(u8"detail-signature-key", tr(u8"repo.detail.signature_key"), make_text(u8"detail-signature-key", signature.key, text_style { .size = 11.5f, .code = true })),
                        detail_row_height);
                if (signature.description.empty() == false)
                {
                    // 설명은 값 칸에서 줄을 바꾼다 (allowed signers 안내처럼 길다).
                    const text_style style { .size = 11.0f, .color = dim_color };
                    const float lines { std::max(wrapped_text_height(signature.description, value_width, style, 16.0f), 16.0f) };
                    add(detail_field(u8"detail-signature-detail", {}, make_wrapped_text(u8"detail-signature-detail", signature.description, style, 16.0f)), lines + 2.0f);
                }
            }
            // 로그인한 GitHub·GitLab 호스트의 CI 상태다.
            if (commit.ci.has_value())
                add(detail_field(u8"detail-ci", u8"CI", build_commit_ci_line(*commit.ci)), detail_row_height);
            gap(10.0f);
            add(build_rebase_button(repository, commit), 28.0f);
            return meta;
        }

        // 변경 파일 머리다: "변경 파일 N개 +a −r"와 목록·트리 전환 단추.
        std::unique_ptr<luil::ui_element> build_files_header(const commit_detail_view& commit)
        {
            auto row = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"detail-files-header" },
                luil::stack_config { .direction = luil::stack_direction::row, .spacing = 6.0f, .padding = luil::edge_insets { detail_padding, 0.0f, 6.0f, 0.0f } });
            const std::u8string title { tr(u8"repo.detail.changed_files", { { u8"count", commit.files.size() } }) };
            row->add(make_text(u8"detail-files-title", title, text_style { .size = 11.0f, .color = dim_color, .bold = true }), estimate_text_width(title, 11.0f, true) + 4.0f);
            if (commit.added_total.empty() == false)
                row->add(make_text(u8"detail-files-added", commit.added_total, text_style { .size = 11.0f, .color = [](const luil::ui_color_palette& palette) { return success_color(palette); } }),
                    estimate_text_width(commit.added_total, 11.0f, false) + 2.0f);
            if (commit.removed_total.empty() == false)
                row->add(make_text(u8"detail-files-removed", commit.removed_total, text_style { .size = 11.0f, .color = [](const luil::ui_color_palette& palette) { return palette.error_accent; } }),
                    estimate_text_width(commit.removed_total, 11.0f, false) + 2.0f);
            row->add_flexible_gap();
            const bool tree { commit.files_tree };
            row->add(make_file_view_control(u8"detail-files", tree, [](const std::u8string& value) { return message(set_commit_files_view_intent { value == u8"tree" }); }),
                { .length = 60.0f, .cross_length = segmented_control_height });
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"detail-files-header-bar" }, bar_style { .divider_bottom = true, .divider_top = true }, std::move(row));
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
            list->set_access_name(tr(u8"repo.detail.file_list"));
            sources.detail = list.get();
            return list;
        }

        std::unique_ptr<luil::ui_element> build_commit_detail(
            const repository_view& repository, const commit_detail_view& commit, const bool drawer, const float width, const float height, feedback_sources& sources)
        {
            auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"commit-detail" }, luil::stack_config {}) };
            auto copy { make_icon_button(u8"detail-copy-sha", codicons::icon_copy, tr(u8"repo.detail.copy_sha"), message(copy_commit_sha_intent { commit.sha })) };
            column->add(detail_header(tr(u8"repo.detail.commit_title"), commit.short_sha, drawer, true, std::move(copy)), pane_header_height);

            // 메시지·메타 칸은 내용만큼 서되, 판 높이의 일정 몫을 넘으면 그 칸만 흘린다. 흘릴 때는 막대 폭만큼 글이 좁아진다.
            const float text_width { std::max(width - detail_padding * 2.0f, 0.0f) };
            const float limit { std::max(height * commit_meta_share, commit_meta_minimum) };
            float meta_height { 0.0f };
            auto meta { build_commit_meta(repository, commit, text_width, meta_height) };
            if (meta_height > limit)
            {
                meta = build_commit_meta(repository, commit, std::max(text_width - luil::scroll_area_bar_width, 0.0f), meta_height);
                luil::scroll_area_config config {};
                config.owner = u8"commit-meta-scroll";
                config.content_height = meta_height;
                config.scroll_offset = repository.commit_meta_scroll;
                config.scroll = [](const float delta) { return message(scroll_intent { .target = scroll_target::commit_meta, .value = delta }); };
                config.scroll_to = [](const float offset) { return message(scroll_intent { .target = scroll_target::commit_meta, .value = offset, .absolute = true }); };
                config.bar = luil::scrollbar_visibility::always;
                config.edges = luil::scroll_edges { .shadows = true };
                auto area { std::make_unique<luil::scroll_area_element>(std::move(config)) };
                area->set_content(std::move(meta));
                sources.commit_meta = area.get();
                column->add(std::move(area), limit);
            }
            else
                column->add(std::move(meta), meta_height);

            if (commit.loading)
            {
                column->add_flexible(centered_message(u8"detail-loading", tr(u8"repo.detail.loading_files")));
                return column;
            }
            column->add(build_files_header(commit), files_header_height);
            auto builders { std::make_shared<std::map<std::u8string, std::function<std::unique_ptr<luil::ui_element>()>>>() };
            auto requests { std::make_shared<std::map<std::u8string, diff_file_request>>() };
            std::vector<luil::virtual_list_item> items {};
            std::u8string selected {};
            const auto add_file = [&](const file_row_view& row, const std::optional<std::size_t> depth) {
                items.push_back({ .key = row.key, .label = row.name });
                const diff_file_request request { .path = row.path, .view = diff_view::commit, .sha = commit.sha, .untracked = false };
                // 파일 행은 경로 툴팁이 있어 포인터의 hit를 직접 받는다. 고르기 메시지를 행이 다시 내지 않으면 마우스 누름이 목록까지
                // 가지 못해 diff가 열리지 않는다 (키보드·보조 기술은 목록으로 바로 가므로 열린다). 작업 영역·그래프 행과 같은 이유다.
                const luil::input_action select { message(select_file_intent { request }) };
                // 우클릭 메뉴는 변경 내용 보기와 경로·이름·`커밋:경로` 복사다.
                const menu_target target { .kind = menu_target_kind::commit_file, .name = row.path, .sha = commit.sha };
                file_row_view shown { row };
                // 트리에서는 폴더가 이미 보이므로 이름 바꾸기(← 옛 경로)만 남긴다.
                if (depth.has_value() && shown.original_path.empty())
                    shown.folder.clear();
                (*builders)[row.key] = [shown, select, target, depth] { return std::make_unique<file_row_element>(shown, std::vector<file_row_button> {}, select, target, depth); };
                (*requests)[row.key] = request;
                if (row.selected)
                    selected = row.key;
            };
            if (commit.files_tree)
                for (const file_tree_row& node : commit.tree)
                {
                    if (node.folder == false)
                    {
                        if (node.file_index < commit.files.size())
                            add_file(commit.files[node.file_index], node.depth);
                        continue;
                    }
                    // 폴더 행은 키보드 훑기가 건너뛴다 (고를 것이 아니다). 누르면 접거나 편다.
                    items.push_back({ .key = node.key, .label = node.label, .enabled = false });
                    (*builders)[node.key] = [node] { return std::make_unique<folder_row_element>(node, message(toggle_file_folder_intent { node.path })); };
                }
            else
                for (const file_row_view& row : commit.files)
                    add_file(row, std::nullopt);
            column->add_flexible(make_file_list(u8"commit-files", std::move(items), std::move(builders), std::move(requests), std::move(selected), repository.detail_scroll, sources));
            return column;
        }

        std::unique_ptr<luil::ui_element> build_working_area(const repository_view& repository, const working_area_view& working, const bool drawer, const float height, feedback_sources& sources)
        {
            // 작은 높이에서 고정 커밋 상자가 파일 목록과 확인 단추를 창 밖으로 밀지 않게 한다.
            // 평소에는 기존 큰 작성 영역을 유지하고, 모자랄 때만 전체 영역에 스크롤을 제공한다.
            const float minimum_height { 40.0f + 150.0f + commit_box_minimum + (working.amend_warning ? amend_warning_extra : 0.0f) };
            if (height < minimum_height)
            {
                luil::scroll_area_config config {};
                config.owner = u8"working-scroll";
                config.content_height = minimum_height;
                config.scroll_offset = working.scroll;
                config.scroll = [](const float delta) { return message(scroll_intent { .target = scroll_target::working_area, .value = delta }); };
                config.scroll_to = [](const float offset) { return message(scroll_intent { .target = scroll_target::working_area, .value = offset, .absolute = true }); };
                config.bar = luil::scrollbar_visibility::always;
                config.edges = luil::scroll_edges { .shadows = true };
                auto area { std::make_unique<luil::scroll_area_element>(std::move(config)) };
                area->set_access_name(tr(u8"repo.working.access_name"));
                area->set_content(build_working_area(repository, working, drawer, minimum_height, sources));
                sources.working = area.get();
                return area;
            }
            const std::string& repo { repository.repository_id };
            auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"working-area" }, luil::stack_config {}) };
            luil::stack_config view_config { .direction = luil::stack_direction::row, .padding = luil::edge_insets { 10.0f, 4.0f, 10.0f, 4.0f } };
            auto view_row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"working-files-header" }, view_config) };
            view_row->add_flexible(make_text(u8"working-files-title", tr(u8"repo.working.files_title", { { u8"count", repository.graph.wip_count } }), text_style { .size = 12.0f, .bold = true }));
            view_row->add(make_file_view_control(u8"working-files-view", working.files_tree, [](const std::u8string& value) { return message(set_working_files_view_intent { value == u8"tree" }); }),
                { .length = 60.0f, .cross_length = segmented_control_height });
            if (drawer)
                view_row->add(make_icon_button(u8"detail-close", codicons::icon_close, tr(u8"repo.working.close_detail_panel"), message(toggle_drawer_intent { false })), 28.0f);
            column->add(std::move(view_row), 40.0f);

            auto builders { std::make_shared<std::map<std::u8string, std::function<std::unique_ptr<luil::ui_element>()>>>() };
            auto requests { std::make_shared<std::map<std::u8string, diff_file_request>>() };
            std::vector<luil::virtual_list_item> items {};
            std::u8string selected {};

            const auto add_group = [&](std::u8string key, std::u8string title, const std::vector<file_row_view>& rows, std::optional<file_row_button> button) {
                items.push_back({ .key = key, .label = title, .height = 28.0f, .enabled = false });
                (*builders)[key] = [key, title, count = rows.size(), button]() mutable { return std::make_unique<file_group_element>(u8"group-" + key, title, count, std::move(button)); };
            };
            const auto add_rows = [&](const std::vector<file_row_view>& rows, const diff_view view) {
                const auto add_file = [&](const file_row_view& row, const std::optional<std::size_t> depth) {
                    items.push_back({ .key = row.key, .label = row.name });
                    std::vector<file_row_button> buttons {};
                    if (view == diff_view::unstaged && row.conflicted == false)
                    {
                        buttons.push_back(make_row_button(u8"stage:" + to_u8(row.path), codicons::icon_add, tr(u8"repo.working.stage"), submit(repo, stage_files_request { { row.path } })));
                        // 버리기는 확인 대화 상자를 거친다. 추적하지 않는 파일은 휴지통으로 보낸다.
                        std::u8string tooltip { row.untracked ? tr(u8"repo.working.move_to_trash") : tr(u8"repo.working.discard") };
                        buttons.push_back(make_row_button(u8"discard:" + to_u8(row.path), row.untracked ? codicons::icon_trash : codicons::icon_discard, std::move(tooltip),
                            message(discard_files_intent { { row.path }, row.untracked }), true));
                    }
                    if (view == diff_view::staged)
                        buttons.push_back(make_row_button(u8"unstage:" + to_u8(row.path), codicons::icon_remove, tr(u8"repo.working.unstage"), submit(repo, unstage_files_request { { row.path } })));
                    const diff_file_request request { .path = row.path, .view = view, .sha = {}, .untracked = row.untracked };
                    menu_target target {};
                    target.kind = menu_target_kind::file;
                    target.name = row.path;
                    target.side = view == diff_view::staged ? change_side::staged : change_side::unstaged;
                    target.untracked = row.untracked;
                    target.conflicted = row.conflicted;
                    target.lfs = row.lfs;
                    file_row_view shown { row };
                    if (depth.has_value() && shown.original_path.empty())
                        shown.folder.clear();
                    (*builders)[row.key]
                        = [shown, buttons, request, target, depth] { return std::make_unique<file_row_element>(shown, buttons, message(select_file_intent { request }), target, depth); };
                    (*requests)[row.key] = request;
                    if (row.selected)
                        selected = row.key;
                };
                if (working.files_tree == false)
                {
                    for (const file_row_view& row : rows)
                        add_file(row, std::nullopt);
                    return;
                }
                const std::string prefix { rows.empty() ? std::string {} : to_text(rows.front().key.substr(0, 2)) };
                std::vector<std::string> paths {};
                for (const file_row_view& row : rows)
                    paths.push_back(row.path);
                std::set<std::string> collapsed {};
                for (const std::string& path : working.collapsed_folders)
                    if (path.starts_with(prefix))
                        collapsed.insert(path.substr(prefix.size()));
                for (file_tree_row node : build_file_tree(paths, collapsed))
                {
                    if (node.folder == false)
                    {
                        if (node.file_index < rows.size())
                            add_file(rows[node.file_index], node.depth);
                        continue;
                    }
                    node.key = to_u8(prefix) + node.key;
                    items.push_back({ .key = node.key, .label = node.label, .enabled = false });
                    (*builders)[node.key] = [node, prefix] { return std::make_unique<folder_row_element>(node, message(toggle_file_folder_intent { prefix + node.path })); };
                }
            };

            if (working.conflicts.empty() == false)
            {
                add_group(u8"g:conflicts", tr(u8"repo.working.conflicts"), working.conflicts, std::nullopt);
                add_rows(working.conflicts, diff_view::unstaged);
            }
            std::vector<std::string> unstaged_paths {};
            for (const file_row_view& row : working.unstaged)
                unstaged_paths.push_back(row.path);
            std::vector<std::string> staged_paths {};
            for (const file_row_view& row : working.staged)
                staged_paths.push_back(row.path);
            const bool any_unstaged { working.unstaged.empty() == false };
            add_group(u8"g:unstaged", tr(u8"repo.working.unstaged"), working.unstaged,
                make_row_button(u8"stage-all", codicons::icon_add, tr(u8"repo.working.stage_all"), submit(repo, stage_files_request { unstaged_paths }), any_unstaged));
            add_rows(working.unstaged, diff_view::unstaged);
            const bool any_staged { working.staged.empty() == false };
            add_group(u8"g:staged", tr(u8"repo.working.staged"), working.staged,
                make_row_button(u8"unstage-all", codicons::icon_remove, tr(u8"repo.working.unstage_all"), submit(repo, unstage_files_request { staged_paths }), any_staged));
            add_rows(working.staged, diff_view::staged);
            column->add_flexible(make_file_list(u8"working-files", std::move(items), std::move(builders), std::move(requests), std::move(selected), repository.detail_scroll, sources));

            // 커밋 상자 (아래 고정)
            auto box { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"commit-box" }, luil::stack_config { .spacing = 8.0f, .padding = luil::edge_insets::all(12.0f) }) };
            box->add(make_text(u8"commit-message-label", tr(u8"repo.commit_message"), text_style { .size = 12.0f, .bold = true }), 18.0f);
            auto subject_row
                = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"commit-subject-row" }, luil::stack_config { .direction = luil::stack_direction::row, .spacing = 6.0f });
            subject_row->add_flexible(std::make_unique<luil::text_input_element>(
                luil::ui_element_id { kind_commit_subject_input, u8"commit-subject" }, working.subject, luil::text_input_config { .placeholder = tr(u8"repo.working.subject_placeholder") }));
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
            box->add_flexible(std::make_unique<multiline_text_element>(
                luil::ui_element_id { kind_commit_body_input, u8"commit-body" }, working.body, tr(u8"repo.working.body_placeholder"), tr(u8"repo.working.body_access_name")));
            // amend와 서명 토글은 한 줄이다. 서명 토글의 기본값은 설정 `commit.gpgsign`이다.
            auto toggles
                = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"commit-toggles" }, luil::stack_config { .direction = luil::stack_direction::row, .spacing = 6.0f });
            luil::check_config amend { .owner = u8"commit-amend", .label = tr(u8"repo.working.amend"), .checked = working.amend, .toggle = action_of(message(toggle_amend_intent {})) };
            toggles->add_flexible(std::make_unique<luil::check_element>(std::move(amend)));
            luil::check_config sign { .owner = u8"commit-sign", .label = tr(u8"repo.working.sign"), .checked = working.sign, .toggle = action_of(message(toggle_sign_intent {})) };
            toggles->add(std::make_unique<luil::check_element>(std::move(sign)), sign_toggle_width);
            box->add(std::move(toggles), luil::check_row_height);
            if (working.amend_warning)
            {
                const color_selector warning { [](const luil::ui_color_palette& palette) { return palette.warning_accent; } };
                box->add(make_text(u8"commit-amend-warning", tr(u8"repo.working.amend_warning"), text_style { .size = 11.0f, .color = warning, .bold = true }), 16.0f);
                box->add(make_text(u8"commit-amend-warning-detail", tr(u8"repo.working.amend_warning_detail"), text_style { .size = 11.0f, .color = warning }), 16.0f);
            }
            box->add(
                make_text_button(u8"commit-button", working.commit_label, luil::text_button_visual::accent, message(commit_intent {}), working.can_commit, codicons::icon_git_commit, true), 30.0f);
            // 커밋 단추가 막힌 이유가 없을 때는 서명 설정의 문제를 같은 자리에 알린다.
            if (working.blocked_reason.empty() && working.sign_hint.empty() == false)
            {
                const color_selector warning { [](const luil::ui_color_palette& palette) { return palette.warning_accent; } };
                box->add(make_text(u8"commit-blocked", working.sign_hint, text_style { .size = 11.0f, .color = warning }), 16.0f);
            }
            else
                box->add(make_text(u8"commit-blocked", working.blocked_reason, text_style { .size = 11.0f, .color = dim_color }), 16.0f);
            column->add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"commit-box-bar" }, bar_style { .divider_top = true }, std::move(box)),
                std::min(std::clamp(height * 0.45f, commit_box_minimum, commit_box_maximum) + (working.amend_warning ? amend_warning_extra : 0.0f), std::max(height - 150.0f, 220.0f)));
            return column;
        }
    } // namespace

    std::unique_ptr<luil::ui_element> build_sidebar(const repository_view& repository, feedback_sources& sources)
    {
        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"sidebar" }, luil::stack_config {}) };
        auto header = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"sidebar-header" },
            luil::stack_config { .direction = luil::stack_direction::row, .spacing = 6.0f, .padding = luil::edge_insets { 10.0f, 0.0f, 8.0f, 0.0f } });
        header->add(std::make_unique<icon_element>(luil::ui_element_id { kind_label, u8"sidebar-repo-icon" }, codicons::icon_git_branch, color_selector {}), 18.0f);
        header->add_flexible(make_text(u8"sidebar-repo-name", tr(u8"repo.sidebar.title"), text_style { .size = 12.0f, .bold = true }));
        column->add(std::move(header), 32.0f);

        auto filter_row
            = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"sidebar-filter-row" }, luil::stack_config { .padding = luil::edge_insets { 8.0f, 2.0f, 8.0f, 6.0f } });
        luil::text_input_config filter_config {
            .placeholder = tr(u8"repo.sidebar.filter_placeholder"),
            .leading_glyph = codicons::icon_search,
            .clear = action_of(message(edit_text_intent { luil::text_edit_request { .target = to_luil_target(text_target::sidebar_filter), .command = luil::text::text_edit_command::clear } })),
            .clear_tooltip = tr(u8"repo.sidebar.filter_clear"),
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
        const std::array rail_sections {
            sidebar_section::local,
            sidebar_section::remote,
            sidebar_section::tags,
            sidebar_section::stashes,
            sidebar_section::submodules,
            sidebar_section::worktrees,
            sidebar_section::pull_requests,
        };
        for (const sidebar_section section : rail_sections)
        {
            const std::u8string title { section_title(section) };
            const std::u8string count { number_text(repository.section_counts[static_cast<std::size_t>(section)]) };
            column->add(std::make_unique<tool_button_element>(u8"rail-" + std::u8string { section_owner(section) }, section_icon(section), count,
                            tr(u8"repo.rail.section_tooltip", { { u8"title", title }, { u8"count", count } }), true, true, message(open_rail_section_intent { section })),
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
        header->add(make_icon_button(u8"rail-popup-close", codicons::icon_close, tr(u8"repo.rail.close"), message(open_rail_section_intent { section })), 28.0f);
        column->add(std::move(header), 30.0f);
        if (rows.empty())
            column->add_flexible(centered_message(u8"rail-popup-empty", tr(u8"repo.rail.empty")));
        else
            column->add_flexible(make_sidebar_list(u8"rail-popup-list", rows, repository.selected_sidebar_key, 0.0f, false));
        auto panel = std::make_unique<luil::panel_element>(luil::ui_element_id { kind_panel, u8"rail-popup" }, floating_panel_config(6.0f, 0.35f));
        panel->set_content(std::move(column));
        panel->set_hit_opaque(true);
        return panel;
    }

    std::unique_ptr<luil::ui_element> build_graph_area(const view_snapshot& view, const repository_view& repository, const float width, feedback_sources& sources)
    {
        if (repository.diff.has_value())
            return build_diff_area(repository, width, sources);

        // rebase·merge·cherry-pick·revert가 멈춰 있으면 그래프 자리를 닫을 수 없는 충돌 해결 화면이 덮는다.
        if (repository.conflict.has_value())
            return build_conflict_screen(repository, *repository.conflict, width, repository.conflict_scroll, sources);

        if (view.graph_layout == false && repository.detail == detail_mode::working)
        {
            if (repository.loaded == false)
                return surface(u8"review-loading",
                    centered_message(u8"review-loading-note", repository.load_error.empty() ? tr(u8"repo.loading") : tr(u8"repo.load_failed", { { u8"error", repository.load_error } })));
            const bool clean { repository.graph.wip_count == 0 };
            auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"review-empty-content" }, luil::stack_config { .padding = luil::edge_insets::all(24.0f) }) };
            column->add_flexible_gap();
            column->add(make_text(u8"review-empty-title", clean ? tr(u8"repo.review.clean_title") : tr(u8"repo.review.select_title"), text_style { .size = 18.0f, .bold = true, .align = 1 }), 38.0f);
            column->add(make_text(u8"review-empty-description", clean ? tr(u8"repo.review.clean_description") : tr(u8"repo.review.select_description"),
                            text_style { .size = 12.0f, .color = dim_color, .align = 1 }),
                26.0f);
            if (clean == false)
                column->add(make_text(u8"review-empty-next", tr(u8"repo.review.next"), text_style { .size = 12.0f, .color = dim_color, .align = 1 }), 26.0f);
            column->add_flexible_gap();
            return surface(u8"review-empty", std::move(column));
        }

        const graph_view& graph { repository.graph };
        // 행은 목록의 스크롤 막대만큼 좁다. 머리줄도 같은 열 폭을 써야 제목이 열 위에 선다.
        float labels_width { 0.0f };
        if (graph.labels != nullptr)
            for (const auto& [sha, labels] : *graph.labels)
                labels_width = std::max(labels_width, estimate_ref_label_strip_width(labels));
        const graph_columns columns { make_graph_columns(graph.lane_count, graph.compact_columns, width - luil::list_scrollbar_width, labels_width) };
        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"graph-area" }, luil::stack_config {}) };
        column->add(std::make_unique<graph_header_element>(columns), 26.0f);
        if (repository.loaded == false || graph.items == nullptr || graph.items->empty())
        {
            if (repository.load_error.empty() == false)
                column->add_flexible(
                    centered_message(u8"graph-error", tr(u8"repo.load_failed", { { u8"error", repository.load_error } }), [](const luil::ui_color_palette& palette) { return palette.error_accent; }));
            else
                column->add_flexible(centered_message(u8"graph-loading", repository.loaded ? tr(u8"repo.graph.no_commits") : tr(u8"repo.loading")));
            return surface(u8"graph-panel", std::move(column));
        }

        luil::virtual_list_config config {};
        config.owner = u8"graph";
        config.edges.shadows = true;
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
            if (graph_state.signatures != nullptr)
                if (const auto signature { graph_state.signatures->find(commit.sha) }; signature != graph_state.signatures->end())
                {
                    data.signature_known = true;
                    data.signature = signature->second.status;
                }
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
        const std::size_t row_count { graph.items->size() };
        list->set_access_name(row_count == 1 ? tr(u8"repo.graph.access_name_one", { { u8"count", row_count } }) : tr(u8"repo.graph.access_name", { { u8"count", row_count } }));
        sources.graph = list.get();
        column->add_flexible(std::move(list));
        static_cast<void>(view);
        return surface(u8"graph-panel", std::move(column));
    }

    std::unique_ptr<luil::ui_element> build_detail(const view_snapshot& view, const repository_view& repository, const bool drawer, const float width, const float height, feedback_sources& sources)
    {
        static_cast<void>(view);
        std::unique_ptr<luil::ui_element> content {};
        if (repository.detail == detail_mode::pull_request && repository.hosting.has_value() && repository.hosting->detail.has_value())
            content = build_pull_request_detail(*repository.hosting->detail, drawer, width, repository.detail_scroll, sources);
        else if (repository.detail == detail_mode::commit && repository.commit.has_value())
            content = build_commit_detail(repository, *repository.commit, drawer, width, height, sources);
        else if (repository.detail == detail_mode::working && repository.working.has_value())
            content = build_working_area(repository, *repository.working, drawer, height, sources);
        else
            content = centered_message(u8"detail-empty", tr(u8"repo.detail.empty"));
        return std::make_unique<bar_element>(
            luil::ui_element_id { kind_panel, u8"detail-panel" }, bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.surface_background; } }, std::move(content));
    }
} // namespace cyella::ui
