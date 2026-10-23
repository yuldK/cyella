#include "presentation/file_tree.h"
#include "presentation/shell_controller.h"

#include <algorithm>
#include <optional>
#include <set>
#include <string_view>

// 작업 영역의 변경 파일을 여러 개 고르는 화면 쪽 규칙이다 (탐색기와 같은 몸짓).
//  - 그냥 클릭: 그 파일 하나를 고르고 diff를 연다. 그 파일이 Shift의 기준이 된다.
//  - Ctrl+클릭: 하나 더하거나 뺀다. diff는 더한 파일이고, 보이던 파일을 빼면 남은 것 중 마지막에 고른 파일이다.
//  - Shift+클릭: 기준부터 누른 파일까지 화면에 보이는 순서(트리면 펼친 폴더의 순서)의 범위다. Ctrl+Shift는 그 범위를 더한다.
//  - 고르기는 한 묶음(충돌·스테이징 전·커밋에 포함) 안에서다. 다른 묶음의 파일을 Ctrl·Shift로 누르면 그 파일 하나로 다시 시작한다 —
//    stage와 unstage를 한 번에 할 수 없으니 섞인 선택은 메뉴가 할 일이 없다.
namespace cyella {
    namespace {
        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::string to_text(const std::u8string_view text)
        {
            return std::string { text.begin(), text.end() };
        }

        // 행 키의 묶음 접두다 (`u:`). 작업 영역 파일 행의 키가 아니면 비어 있다.
        std::u8string_view group_of(const std::u8string_view key) noexcept
        {
            if (key.size() > 2 && (key.starts_with(u8"c:") || key.starts_with(u8"u:") || key.starts_with(u8"s:")))
                return key.substr(0, 2);
            return {};
        }

        const std::vector<file_change>& group_changes(const repository_snapshot& snapshot, const std::u8string_view group) noexcept
        {
            if (group == u8"c:")
                return snapshot.conflicts;
            return group == u8"s:" ? snapshot.staged : snapshot.unstaged;
        }

        // 묶음 안의 파일 행 키를 화면에 보이는 순서로 늘어놓는다. 트리면 접힌 폴더 아래의 파일은 빠진다 (화면의 범위와 같다).
        std::vector<std::u8string> working_order(const repository_snapshot& snapshot, const tab_ui_state& tab, const std::u8string_view group, const bool tree)
        {
            std::vector<std::string> paths {};
            for (const file_change& change : group_changes(snapshot, group))
                paths.push_back(change.path);
            const std::u8string prefix { group };
            std::vector<std::u8string> keys {};
            if (tree == false)
            {
                for (const std::string& path : paths)
                    keys.push_back(prefix + to_u8(path));
                return keys;
            }
            // 작업 영역 트리의 접힌 폴더는 묶음 접두를 붙여 기억한다 (`repository_ui`의 작업 영역 목록과 같은 규칙).
            const std::string text_prefix { to_text(prefix) };
            std::set<std::string> collapsed {};
            for (const std::string& path : tab.collapsed_folders)
                if (path.starts_with(text_prefix))
                    collapsed.insert(path.substr(text_prefix.size()));
            for (const file_tree_row& node : build_file_tree(paths, collapsed))
                if (node.folder == false && node.file_index < paths.size())
                    keys.push_back(prefix + to_u8(paths[node.file_index]));
            return keys;
        }

        // 행 키가 여는 diff다. 그 파일이 지금 묶음에 없으면 값이 없다.
        std::optional<diff_file_request> working_request(const repository_snapshot& snapshot, const std::u8string_view key)
        {
            const std::u8string_view group { group_of(key) };
            if (group.empty())
                return std::nullopt;
            const std::string path { to_text(key.substr(2)) };
            for (const file_change& change : group_changes(snapshot, group))
                if (change.path == path)
                    return diff_file_request { .path = path, .view = group == u8"s:" ? diff_view::staged : diff_view::unstaged, .sha = {}, .untracked = change.status == '?' };
            return std::nullopt;
        }

        // 지금 보이는 작업 영역 diff의 행 키다. 충돌 파일도 unstaged 보기라 충돌 묶음을 먼저 본다.
        std::optional<std::u8string> shown_key(const repository_snapshot& snapshot, const std::optional<diff_file_request>& diff)
        {
            if (diff.has_value() == false || (diff->view != diff_view::unstaged && diff->view != diff_view::staged))
                return std::nullopt;
            const auto contains
                = [&diff](const std::vector<file_change>& changes) { return std::any_of(changes.begin(), changes.end(), [&diff](const file_change& change) { return change.path == diff->path; }); };
            if (diff->view == diff_view::staged)
                return contains(snapshot.staged) ? std::optional<std::u8string> { u8"s:" + to_u8(diff->path) } : std::nullopt;
            if (contains(snapshot.conflicts))
                return u8"c:" + to_u8(diff->path);
            return contains(snapshot.unstaged) ? std::optional<std::u8string> { u8"u:" + to_u8(diff->path) } : std::nullopt;
        }
    } // namespace

