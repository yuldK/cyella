#include "infrastructure/git_extension_operations.h"

#include "domain/path_syntax.h"
#include "infrastructure/git_worktree_parser.h"

#include <type_traits>
#include <utility>

namespace cyella {
    std::optional<operation_outcome> run_extension_operation(git_operation_context& context)
    {
        return std::visit(
            [&context](const auto& request) -> std::optional<operation_outcome> {
                using request_type = std::decay_t<decltype(request)>;
                if constexpr (std::is_same_v<request_type, submodule_update_request>)
                    return run_submodule_update(context, request);
                else if constexpr (std::is_same_v<request_type, submodule_sync_request>)
                    return run_submodule_sync(context, request);
                else if constexpr (std::is_same_v<request_type, worktree_add_request>)
                    return run_worktree_add(context, request);
                else if constexpr (std::is_same_v<request_type, worktree_remove_request>)
                    return run_worktree_remove(context, request);
                else if constexpr (std::is_same_v<request_type, worktree_prune_request>)
                    return run_worktree_prune(context);
                else if constexpr (std::is_same_v<request_type, lfs_pull_request>)
                    return run_lfs_pull(context);
                else if constexpr (std::is_same_v<request_type, lfs_fetch_request>)
                    return run_lfs_fetch(context, request);
                else if constexpr (std::is_same_v<request_type, lfs_track_request>)
                    return run_lfs_track(context, request);
                else
                    return std::nullopt;
            },
            context.job().operation);
    }

    void read_extension_snapshot(git_operation_context& context, repository_snapshot& snapshot)
    {
        snapshot.worktrees = read_worktrees(context);
        apply_worktree_branches(snapshot.refs, snapshot.worktrees, normalize_path_key(context.job().repository_path));
        snapshot.submodules = read_submodules(context, snapshot);
        snapshot.lfs = read_lfs_state(context);
        if (snapshot.lfs.in_use)
        {
            mark_lfs_changes(context, snapshot.staged);
            mark_lfs_changes(context, snapshot.unstaged);
            mark_lfs_changes(context, snapshot.conflicts);
        }
    }
} // namespace cyella
