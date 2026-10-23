#include "presentation/shell_controller.h"

#include "domain/commit_graph_layout.h"

#include <algorithm>

// 그래프에서 여러 커밋을 골라 비교하는 화면 쪽 규칙이다.
//  - Ctrl+클릭: 기준 커밋과 누른 커밋을 비교한다 (`git diff 오래된 새것`). 다른 갈래의 커밋끼리도 된다.
//  - Shift+클릭: 한쪽이 다른 쪽의 조상이면 그 사이 범위를 합친 변경이다 (`git diff 오래된^ 새것`). 아니면 Ctrl과 같다.
//  - 기준은 처음 Ctrl·Shift+클릭 직전에 골라 둔 커밋이고, 거듭 누르면 마지막에 누른 커밋만 바뀐다.
namespace cyella {
    bool shell_controller::select_compare(tab_ui_state& tab, repository_session& session, const commit_info& clicked, const bool shift)
    {
        // WIP 행은 비교에 넣지 않는다. 기준이 없거나 기준을 다시 누르면 그냥 클릭으로 다룬다 (비교가 풀리고 그 커밋 하나다).
        const std::string anchor { tab.compare.has_value() ? tab.compare->anchor : tab.selected_sha };
        if (clicked.sha == graph_wip_sha || anchor.empty() || anchor == graph_wip_sha || anchor == clicked.sha)
            return false;
        if (history_index(session)->find(anchor).has_value() == false)
            return false;

        tab.compare = commit_compare_state { .anchor = anchor, .kind = shift ? commit_compare_kind::undecided : commit_compare_kind::pair, .request = {}, .ancestry_query = 0 };
        tab.selected_sha = clicked.sha;
        tab.history_sha = clicked.sha;
        tab.diff.reset();
        tab.detail_scroll = 0.0f;
        tab.commit_meta_scroll = 0.0f;
        // 마지막에 누른 커밋 하나의 상세·서명·CI도 읽어 둔다. Esc로 비교를 풀면 그 커밋이 바로 보인다.
        static_cast<void>(submit_to(session.id(), show_commit_request { clicked.sha }));
        request_signatures({ clicked.sha });
        hosting_.on_commit_selected(session.id(), clicked.sha);
        decide_compare(tab, session);
        // medium·narrow에서는 상세가 서랍이라 고르면 연다 (커밋 하나를 고를 때와 같다).
        if (last_mode_ != layout_mode::wide)
            ui_.drawer_open = true;
        return true;
    }

    void shell_controller::decide_compare(tab_ui_state& tab, repository_session& session)
    {
        commit_compare_state& compare { *tab.compare };
        const std::shared_ptr<const graph_index> index { history_index(session) };
        const std::optional<std::size_t> anchor_at { index->find(compare.anchor) };
        const std::optional<std::size_t> clicked_at { index->find(tab.selected_sha) };
        const std::vector<commit_info>& commits { session.graph_commits() };
        if (anchor_at.has_value() == false || clicked_at.has_value() == false || std::max(*anchor_at, *clicked_at) >= commits.size())
        {
            tab.compare.reset();
            return;
        }
        // 그래프는 자식이 부모보다 앞인 순서다. 뒤에 있는 쪽이 오래된 커밋이고 왼쪽에 선다.
        const commit_info& older { commits[std::max(*anchor_at, *clicked_at)] };
        const commit_info& newer { commits[std::min(*anchor_at, *clicked_at)] };
        if (compare.kind == commit_compare_kind::undecided)
        {
            std::optional<bool> ancestor { index->is_ancestor(older.sha, newer.sha) };
            if (ancestor.has_value() == false)
                ancestor = session.ancestry(older.sha, newer.sha);
            if (ancestor.has_value() == false)
            {
                // 그래프로 답할 수 없으면 git에게 한 번 묻는다. 답이 오면 `sync_compares`가 이어서 정한다.
                if (compare.ancestry_query == 0)
                {
                    compare.ancestry_query = submit_to(session.id(), is_ancestor_request { .ancestor = older.sha, .descendant = newer.sha });
                    if (compare.ancestry_query != 0)
                        return;
                }
                // 물을 수 없었거나 답 없이 끝났다 (실패·취소).
                ancestor = false;
            }
            compare.kind = *ancestor ? commit_compare_kind::range : commit_compare_kind::pair;
        }
        show_range_request request { .base = older.sha, .tip = newer.sha, .range = false };
        if (compare.kind == commit_compare_kind::range)
        {
            // 범위의 왼쪽은 가장 오래된 커밋의 첫 부모다. 루트 커밋이면 빈 트리와 비교한다 (실행기가 해시를 구한다).
            request.base = older.parents.empty() ? std::string {} : older.parents.front();
            request.range = true;
        }
        compare.request = request;
        static_cast<void>(submit_to(session.id(), std::move(request)));
    }

    void shell_controller::clear_compare(tab_ui_state& tab)
    {
        if (tab.compare.has_value() == false)
            return;
        tab.compare.reset();
        // 비교의 파일 diff는 커밋 하나의 상세에 이어지지 않는다.
        if (tab.diff.has_value() && tab.diff->view == diff_view::range)
            tab.diff.reset();
        tab.detail_scroll = 0.0f;
    }

    void shell_controller::sync_compares()
    {
        git_workspace* const workspace { dependencies_.workspace };
        if (workspace == nullptr)
            return;
        for (auto& [tab_id, tab] : ui_.tabs)
        {
            if (tab.compare.has_value() == false)
                continue;
            const auto repository { tab_repositories_.find(tab_id) };
            repository_session* const session { repository != tab_repositories_.end() ? workspace->find_session(repository->second) : nullptr };
            if (session == nullptr)
            {
                clear_compare(tab);
                continue;
            }
            // 새로 고침이 끝나 그래프가 바뀌었는데 고른 커밋이 없으면 비교를 푼다. 새로 고침의 첫 쪽이 오기 전에는 옛 그래프가 그대로 보인다.
            if (session->requested_generation() == session->displayed_generation() && session->log_page_loading() == false)
            {
                const std::shared_ptr<const graph_index> index { history_index(*session) };
                if (index->find(tab.compare->anchor).has_value() == false || index->find(tab.selected_sha).has_value() == false)
                {
                    clear_compare(tab);
                    continue;
                }
            }
            if (tab.compare->kind != commit_compare_kind::undecided || tab.compare->ancestry_query == 0)
                continue;
            const operation_status* const status { workspace->find_operation(tab.compare->ancestry_query) };
            if (status != nullptr && status->finished() == false)
                continue;
            // 답이 세션에 들어 있으면 그것으로, 없이 끝났으면(실패·취소) 두 커밋 비교로 정한다.
            decide_compare(tab, *session);
        }
    }
} // namespace cyella
