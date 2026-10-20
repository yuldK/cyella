#include "infrastructure/git_submodule_commands.h"

#include "infrastructure/git_extension_arguments.h"

namespace cyella {
    using namespace git_extension;

    git_arguments make_git_submodule_status_args()
    {
        git_arguments arguments {};
        push_text(arguments, "submodule");
        push_text(arguments, "status");
        push_text(arguments, "--recursive");
        return arguments;
    }

    git_arguments make_git_gitmodules_config_args()
    {
        git_arguments arguments {};
        for (const std::string_view value : { "config", "-f", ".gitmodules", "-z", "--get-regexp", "^submodule[.]" })
            push_text(arguments, value);
        return arguments;
    }

    git_arguments make_git_gitlinks_args(const std::span<const std::string> paths)
    {
        if (paths.empty())
            reject("paths", "no paths given");
        git_arguments arguments {};
        for (const std::string_view value : { "--literal-pathspecs", "ls-files", "--stage", "-z" })
            push_text(arguments, value);
        push_optional_paths(arguments, paths);
        return arguments;
    }

    git_arguments make_git_submodule_update_args(const git_submodule_update_options& options)
    {
        git_arguments arguments {};
        push_text(arguments, "submodule");
        push_text(arguments, "update");
        if (options.init)
            push_text(arguments, "--init");
        if (options.recursive)
            push_text(arguments, "--recursive");
        push_text(arguments, "--progress");
        push_optional_paths(arguments, options.paths);
        return arguments;
    }

    git_arguments make_git_submodule_sync_args(const bool recursive, const std::span<const std::string> paths)
    {
        git_arguments arguments {};
        push_text(arguments, "submodule");
        push_text(arguments, "sync");
        if (recursive)
            push_text(arguments, "--recursive");
        push_optional_paths(arguments, paths);
        return arguments;
    }
} // namespace cyella
