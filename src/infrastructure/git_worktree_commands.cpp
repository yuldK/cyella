#include "infrastructure/git_worktree_commands.h"

#include "infrastructure/git_extension_arguments.h"

namespace cyella {
    using namespace git_extension;

    git_arguments make_git_worktree_list_args()
    {
        git_arguments arguments {};
        push_text(arguments, "worktree");
        push_text(arguments, "list");
        push_text(arguments, "--porcelain");
        return arguments;
    }

    git_arguments make_git_worktree_add_args(const git_worktree_add_options& options)
    {
        git_arguments arguments {};
        push_text(arguments, "worktree");
        push_text(arguments, "add");
        const bool creates_branch { options.new_branch.empty() == false };
        if (creates_branch)
        {
            push_text(arguments, "-b");
            push_text(arguments, require_new_ref_name(options.new_branch, "new branch"));
        }
        else if (options.detach)
        {
            push_text(arguments, "--detach");
        }
        push_text(arguments, "--");
        push_text(arguments, require_path(options.path));
        if (creates_branch || options.detach)
        {
            if (options.start_point.empty() == false)
                push_text(arguments, require_revision(options.start_point, "start point"));
        }
        else if (options.branch.empty() == false)
        {
            push_text(arguments, require_revision(options.branch, "branch"));
        }
        return arguments;
    }

    git_arguments make_git_worktree_remove_args(const std::string_view path, const bool force, const bool locked)
    {
        git_arguments arguments {};
        push_text(arguments, "worktree");
        push_text(arguments, "remove");
        if (force || locked)
        {
            push_text(arguments, "--force");
            if (locked)
                push_text(arguments, "--force");
        }
        push_text(arguments, "--");
        push_text(arguments, require_path(path));
        return arguments;
    }

    git_arguments make_git_worktree_prune_args()
    {
        git_arguments arguments {};
        push_text(arguments, "worktree");
        push_text(arguments, "prune");
        push_text(arguments, "--verbose");
        return arguments;
    }
} // namespace cyella
