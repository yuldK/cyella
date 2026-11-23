#include "presentation/shell_controller.h"

#include "presentation/menu_model.h"

#include <utility>

// 그래프의 브랜치 필터다 (ADR-012).
//  - 필터의 정본은 저장소 세션이다. 여기서는 메뉴·띠·명령이 고른 것을 workspace의 `set_graph_filter`로 넘기는 것으로 끝낸다.
//    workspace가 refresh를 내고, 걸러 읽은 log가 오면 그래프가 처음부터 다시 배치된다.
//  - 커밋 검색은 같은 ref 집합에서 찾아야 한다. 필터가 달라지면 열린 검색을 새 집합으로 다시 낸다.
namespace cyella {
    bool shell_controller::handle_graph_filter_message(const luil::app_message& message)
    {
        if (message.get<clear_graph_filter_intent>() == nullptr)
            return false;
        apply_graph_filter(graph_ref_filter {});
        return true;
    }

    void shell_controller::toggle_graph_filter(const menu_target& target, const menu_context& context, const bool only)
    {
        const std::optional<std::string> name { graph_filter_ref_of(target, context.refs) };
        if (name.has_value() == false)
            return;
        graph_ref_filter filter { context.graph_filter };
        if (only)
            toggle_graph_ref_only(filter, *name);
        else
            toggle_graph_ref_hidden(filter, *name);
        apply_graph_filter(std::move(filter));
    }

    void shell_controller::apply_graph_filter(graph_ref_filter filter)
    {
        git_workspace* const workspace { dependencies_.workspace };
        const std::string repository { selected_repository_id() };
        if (workspace == nullptr || repository.empty())
            return;
        // 그대로면 아무것도 내지 않는다 (0).
        if (workspace->set_graph_filter(repository, std::move(filter)) == 0)
            return;
        // 그래프가 통째로 바뀐다. 앞 그래프의 스크롤 자리는 새 그래프에서 뜻이 없다.
        if (tab_ui_state* const tab { selected_tab_state() }; tab != nullptr)
            tab->graph_scroll = 0.0f;
        rearm_filtered_searches();
    }

    void shell_controller::rearm_filtered_searches()
    {
        const git_workspace* const workspace { dependencies_.workspace };
        if (workspace == nullptr)
            return;
        for (auto& [id, tab] : ui_.tabs)
        {
            if (tab.search.has_value() == false)
                continue;
            const auto repository { tab_repositories_.find(id) };
            if (repository == tab_repositories_.end())
                continue;
            const repository_session* const session { workspace->find_session(repository->second) };
            if (session == nullptr || session->graph_filter() == tab.search->filter)
                continue;
            commit_search_state& search { *tab.search };
            // 먼저 적어 둔다. 이 함수는 사건을 처리할 때마다 불리므로, 적지 않으면 같은 차이를 보고 거듭 다시 낸다.
            search.filter = session->graph_filter();
            // 세션은 앞 필터로 찾은 결과를 이미 버렸다. 낸 질의를 비워, 글이 그대로여도 다시 내게 한다.
            search.submitted.clear();
            // "지금 자리"는 앞 ref 집합의 결과에서 센 것이다.
            search.current.clear();
            search.due = dependencies_.steady_clock();
        }
    }
} // namespace cyella