    void shell_controller::open_file_diff(tab_ui_state& tab, diff_file_request request)
    {
        // 문맥 줄 수는 환경 설정이 정한다. 행이 만든 요청에는 기본값이 들어 있다.
        request.context_lines = settings().diff.context_lines;
        tab.diff = request;
        tab.diff_scroll = 0.0f;
        static_cast<void>(submit(std::move(request)));
        // medium·narrow에서는 상세가 그래프 자리 위에 뜬 서랍이라, 그대로 두면 방금 연 diff를 서랍이 덮는다.
        // diff를 가리지 않게 서랍을 닫는다. 파일 목록은 툴바의 "상세 패널" 단추로 다시 연다.
        if (last_mode_ != layout_mode::wide)
            ui_.drawer_open = false;
    }

    void shell_controller::handle_select_file(const select_file_intent& intent)
    {
        tab_ui_state* const tab { selected_tab_state() };
        if (tab == nullptr)
            return;
        const std::u8string_view group { group_of(intent.key) };
        repository_session* const session { selected_session() };
        const auto select_one = [&] {
            tab->working_selection = group.empty() ? std::vector<std::u8string> {} : std::vector<std::u8string> { intent.key };
            tab->working_anchor = group.empty() ? std::u8string {} : intent.key;
            open_file_diff(*tab, intent.request);
        };
        if (group.empty() || session == nullptr || (intent.control == false && intent.shift == false))
        {
            select_one();
            return;
        }

        const repository_snapshot& snapshot { session->snapshot() };
        std::vector<std::u8string> selection { tab->active_working_selection() };
        std::u8string anchor { tab->working_anchor };
        // 다른 길(메뉴·diff 머리)로 연 diff는 선택을 남기지 않는다. 그때는 보이는 파일이 선택이자 기준이다.
        if (selection.empty())
            if (const std::optional<std::u8string> shown { shown_key(snapshot, tab->diff) }; shown.has_value())
            {
                selection = { *shown };
                anchor = *shown;
            }
        if (selection.empty() || group_of(selection.front()) != group)
        {
            select_one();
            return;
        }
        if (group_of(anchor) != group)
            anchor = selection.back();

        if (intent.shift)
        {
            const std::vector<std::u8string> order { working_order(snapshot, *tab, group, settings().ui.working_files_tree) };
            const auto from { std::find(order.begin(), order.end(), anchor) };
            const auto to { std::find(order.begin(), order.end(), intent.key) };
            if (from == order.end() || to == order.end())
            {
                select_one();
                return;
            }
            const auto [first, last] { std::minmax(from, to) };
            if (intent.control == false)
                selection.clear();
            for (auto it { first }; it <= last; ++it)
                if (std::find(selection.begin(), selection.end(), *it) == selection.end())
                    selection.push_back(*it);
            tab->working_selection = std::move(selection);
            tab->working_anchor = anchor;
            open_file_diff(*tab, intent.request);
            return;
        }

        tab->working_anchor = intent.key;
        const auto found { std::find(selection.begin(), selection.end(), intent.key) };
        if (found == selection.end())
        {
            selection.push_back(intent.key);
            tab->working_selection = std::move(selection);
            open_file_diff(*tab, intent.request);
            return;
        }
        selection.erase(found);
        tab->working_selection = selection;
        // 마지막 하나까지 뺐으면 고른 것이 없다. diff도 닫는다.
        if (selection.empty())
        {
            tab->diff.reset();
            return;
        }
        // 보이던 파일을 뺐으면 남은 것 중 마지막에 고른 파일을 보인다.
        if (shown_key(snapshot, tab->diff) == intent.key)
            if (std::optional<diff_file_request> request { working_request(snapshot, selection.back()) }; request.has_value())
                open_file_diff(*tab, std::move(*request));
    }

    std::vector<std::string> shell_controller::working_menu_paths(const tab_ui_state& tab, const menu_target& target) const
    {
        if (target.kind != menu_target_kind::file)
            return {};
        const std::u8string key { (target.conflicted ? u8"c:" : target.side == change_side::staged ? u8"s:" : u8"u:") + to_u8(target.name) };
        const std::vector<std::u8string> selection { tab.active_working_selection() };
        if (selection.size() < 2 || std::find(selection.begin(), selection.end(), key) == selection.end())
            return {};
        std::vector<std::string> paths {};
        for (const std::u8string& chosen : selection)
            paths.push_back(to_text(std::u8string_view { chosen }.substr(2)));
        return paths;
    }

    discard_files_intent shell_controller::discard_intent_for(const std::vector<std::string>& paths, const menu_target& target)
    {
        // 파일 하나는 우클릭한 행이 이미 안다.
        if (paths.size() == 1)
            return target.untracked ? discard_files_intent { .paths = {}, .untracked = paths } : discard_files_intent { .paths = paths, .untracked = {} };
        discard_files_intent intent {};
        repository_session* const session { selected_session() };
        for (const std::string& path : paths)
        {
            bool untracked { false };
            if (session != nullptr)
                for (const file_change& change : session->snapshot().unstaged)
                    if (change.path == path)
                        untracked = change.status == '?';
            (untracked ? intent.untracked : intent.paths).push_back(path);
        }
        return intent;
    }
} // namespace cyella
