#include "presentation/build_view_snapshot.h"

#include "domain/repository_path.h"
#include "presentation/diff_special.h"
#include "presentation/hosting_sidebar.h"
#include "presentation/sidebar_extension.h"
#include "presentation/snapshot_dialogs.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <format>
#include <string>
#include <utility>

namespace cyella {
    namespace {
        // 실패 배너의 [자세히]가 보일 git 원문 줄 수의 상한이다. 넘치면 마지막 줄에 생략을 알린다.
        constexpr std::size_t banner_detail_line_limit { 40 };
        // 작업 로그가 보이는 operation 수다.
        constexpr std::size_t log_operation_limit { 60 };
        // HEAD 행을 찾아보는 범위다. WIP 점선은 화면 위쪽에서만 뜻이 있다.
        constexpr std::size_t head_search_limit { 4096 };
        // diff 임시 본문이 보이는 줄 수의 상한이다. 큰 diff는 웹뷰의 몫이다.
        constexpr std::size_t diff_line_limit { 20000 };

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::u8string number(const std::uint64_t value)
        {
            return to_u8(std::to_string(value));
        }

        std::u8string percent_text(const double fraction)
        {
            const double clamped { std::clamp(fraction, 0.0, 1.0) };
            return number(static_cast<std::uint64_t>(clamped * 100.0 + 0.5)) + u8"%";
        }

        std::vector<std::u8string> split_lines(const std::u8string_view text)
        {
            std::vector<std::u8string> lines {};
            std::size_t start { 0 };
            while (start < text.size())
            {
                std::size_t end { text.find(u8'\n', start) };
                if (end == std::u8string_view::npos)
                    end = text.size();
                std::u8string line { text.substr(start, end - start) };
                if (line.empty() == false && line.back() == u8'\r')
                    line.pop_back();
                lines.push_back(std::move(line));
                start = end + 1;
            }
            return lines;
        }

