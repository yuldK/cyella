#include "presentation/rebase/rebase_editor_intents.h"
#include "presentation/shell_controller.h"

#include <algorithm>
#include <utility>

namespace cyella {
    void shell_controller::open_rebase_editor(const std::string& onto, const std::string& branch, std::u8string onto_label, std::u8string branch_label)
    {
        repository_session* const session { selected_session() };
        if (session == nullptr)
            return;
        ui_.menu.reset();
        const repository_snapshot& snapshot { session->snapshot() };
        open_rebase_editor_intent intent {};
        intent.repository_id = session->id();
        intent.onto = onto;
        intent.branch = branch;
        intent.onto_label = std::move(onto_label);
        intent.branch_label = std::move(branch_label);
        // 추적하는 파일의 변경만 rebase를 막는다. 추적하지 않는 파일은 git이 그대로 둔다.
        intent.worktree_dirty = snapshot.staged.empty() == false || std::ranges::any_of(snapshot.unstaged, [](const file_change& change) { return change.status != '?'; });
        rebase_.open(intent);
    }

    bool shell_controller::handle_rebase_entry(const luil::app_message& message)
    {
        const auto* const here { message.get<open_interactive_rebase_here_intent>() };
        if (here == nullptr)
            return false;
        repository_session* const session { selected_session() };
        if (session == nullptr || here->sha.empty())
            return true;
        const repository_summary& summary { session->snapshot().summary };
        // HEAD 커밋 위에는 편집할 커밋이 없다 (메뉴 항목과 같은 조건이다).
        if (here->sha == summary.head_sha)
            return true;
        const std::string current { summary.is_detached ? std::string {} : summary.current_branch };
        const std::u8string label { current.empty() ? std::u8string { u8"HEAD" } : std::u8string { current.begin(), current.end() } };
        const std::string_view sha { here->sha };
        const std::string_view shown { sha.substr(0, std::min<std::size_t>(sha.size(), 7)) };
        open_rebase_editor(here->sha, current, std::u8string { shown.begin(), shown.end() }, label);
        return true;
    }
} // namespace cyella