        // UTF-8 글자 수다. 이어지는 바이트(10xxxxxx)는 세지 않는다.
        std::size_t character_count(const std::u8string_view text) noexcept
        {
            return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](const char8_t value) { return (value & 0xC0u) != 0x80u; }));
        }

        // 이 컴퓨터의 시간대다. tzdb를 읽지 못하면 null이다.
        const std::chrono::time_zone* local_time_zone() noexcept
        {
            try
            {
                return std::chrono::current_zone();
            }
            catch (...)
            {
                return nullptr;
            }
        }

        std::u8string short_sha(const std::string_view sha)
        {
            return to_u8(sha.substr(0, std::min<std::size_t>(sha.size(), 7)));
        }

        // 상태 줄·탭 표시·배너가 보이는 작업이다. 판정은 `is_user_operation_status` 하나다.
        bool is_user_operation(const operation_status& status) noexcept
        {
            return is_user_operation_status(status);
        }

        bool is_active(const operation_status& status) noexcept
        {
            return status.state == operation_state::queued || status.state == operation_state::running;
        }

        std::u8string command_line_text(const operation_command& command)
        {
            std::u8string line { u8"$ " + command.command_line };
            if (command.exit_code.has_value())
                line += u8"   → exit " + to_u8(std::to_string(*command.exit_code));
            else if (command.completion == process_completion::cancelled)
                line += u8"   → 취소 (프로세스 트리 종료)";
            else if (command.completion == process_completion::timed_out)
                line += u8"   → 시간 초과";
            line += u8"  (" + number(static_cast<std::uint64_t>(std::max<long long>(command.duration.count(), 0))) + u8" ms)";
            return line;
        }

        std::u8string phase_text(const operation_status& status)
        {
            std::u8string text { operation_display_name(status.kind) };
            if (status.state == operation_state::queued)
                return text + u8": 대기 중";
            if (status.progress.phase_text.empty() == false)
                text += u8": " + status.progress.phase_text;
            else
                text += u8": 실행 중";
            if (status.progress.fraction.has_value())
                text += u8" " + percent_text(*status.progress.fraction);
            return text;
        }

        std::u8string result_text(const operation_status& status)
        {
            const std::u8string name { operation_display_name(status.kind) };
            switch (status.state)
            {
            case operation_state::succeeded:
                return name + (status.result_summary.empty() ? std::u8string { u8" 완료" } : u8": " + status.result_summary);
            case operation_state::failed:
                return name + u8" 실패: " + (status.failure.has_value() ? status.failure->summary : std::u8string {});
            case operation_state::cancelled:
                return name + u8" 취소됨";
            case operation_state::needs_input:
                return name + u8": 입력 필요";
            case operation_state::queued:
            case operation_state::running:
                break;
            }
            return name;
        }

        std::vector<const operation_status*> operations_of(const std::vector<const operation_status*>& operations, const std::string& repository_id)
        {
            std::vector<const operation_status*> result {};
            for (const operation_status* status : operations)
                if (status != nullptr && status->repository_id == repository_id)
                    result.push_back(status);
            return result;
        }

        const operation_status* latest_finished_user(std::span<const operation_status* const> operations)
        {
            for (auto it { operations.rbegin() }; it != operations.rend(); ++it)
                if (is_user_operation(**it) && (*it)->finished())
                    return *it;
            return nullptr;
        }

        std::optional<active_operation_view> active_operation(std::span<const operation_status* const> operations)
        {
            const operation_status* running { nullptr };
            const operation_status* queued { nullptr };
            for (const operation_status* status : operations)
            {
                if (is_user_operation(*status) == false)
                    continue;
                if (status->state == operation_state::running)
                    running = status;
                else if (status->state == operation_state::queued && queued == nullptr)
                    queued = status;
            }
            const operation_status* chosen { running != nullptr ? running : queued };
            if (chosen == nullptr)
                return std::nullopt;
            return active_operation_view {
                .id = chosen->id,
                .text = phase_text(*chosen),
                .fraction = chosen->progress.fraction,
                .cancellable = chosen->cancellable && chosen->cancel_requested == false,
                .queued = chosen->state == operation_state::queued,
            };
        }

        std::optional<banner_view> operation_banner(const operation_status& status, const operation_id dismissed, const bool expanded)
        {
            if (status.id <= dismissed)
                return std::nullopt;
            banner_view banner {};
            banner.id = status.id;
            banner.expanded = expanded;
            if (status.state == operation_state::failed && status.failure.has_value())
            {
                banner.kind = banner_kind::failure;
                banner.title = operation_display_name(status.kind) + u8" 실패";
                banner.summary = status.failure->summary.empty() ? std::u8string { failure_kind_message(status.failure->kind) } : status.failure->summary;
                for (const operation_command& command : status.commands)
                    banner.details.push_back(command_line_text(command));
                for (std::u8string& line : split_lines(status.failure->detail))
                {
                    if (banner.details.size() >= banner_detail_line_limit)
                    {
                        banner.details.push_back(u8"…");
                        break;
                    }
                    banner.details.push_back(std::move(line));
                }
                banner.actions = banner_actions_for(status);
                return banner;
            }
            return std::nullopt;
        }

        std::vector<log_line_view> build_log(const std::vector<const operation_status*>& operations)
        {
            std::vector<log_line_view> lines {};
            std::size_t shown { 0 };
            for (auto it { operations.rbegin() }; it != operations.rend() && shown < log_operation_limit; ++it)
            {
                const operation_status& status { **it };
                if (status.kind == operation_kind::load_more_log || status.kind == operation_kind::diff_file || status.kind == operation_kind::show_commit
                    || status.kind == operation_kind::refresh_status || status.kind == operation_kind::verify_signatures)
                    continue;
                ++shown;
                std::u8string heading { u8"#" + number(status.id) + u8"  " + (status.finished() ? result_text(status) : phase_text(status)) };
                if (status.automatic)
                    heading += u8"  (자동)";
                lines.push_back({ .key = u8"op-" + number(status.id), .text = std::move(heading), .heading = true, .failed = status.state == operation_state::failed, .automatic = status.automatic });
                std::size_t index { 0 };
                for (const operation_command& command : status.commands)
                    lines.push_back({ .key = u8"op-" + number(status.id) + u8"-" + number(index++), .text = command_line_text(command), .automatic = status.automatic });
            }
            return lines;
        }

        std::u8string folder_of(const std::string_view path)
        {
            const std::size_t slash { path.find_last_of('/') };
            if (slash == std::string_view::npos)
                return {};
            return u8"/" + to_u8(path.substr(0, slash));
        }

        std::u8string name_of(const std::string_view path)
        {
            const std::size_t slash { path.find_last_of('/') };
            return to_u8(slash == std::string_view::npos ? path : path.substr(slash + 1));
        }

        bool same_diff(const diff_file_request& left, const diff_file_request& right) noexcept
        {
            return left.path == right.path && left.view == right.view && left.sha == right.sha;
        }

        void mark_selected(std::vector<file_row_view>& rows, const std::optional<diff_file_request>& diff, const diff_view view, const std::string& sha)
        {
            if (diff.has_value() == false || diff->view != view || diff->sha != sha)
                return;
            for (file_row_view& row : rows)
                row.selected = row.path == diff->path;
        }

        working_area_view build_working_area(const repository_snapshot& snapshot, const tab_ui_state& tab, const std::optional<luil::text_composition_event>& composition, const bool committing)
        {
            working_area_view view {};
            view.scroll = tab.working_scroll;
            view.branch = snapshot.summary.is_detached ? u8"(detached) " + short_sha(snapshot.summary.head_sha) : to_u8(snapshot.summary.current_branch);
            for (const file_change& change : snapshot.conflicts)
                view.conflicts.push_back(make_file_row(change, u8"c:"));
            for (const file_change& change : snapshot.unstaged)
                view.unstaged.push_back(make_file_row(change, u8"u:"));
            for (const file_change& change : snapshot.staged)
                view.staged.push_back(make_file_row(change, u8"s:"));
            mark_selected(view.unstaged, tab.diff, diff_view::unstaged, {});
            mark_selected(view.staged, tab.diff, diff_view::staged, {});

            view.subject = luil::make_text_input_view(tab.commit_subject, composition, to_luil_target(text_target::commit_subject));
            view.body = luil::make_text_input_view(tab.commit_body, composition, to_luil_target(text_target::commit_body));
            view.subject_length = character_count(view.subject.displayed_text());
            view.amend = tab.amend;
            view.sign = tab.sign.value_or(snapshot.signing.commit_signing());
            view.sign_hint = signing_setup_hint(snapshot.signing, view.sign);
            view.committing = committing;
            const std::u8string staged_count { u8"(staged " + number(view.staged.size()) + u8"개)" };
            view.commit_label = tab.amend ? u8"직전 커밋 수정 " + staged_count : view.branch + u8"에 커밋 " + staged_count;

            const bool has_subject { view.subject.displayed_text().empty() == false };
            if (committing)
                view.blocked_reason = u8"커밋하는 중입니다…";
            else if (view.conflicts.empty() == false)
                view.blocked_reason = u8"충돌을 먼저 해결하세요.";
            else if (has_subject == false)
                view.blocked_reason = u8"커밋 제목을 입력하세요.";
            else if (view.staged.empty() && tab.amend == false)
                view.blocked_reason = u8"stage한 변경이 없습니다.";
            view.can_commit = view.blocked_reason.empty();
            view.amend_warning = tab.amend && snapshot.summary.head_pushed;
            return view;
        }

        commit_detail_view build_commit_detail(
            const repository_session& session, const std::string& sha, const tab_ui_state& tab, const ref_label_map& labels, const std::int64_t now, const bool files_tree)
        {
            commit_detail_view view {};
            view.sha = sha;
            view.short_sha = short_sha(sha);
            const std::optional<commit_detail>& detail { session.selected_commit() };
            const commit_info* info { nullptr };
            if (detail.has_value() && detail->commit.sha == sha)
                info = &detail->commit;
            else
                view.loading = true;
            const std::vector<commit_info>& commits { session.graph_commits() };
            const auto found { std::find_if(commits.begin(), commits.end(), [&sha](const commit_info& commit) { return commit.sha == sha; }) };
            if (found != commits.end())
            {
                const auto index { static_cast<std::size_t>(found - commits.begin()) };
                if (index < session.graph_rows().size())
                    view.lane_color = session.graph_rows()[index].color;
                if (info == nullptr)
                    info = &*found;
            }
            if (info != nullptr)
            {
                view.subject = to_u8(info->subject);
                view.body = to_u8(info->body.value_or(std::string {}));
                view.author = to_u8(info->author_name) + (info->author_email.empty() ? std::u8string {} : u8" <" + to_u8(info->author_email) + u8">");
                view.author_name = to_u8(info->author_name);
                view.author_email = to_u8(info->author_email);
                view.date = relative_time_text(now, info->author_time);
                view.date_absolute = absolute_time_text(info->author_time);
                for (const std::string& parent : info->parents)
                {
                    view.parents.push_back(short_sha(parent));
                    view.parent_shas.push_back(parent);
                }
            }
            if (const auto labelled { labels.find(sha) }; labelled != labels.end())
                view.labels = labelled->second;
            view.signature = make_signature_view(session.signature_of(sha));
            view.files_tree = files_tree;
            if (view.loading == false)
            {
                std::uint64_t added { 0 };
                std::uint64_t removed { 0 };
                for (const file_change& change : detail->files)
                {
                    view.files.push_back(make_file_row(change, u8"f:"));
                    if (change.is_submodule)
                        continue;
                    added += change.lines_added.value_or(0);
                    removed += change.lines_removed.value_or(0);
                }
                if (added > 0)
                    view.added_total = u8"+" + number(added);
                if (removed > 0)
                    view.removed_total = u8"-" + number(removed);
                mark_selected(view.files, tab.diff, diff_view::commit, sha);
                if (files_tree)
                {
                    std::vector<std::string> paths {};
                    paths.reserve(view.files.size());
                    for (const file_row_view& row : view.files)
                        paths.push_back(row.path);
                    view.tree = build_file_tree(paths, tab.collapsed_folders);
                }
            }
            return view;
        }

        diff_panel_view build_diff(const repository_session& session, const diff_file_request& request, const float scroll, const diff_surface_state& surface)
        {
            diff_panel_view view {};
            view.request = request;
            view.scroll = scroll;
            view.surface = surface;
            view.use_webview = surface.webview_available;
            view.title = name_of(request.path);
            switch (request.view)
            {
            case diff_view::unstaged:
                view.subtitle = to_u8(request.path) + u8" · 작업 파일";
                break;
            case diff_view::staged:
                view.subtitle = to_u8(request.path) + u8" · 스테이징 사본";
                break;
            case diff_view::commit:
                view.subtitle = to_u8(request.path) + u8" · 이력 " + short_sha(request.sha);
                break;
            }
            const std::optional<file_diff>& diff { session.selected_diff() };
            if (diff.has_value() == false || same_diff(diff->request, request) == false)
            {
                view.loading = true;
                return view;
            }
            for (const diff_file& file : diff->files)
            {
                // gitlink·LFS 포인터는 텍스트 diff 대신 카드다.
                if (file.is_special())
                {
                    view.special = build_diff_special(file);
                    break;
                }
                if (file.is_binary)
                {
                    view.binary = true;
                    continue;
                }
                // 웹뷰가 본문을 그린다. 줄 목록은 대체 보기에만 필요하다.
                if (view.use_webview)
                    continue;
                for (const diff_hunk& hunk : file.hunks)
                {
                    if (view.lines.size() >= diff_line_limit)
                        return view;
                    view.lines.push_back({ .hunk_header = true, .text = to_u8(hunk.header) });
                    for (const diff_line& line : hunk.lines)
                    {
                        if (view.lines.size() >= diff_line_limit)
                            return view;
                        view.lines.push_back({ .kind = line.kind, .old_line = line.old_line, .new_line = line.new_line, .text = to_u8(line.content) });
                    }
                }
            }
            return view;
        }

        std::size_t find_head_row(const std::vector<commit_info>& commits, const std::string& head_sha)
        {
            const std::size_t limit { std::min(commits.size(), head_search_limit) };
            for (std::size_t index { 0 }; index < limit; ++index)
                if (commits[index].sha == head_sha)
                    return index;
            return 0;
        }

        // 처음 읽는 중이 아니고 마지막 refresh가 실패했으면 그 문구다.
        std::u8string load_error_of(std::span<const operation_status* const> operations)
        {
            for (auto it { operations.rbegin() }; it != operations.rend(); ++it)
            {
                if ((*it)->kind != operation_kind::refresh)
                    continue;
                if ((*it)->state == operation_state::failed && (*it)->failure.has_value())
                    return (*it)->failure->summary;
                return {};
            }
            return {};
        }

        void fill_diff_navigation(repository_view& repository)
        {
            diff_panel_view& diff { *repository.diff };
            const std::vector<file_row_view>* files { nullptr };
            bool tree { false };
            if (diff.request.view == diff_view::commit)
            {
                if (repository.commit.has_value() == false || repository.commit->sha != diff.request.sha)
                    return;
                files = &repository.commit->files;
                tree = repository.commit->files_tree;
            }
            else if (repository.working.has_value())
            {
                files = diff.request.view == diff_view::staged ? &repository.working->staged : &repository.working->unstaged;
                tree = repository.working->files_tree;
            }
            if (files == nullptr || files->empty())
                return;
            std::vector<std::size_t> order {};
            order.reserve(files->size());
            if (tree)
            {
                std::vector<std::string> paths {};
                paths.reserve(files->size());
                for (const file_row_view& file : *files)
                    paths.push_back(file.path);
                // 접힌 폴더 안의 파일도 검토하며 폴더·파일 순서는 트리와 맞춘다.
                for (const file_tree_row& row : build_file_tree(paths, {}))
                    if (row.folder == false)
                        order.push_back(row.file_index);
            }
            else
                for (std::size_t index { 0 }; index < files->size(); ++index)
                    order.push_back(index);
            const auto selected { std::ranges::find_if(order, [&](const std::size_t index) { return (*files)[index].path == diff.request.path; }) };
            if (selected == order.end())
                return;
            const std::size_t position { static_cast<std::size_t>(selected - order.begin()) };
            diff.file_position = position + 1;
            diff.file_count = order.size();
            const auto request_at = [&](const std::size_t index) {
                diff_file_request request { diff.request };
                request.path = (*files)[order[index]].path;
                request.untracked = (*files)[order[index]].untracked;
                return request;
            };
            if (position > 0)
                diff.previous_file = request_at(position - 1);
            if (position + 1 < order.size())
                diff.next_file = request_at(position + 1);
        }

        repository_view build_repository(const view_inputs& inputs, const repository_tab& tab, const repository_session& session, const tab_ui_state& tab_state,
            std::span<const operation_status* const> operations, const std::int64_t now, const std::shared_ptr<const graph_index>& index)
        {
            const ui_state& ui { *inputs.ui };
            const repository_snapshot& snapshot { session.snapshot() };
            repository_view view {};
            view.tab = tab.id;
            view.repository_id = session.id();
            view.display_name = tab.display_name;
            view.loaded = session.loaded();
            if (view.loaded == false)
                view.load_error = load_error_of(operations);

            view.sidebar_filter = luil::make_text_input_view(tab_state.sidebar_filter, ui.composition, to_luil_target(text_target::sidebar_filter));
            sidebar_rows sidebar { build_sidebar_rows(snapshot, view.sidebar_filter.displayed_text(), tab_state.collapsed_sections) };
            std::optional<hosting_repository_view> hosting {};
            if (inputs.hosting_view != nullptr)
            {
                hosting = inputs.hosting_view(session.id());
                append_hosting_sidebar_rows(sidebar, *hosting, view.sidebar_filter.displayed_text(), tab_state.collapsed_sections);
            }
            view.sidebar = std::move(sidebar.rows);
            view.section_counts = sidebar.counts;
            view.selected_sidebar_key = tab_state.selected_sidebar_key;
            view.sidebar_scroll = tab_state.sidebar_scroll;
            view.rail_section = ui.rail_section;

            auto labels { std::make_shared<ref_label_map>(build_ref_label_map(snapshot.refs, snapshot.summary)) };
            graph_view& graph { view.graph };
            graph.rows = &session.graph_rows();
            graph.commits = &session.graph_commits();
            graph.signatures = &session.signatures();
            graph.items = inputs.graph_items != nullptr ? inputs.graph_items(session) : std::make_shared<const std::vector<luil::virtual_list_item>>(build_graph_items(session.graph_commits()));
            graph.selected_key = tab_state.selected_sha.empty() ? std::u8string {} : graph_item_key(tab_state.selected_sha);
            graph.scroll = tab_state.graph_scroll;
            graph.lane_count = session.graph_lane_count();
            graph.has_wip = session.has_wip_row();
            graph.wip_count = snapshot.summary.changed_file_count;
            graph.head_sha = snapshot.summary.head_sha;
            graph.head_row_index = graph.has_wip ? find_head_row(session.graph_commits(), snapshot.summary.head_sha) : 0;
            graph.loading = session.log_page_loading();
            graph.history_complete = session.history_complete();
            graph.compact_columns = select_layout_mode(inputs.width / std::max(inputs.scale, 0.01f)) == layout_mode::narrow;
            graph.now = now;

            if (tab_state.selected_sha == graph_wip_sha)
            {
                bool committing { false };
                if (tab_state.pending_commit != 0)
                    for (const operation_status* status : operations)
                        if (status->id == tab_state.pending_commit && status->finished() == false)
                            committing = true;
                view.detail = detail_mode::working;
                view.working = build_working_area(snapshot, tab_state, ui.composition, committing);
                view.working->files_tree = inputs.settings->ui.working_files_tree;
                view.working->collapsed_folders = tab_state.collapsed_folders;
            }
            else if (tab_state.selected_sha.empty() == false && tab_state.selected_sha != graph_wip_sha)
            {
                view.detail = detail_mode::commit;
                view.commit = build_commit_detail(session, tab_state.selected_sha, tab_state, *labels, now, inputs.settings->ui.commit_files_tree);
                if (inputs.commit_ci != nullptr)
                    view.commit->ci = inputs.commit_ci(session.id(), tab_state.selected_sha);
                // [이후 커밋 N개 편집…]이 말할 수다: `rev-list --count 커밋..HEAD`, 곧 이 커밋 위로 다시 쌓을 현재 브랜치의 커밋이다.
                // 다른 갈래의 커밋(main 끝 등)을 고르면 현재 브랜치를 그 위로 옮겨 쌓는 일이 된다. 그 차이는 `on_current_branch`가 말한다.
                const std::string& head { snapshot.summary.head_sha };
                if (index != nullptr && head.empty() == false && head != tab_state.selected_sha)
                {
                    view.commit->commits_above = index->count(tab_state.selected_sha, head);
                    view.commit->on_current_branch = index->is_ancestor(tab_state.selected_sha, head).value_or(false);
                }
            }
            // 사이드바에서 연 PR·MR 상세는 커밋·WIP 상세를 덮는다. 커밋을 고르면 controller가 PR 선택을 풀어 커밋 상세가 돌아온다.
            if (hosting.has_value() && hosting->detail.has_value())
                view.detail = detail_mode::pull_request;
            view.hosting = std::move(hosting);
            view.detail_scroll = tab_state.detail_scroll;
            view.commit_meta_scroll = tab_state.commit_meta_scroll;
            if (tab_state.diff.has_value())
            {
                view.diff = build_diff(session, *tab_state.diff, tab_state.diff_scroll, ui.diff_surface);
                fill_diff_navigation(view);
            }
            graph.labels = std::move(labels);
            view.drop_hints = build_drop_hints(snapshot, index);
            view.conflict = build_conflict_view(session, operations);
            view.conflict_scroll = tab_state.diff_scroll;
            view.head_pushed = snapshot.summary.head_pushed;
            return view;
        }

        void fill_toolbar(view_snapshot& view, const repository_session& session, std::span<const operation_status* const> operations, const bool git_usable)
        {
            const repository_summary& summary { session.snapshot().summary };
            toolbar_view& toolbar { view.toolbar };
            toolbar.repository_name = view.repository->display_name;
            toolbar.detached = summary.is_detached;
            toolbar.branch_text = summary.is_detached ? u8"(detached) " + short_sha(summary.head_sha) : to_u8(summary.current_branch);
            if (toolbar.branch_text.empty())
                toolbar.branch_text = u8"(브랜치 없음)";
            toolbar.ahead = summary.ahead;
            toolbar.behind = summary.behind;
            toolbar.show_labels = toolbar_shows_labels(view.logical_width);
            toolbar.show_repository_selector = toolbar_shows_repository_selector(view.logical_width);
            toolbar.show_detail_toggle = view.mode != layout_mode::wide;
            toolbar.drawer_open = view.drawer_open;
            const bool ready { git_usable && session.loaded() };
            toolbar.fetch_enabled = ready && summary.remotes.empty() == false;
            toolbar.pull_enabled = ready && summary.has_upstream;
            toolbar.push_enabled = ready && summary.remotes.empty() == false && summary.is_detached == false && summary.current_branch.empty() == false;
            toolbar.push_needs_dialog = summary.has_upstream == false;
            toolbar.fetch_hint
                = summary.remotes.empty() ? u8"등록된 원격이 없습니다. 원격을 추가하면 Fetch를 사용할 수 있습니다." : u8"원격의 새 커밋과 참조를 가져옵니다. 작업 파일은 바꾸지 않습니다.";
            toolbar.pull_hint = summary.has_upstream ? u8"추적 원격의 변경을 fast-forward로 받습니다 (--ff-only)." : u8"현재 브랜치의 추적 원격(upstream)을 먼저 설정하세요.";
            toolbar.push_hint = summary.remotes.empty() ? u8"등록된 원격이 없습니다. 원격을 추가하면 Push를 사용할 수 있습니다."
                : summary.is_detached                   ? u8"브랜치에 연결되지 않은 HEAD입니다. 브랜치를 만든 뒤 Push할 수 있습니다."
                : toolbar.push_needs_dialog             ? u8"원격을 고르고 현재 브랜치의 추적 원격(upstream)을 설정합니다."
                                                        : u8"현재 브랜치의 커밋을 추적 원격으로 보냅니다.";
            toolbar.branch_enabled = ready;
            toolbar.stash_enabled = ready && summary.changed_file_count > 0;
            toolbar.pop_enabled = ready && session.snapshot().stashes.empty() == false;
            if (const std::optional<active_operation_view> active { active_operation(operations) }; active.has_value())
                toolbar.busy_text = active->text;
        }

        void fill_status(view_snapshot& view, std::span<const operation_status* const> operations, const repository_session* session, const bool log_open)
        {
            status_view& status { view.status };
            status.log_open = log_open;
            status.active = active_operation(operations);
            for (const operation_status* value : operations)
                if (is_user_operation(*value) && value->state == operation_state::queued)
                    ++status.queued_count;
            if (const operation_status* last { latest_finished_user(operations) }; last != nullptr)
            {
                status.last_result = result_text(*last);
                status.last_failed = last->state == operation_state::failed;
                status.last_needs_input = last->state == operation_state::needs_input;
            }
            if (session != nullptr)
            {
                const repository_summary& summary { session->snapshot().summary };
                status.repository = true;
                status.branch_text = view.toolbar.branch_text;
                status.ahead = summary.ahead;
                status.behind = summary.behind;
                status.changes = summary.changed_file_count;
            }
        }
    } // namespace

    bool is_user_operation_status(const operation_status& status) noexcept
    {
        if (status.automatic)
            return false;
        switch (status.kind)
        {
        case operation_kind::show_commit:
        case operation_kind::diff_file:
        case operation_kind::load_more_log:
        case operation_kind::is_ancestor:
        case operation_kind::count_commits:
        case operation_kind::refresh_remote_tags:
        case operation_kind::verify_signatures:
        case operation_kind::refresh_status:
        case operation_kind::inspect_path:
        // 편집기가 자기 자리에 읽는 중과 실패를 보인다.
        case operation_kind::load_rebase_plan:
            return false;
        default:
            break;
        }
        return true;
    }

    tab_marker compute_tab_marker(std::span<const operation_status* const> operations, const std::uint32_t changed_file_count, const bool awaiting_input) noexcept
    {
        if (awaiting_input)
            return tab_marker::needs_input;
        bool running { false };
        bool queued { false };
        for (const operation_status* status : operations)
        {
            if (is_user_operation(*status) == false)
                continue;
            if (status->state == operation_state::needs_input && status->finished() == false)
                return tab_marker::needs_input;
            running = running || status->state == operation_state::running;
            queued = queued || status->state == operation_state::queued;
        }
        if (running)
            return tab_marker::running;
        if (queued)
            return tab_marker::queued;
        return changed_file_count > 0 ? tab_marker::changes : tab_marker::none;
    }

    sidebar_rows build_sidebar_rows(const repository_snapshot& snapshot, const std::u8string_view filter, const std::uint8_t collapsed_sections)
    {
        sidebar_rows result {};
        const auto collapsed = [collapsed_sections](const sidebar_section section) { return (collapsed_sections & (1u << static_cast<unsigned>(section))) != 0; };
        const auto matches = [filter](const std::string_view name) { return matches_search_text(to_u8(name), filter); };
        const bool has_remotes { snapshot.summary.remotes.empty() == false };

        const auto add_header = [&result, &collapsed](const sidebar_section section, std::u8string label) {
            const std::size_t index { static_cast<std::size_t>(section) };
            result.rows.push_back({ .key = u8"h:" + label, .header = true, .section = section, .label = std::move(label), .collapsed = collapsed(section) });
            return index;
        };

        // LOCAL
        {
            const std::size_t header { add_header(sidebar_section::local, u8"LOCAL") };
            const std::size_t header_row { result.rows.size() - 1 };
            std::uint32_t count { 0 };
            for (const ref& value : snapshot.refs)
            {
                if (value.kind != ref_kind::local || matches(value.name) == false)
                    continue;
                ++count;
                if (collapsed(sidebar_section::local))
                    continue;
                sidebar_row_view row {};
                row.key = u8"l:" + to_u8(value.name);
                row.section = sidebar_section::local;
                row.label = to_u8(value.name);
                row.kind = ref_kind::local;
                row.ref_name = value.name;
                row.target_sha = value.target_sha;
                row.current = value.is_current;
                row.ahead = value.ahead;
                row.behind = value.behind;
                row.no_upstream = has_remotes && value.upstream.empty();
                row.upstream_gone = value.upstream_gone;
                row.worktree_path = value.worktree_path;
                row.tooltip = value.upstream.empty() ? row.label + u8" · upstream 없음" : row.label + u8" → " + to_u8(value.upstream);
                if (value.worktree_path.empty() == false)
                    row.tooltip += u8" · 다른 worktree에서 체크아웃됨: " + to_u8(value.worktree_path);
                result.rows.push_back(std::move(row));
            }
            result.counts[header] = count;
            result.rows[header_row].count = count;
        }

        // REMOTE: 원격 이름마다 묶는다.
        {
            const std::size_t header { add_header(sidebar_section::remote, u8"REMOTE") };
            const std::size_t header_row { result.rows.size() - 1 };
            std::vector<std::string> remote_names {};
            for (const remote_info& remote : snapshot.summary.remotes)
                remote_names.push_back(remote.name);
            for (const ref& value : snapshot.refs)
            {
                if (value.kind != ref_kind::remote)
                    continue;
                const std::string prefix { value.name.substr(0, value.name.find('/')) };
                if (std::find(remote_names.begin(), remote_names.end(), prefix) == remote_names.end())
                    remote_names.push_back(prefix);
            }
            std::uint32_t count { 0 };
            for (const std::string& remote : remote_names)
            {
                std::vector<const ref*> branches {};
                for (const ref& value : snapshot.refs)
                    if (value.kind == ref_kind::remote && value.name.starts_with(remote + "/") && value.name.ends_with("/HEAD") == false && matches(value.name))
                        branches.push_back(&value);
                count += static_cast<std::uint32_t>(branches.size());
                if (collapsed(sidebar_section::remote) || (branches.empty() && filter.empty() == false))
                    continue;
                sidebar_row_view group {};
                group.key = u8"r:" + to_u8(remote);
                group.group = true;
                group.section = sidebar_section::remote;
                group.kind = ref_kind::remote;
                group.label = to_u8(remote);
                for (const remote_info& info : snapshot.summary.remotes)
                    if (info.name == remote)
                        group.detail = to_u8(info.fetch_url);
                group.tooltip = group.detail.empty() ? group.label : group.label + u8" · " + group.detail;
                result.rows.push_back(std::move(group));
                for (const ref* value : branches)
                {
                    sidebar_row_view row {};
                    row.key = u8"rb:" + to_u8(value->name);
                    row.section = sidebar_section::remote;
                    row.label = to_u8(value->name.substr(remote.size() + 1));
                    row.kind = ref_kind::remote;
                    row.ref_name = value->name;
                    row.target_sha = value->target_sha;
                    row.depth = 1;
                    row.tooltip = to_u8(value->name);
                    result.rows.push_back(std::move(row));
                }
            }
            result.counts[header] = count;
            result.rows[header_row].count = count;
        }

        // TAGS
        {
            const std::size_t header { add_header(sidebar_section::tags, u8"TAGS") };
            const std::size_t header_row { result.rows.size() - 1 };
            std::uint32_t count { 0 };
            for (const ref& value : snapshot.refs)
            {
                if (value.kind != ref_kind::tag || matches(value.name) == false)
                    continue;
                ++count;
                if (collapsed(sidebar_section::tags))
                    continue;
                sidebar_row_view row {};
                row.key = u8"t:" + to_u8(value.name);
                row.section = sidebar_section::tags;
                row.label = to_u8(value.name);
                row.kind = ref_kind::tag;
                row.ref_name = value.name;
                row.target_sha = value.target_sha;
                row.local_only = has_remotes && value.remotes.empty();
                row.tooltip = row.local_only ? row.label + u8" · 원격에 없는 태그" : row.label;
                result.rows.push_back(std::move(row));
            }
            result.counts[header] = count;
            result.rows[header_row].count = count;
        }

        // STASHES
        {
            const std::size_t header { add_header(sidebar_section::stashes, u8"STASHES") };
            const std::size_t header_row { result.rows.size() - 1 };
            std::uint32_t count { 0 };
            for (const stash_entry& stash : snapshot.stashes)
            {
                if (matches(stash.message) == false && matches(stash.name) == false)
                    continue;
                ++count;
                if (collapsed(sidebar_section::stashes))
                    continue;
                sidebar_row_view row {};
                row.key = u8"s:" + number(stash.index);
                row.section = sidebar_section::stashes;
                row.label = to_u8(stash.message.empty() ? stash.name : stash.message);
                row.detail = to_u8(stash.name);
                row.kind = ref_kind::stash;
                row.stash_index = stash.index;
                row.ref_name = stash.name;
                row.target_sha = stash.sha;
                row.tooltip = row.detail + u8" · " + row.label;
                result.rows.push_back(std::move(row));
            }
            result.counts[header] = count;
            result.rows[header_row].count = count;
        }

        // SUBMODULES·WORKTREES·GIT LFS
        append_extension_sidebar_rows(result, snapshot, filter, collapsed_sections);
        return result;
    }

    std::vector<luil::virtual_list_item> build_graph_items(const std::vector<commit_info>& commits)
    {
        std::vector<luil::virtual_list_item> items {};
        items.reserve(commits.size());
        for (const commit_info& commit : commits)
            items.push_back({ .key = graph_item_key(commit.sha) });
        return items;
    }

    std::u8string relative_time_text(const std::int64_t now_seconds, const std::int64_t then_seconds)
    {
        const std::int64_t elapsed { std::max<std::int64_t>(now_seconds - then_seconds, 0) };
        constexpr std::int64_t minute { 60 };
        constexpr std::int64_t hour { 60 * minute };
        constexpr std::int64_t day { 24 * hour };
        constexpr std::int64_t month { 30 * day };
        constexpr std::int64_t year { 365 * day };
        const auto count = [](const std::int64_t value) { return number(static_cast<std::uint64_t>(value)); };
        if (elapsed < minute)
            return u8"방금";
        if (elapsed < hour)
            return count(elapsed / minute) + u8"분 전";
        if (elapsed < day)
            return count(elapsed / hour) + u8"시간 전";
        if (elapsed < month)
            return count(elapsed / day) + u8"일 전";
        if (elapsed < year)
            return count(elapsed / month) + u8"개월 전";
        return count(elapsed / year) + u8"년 전";
    }

    std::u8string absolute_time_text(const std::int64_t seconds)
    {
        // 시간대 조회는 한 번만 한다. 실패를 매 frame 예외로 되풀이하지 않는다.
        static const std::chrono::time_zone* const zone { local_time_zone() };
        const std::chrono::sys_seconds point { std::chrono::seconds { seconds } };
        if (zone == nullptr)
            return to_u8(std::format("{:%Y-%m-%d %H:%M} UTC", point));
        return to_u8(std::format("{:%Y-%m-%d %H:%M}", std::chrono::zoned_time { zone, point }));
    }

    std::u8string operation_display_name(const operation_kind kind)
    {
        switch (kind)
        {
        case operation_kind::refresh:
            return u8"새로 고침";
        case operation_kind::load_more_log:
            return u8"이력 읽기";
        case operation_kind::show_commit:
            return u8"커밋 읽기";
        case operation_kind::diff_file:
            return u8"diff 읽기";
        case operation_kind::stage_files:
            return u8"Stage";
        case operation_kind::unstage_files:
            return u8"Unstage";
        case operation_kind::discard_files:
            return u8"변경 버리기";
        case operation_kind::commit:
            return u8"커밋";
        case operation_kind::checkout_branch:
        case operation_kind::checkout_remote_branch:
        case operation_kind::checkout_detached:
            return u8"체크아웃";
        case operation_kind::fetch:
            return u8"Fetch";
        case operation_kind::pull:
            return u8"Pull";
        case operation_kind::push_execute:
            return u8"Push";
        case operation_kind::stash_push:
            return u8"Stash";
        case operation_kind::stash_pop:
            return u8"Pop";
        case operation_kind::clone:
            return u8"Clone";
        case operation_kind::init:
            return u8"Init";
        case operation_kind::inspect_path:
            return u8"경로 검사";
        case operation_kind::ssh_keygen:
            return u8"SSH 키 만들기";
        case operation_kind::ssh_connection_test:
            return u8"SSH 연결 시험";
        case operation_kind::submodule_update:
            return u8"서브모듈 업데이트";
        case operation_kind::submodule_sync:
            return u8"서브모듈 url 동기화";
        case operation_kind::worktree_add:
            return u8"worktree 추가";
        case operation_kind::worktree_remove:
            return u8"worktree 제거";
        case operation_kind::worktree_prune:
            return u8"worktree 정리";
        case operation_kind::lfs_pull:
            return u8"LFS pull";
        case operation_kind::lfs_fetch:
            return u8"LFS fetch";
        case operation_kind::lfs_track:
            return u8"LFS 추적";
        case operation_kind::load_rebase_plan:
            return u8"rebase 범위 읽기";
        case operation_kind::interactive_rebase:
            return u8"인터랙티브 rebase";
        case operation_kind::refresh_status:
            return u8"변경 새로 고침";
        case operation_kind::set_upstream:
            return u8"upstream 설정";
        case operation_kind::unset_upstream:
            return u8"upstream 해제";
        case operation_kind::trash_files:
            return u8"휴지통으로 보내기";
        default:
            break;
        }
        return to_u8(operation_kind_name(kind));
    }

    file_row_view make_file_row(const file_change& change, std::u8string key_prefix)
    {
        file_row_view row {};
        row.key = std::move(key_prefix) + to_u8(change.path);
        row.path = change.path;
        row.original_path = change.original_path;
        row.name = name_of(change.path);
        row.folder = change.original_path.empty() ? folder_of(change.path) : u8"← " + to_u8(change.original_path);
        row.status = change.status;
        row.side = change.side;
        row.untracked = change.status == '?';
        row.conflicted = change.status == 'U' || change.conflict_code.empty() == false;
        row.lfs = change.is_lfs;
        row.submodule = change.is_submodule;
        // submodule 포인터는 `Subproject commit` 한 줄이 바뀐 것이라 줄 수가 뜻이 없다.
        if (change.is_submodule == false && change.lines_added.has_value() && *change.lines_added > 0)
            row.added = u8"+" + number(*change.lines_added);
        if (change.is_submodule == false && change.lines_removed.has_value() && *change.lines_removed > 0)
            row.removed = u8"-" + number(*change.lines_removed);
        return row;
    }

    view_snapshot build_view_snapshot(const view_inputs& inputs)
    {
        view_snapshot view {};
        const ui_state& ui { *inputs.ui };
        const app_settings& settings { *inputs.settings };
        const repository_tabs& tabs { *inputs.tabs };
        view.width = inputs.width;
        view.height = inputs.height;
        view.scale = inputs.scale > 0.0f ? inputs.scale : 1.0f;
        view.logical_width = inputs.width / view.scale;
        view.logical_height = inputs.height / view.scale;
        view.mode = select_layout_mode(view.logical_width);
        view.theme = settings.appearance.theme;
        view.graph_layout = settings.ui.graph_layout;
        view.git_version = inputs.git_version;
        view.tab_scroll = ui.tab_scroll;
        view.reveal_selected_tab = ui.reveal_selected_tab;
        view.log_open = ui.operation_log_open;
        view.log_scroll = ui.log_scroll;
        view.sidebar_width = ui.sidebar_width;
        view.detail_width = ui.detail_width;
        view.drawer_open = ui.drawer_open && view.mode != layout_mode::wide;
        const std::int64_t now { std::chrono::duration_cast<std::chrono::seconds>(inputs.now.time_since_epoch()).count() };
        view.now = now;
        const bool git_usable { inputs.git_message.empty() };

        const auto repository_of = [&inputs](const repository_tab_id id) -> const repository_session* {
            if (inputs.tab_repositories == nullptr || inputs.find_session == nullptr)
                return nullptr;
            const auto found { inputs.tab_repositories->find(id) };
            return found == inputs.tab_repositories->end() ? nullptr : inputs.find_session(found->second);
        };

        for (const repository_tab& tab : tabs.tabs())
        {
            tab_view item {};
            item.id = tab.id;
            item.key = u8"tab-" + number(tab.id);
            item.start = tab.kind == repository_tab_kind::start;
            item.label = item.start ? std::u8string { u8"시작" } : tab.display_name;
            item.tooltip = item.start ? std::u8string { u8"시작 페이지" } : tab.path;
            if (const repository_session* session { repository_of(tab.id) }; session != nullptr)
            {
                const std::vector<const operation_status*> mine { operations_of(inputs.operations, session->id()) };
                const repository_summary& summary { session->snapshot().summary };
                // 배너를 닫은 입력 요청은 탭에서도 🔔를 내린다. 대화 상자가 붙기 전에는 사용자가 할 수 있는 일이 없다.
                const auto state { ui.tabs.find(tab.id) };
                const operation_id dismissed { state == ui.tabs.end() ? operation_id { 0 } : state->second.dismissed_banner };
                const bool awaiting { session->awaiting_input().has_value() && *session->awaiting_input() > dismissed };
                item.marker = compute_tab_marker(mine, summary.changed_file_count, awaiting);
                if (summary.in_progress.kind != repository_operation_state::none)
                    item.in_progress = to_u8(repository_operation_state_name(summary.in_progress.kind)) + u8" 중";
            }
            view.tabs.push_back(std::move(item));
        }

        const repository_tab& selected { tabs.selected() };
        view.selected_tab = selected.id;
        const auto tab_state_found { ui.tabs.find(selected.id) };
        static const tab_ui_state empty_tab_state {};
        const tab_ui_state& tab_state { tab_state_found == ui.tabs.end() ? empty_tab_state : tab_state_found->second };

        const repository_session* session { selected.kind == repository_tab_kind::repository ? repository_of(selected.id) : nullptr };
        std::shared_ptr<const graph_index> index {};
        std::vector<const operation_status*> repository_operations {};
        if (session != nullptr)
        {
            const std::vector<const operation_status*> mine { operations_of(inputs.operations, session->id()) };
            repository_operations = mine;
            index = inputs.history_index != nullptr ? inputs.history_index(*session) : std::make_shared<const graph_index>(session->graph_commits());
            view.repository = build_repository(inputs, selected, *session, tab_state, mine, now, index);
            fill_toolbar(view, *session, mine, git_usable);
            fill_status(view, mine, session, ui.operation_log_open);
            if (const operation_status* last { latest_finished_user(mine) }; last != nullptr)
                view.banner = operation_banner(*last, tab_state.dismissed_banner, tab_state.banner_expanded);
        }
        else
        {
            start_page_snapshot start {};
            start.search = luil::make_text_input_view(ui.start_search, ui.composition, to_luil_target(text_target::start_search));
            start.list
                = build_start_page_view(settings, inputs.exists != nullptr ? inputs.exists : repository_exists_function { [](std::u8string_view) { return true; } }, start.search.displayed_text());
            for (const std::u8string& path : tabs.repository_paths())
                start.open_keys.insert(repository_path_key(path));
            start.git_message = inputs.git_message;
            start.git_version = inputs.git_version;
            start.scroll = ui.start_scroll;
            // 저장소 준비 작업(clone·init·경로 검사)이다.
            const std::vector<const operation_status*> setup { operations_of(inputs.operations, std::string {}) };
            if (const std::optional<active_operation_view> active { active_operation(setup) }; active.has_value())
                start.setup_progress = active->text;
            fill_status(view, setup, nullptr, ui.operation_log_open);
            if (const operation_status* last { latest_finished_user(setup) }; last != nullptr && last->kind != operation_kind::clone)
                view.banner = operation_banner(*last, ui.dismissed_start_banner, false);
            view.start = std::move(start);
        }

        // 안내(아직 없는 대화 상자)는 operation 배너보다 앞선다. 사용자가 방금 누른 것의 답이기 때문이다.
        if (ui.notice.has_value())
        {
            view.banner = banner_view { .kind = banner_kind::notice, .title = u8"안내", .summary = ui.notice->text, .warning = ui.notice->warning };
            // 프로필 제안·커밋 전 경고는 조치 단추를 달고 온다.
            for (const notice_action& action : ui.notice->actions)
                view.banner->actions.push_back(banner_action_view { .key = action.key, .label = action.label });
        }

        if (ui.operation_log_open)
            view.log = build_log(inputs.operations);

        // 모달 입력 상자(인증 질문 > 사용자가 연 상자 > 체크아웃 정책)와 우클릭 메뉴다.
        {
            const std::vector<const operation_status*> setup { operations_of(inputs.operations, std::string {}) };
            const tab_ui_state* const tab { session != nullptr ? &tab_state : nullptr };
            const form_source source { resolve_form_source(repository_operations, setup, ui, tab) };
            view.form = build_form_view(source, ui, tab, session != nullptr ? &session->snapshot() : nullptr);
            if (view.form.has_value())
                view.form->scroll = ui.form.scroll;
            view.menu = build_menu_view(ui, tabs, session, index.get(), view.scale);
            view.selector = build_selector_view(ui, tabs, settings, inputs.exists, session, view.scale);
        }

        if (ui.clone.has_value())
        {
            const clone_dialog_state& clone { *ui.clone };
            clone_dialog_view dialog {};
            dialog.url = luil::make_text_input_view(clone.url, ui.composition, to_luil_target(text_target::clone_url));
            dialog.path = luil::make_text_input_view(clone.path, ui.composition, to_luil_target(text_target::clone_path));
            dialog.operation = clone.operation;
            dialog.error = clone.error;
            for (const operation_status* status : inputs.operations)
            {
                if (status == nullptr || status->id != clone.operation)
                    continue;
                dialog.running = status->finished() == false;
                dialog.phase = phase_text(*status);
                dialog.fraction = status->progress.fraction;
                if (status->state == operation_state::failed && status->failure.has_value())
                    dialog.error = status->failure->summary;
            }
            dialog.can_submit = git_usable && dialog.running == false && clone.url.text.empty() == false && clone.path.text.empty() == false;
            view.clone = std::move(dialog);
        }
        return view;
    }
} // namespace cyella
