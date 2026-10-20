#include "infrastructure/git_lfs_commands.h"

#include "infrastructure/git_extension_arguments.h"

#include <initializer_list>

namespace cyella {
    using namespace git_extension;

    namespace {
        git_arguments make_command(const std::initializer_list<std::string_view> words)
        {
            git_arguments arguments {};
            for (const std::string_view word : words)
                push_text(arguments, word);
            return arguments;
        }
    } // namespace

    git_arguments make_git_lfs_version_args()
    {
        return make_command({ "lfs", "version" });
    }

    git_arguments make_git_lfs_ls_files_json_args()
    {
        return make_command({ "lfs", "ls-files", "--json" });
    }

    git_arguments make_git_lfs_ls_files_long_args()
    {
        return make_command({ "lfs", "ls-files", "--long" });
    }

    git_arguments make_git_lfs_install_local_args()
    {
        return make_command({ "lfs", "install", "--local" });
    }

    git_arguments make_git_lfs_filter_probe_args()
    {
        return make_command({ "config", "--get", "filter.lfs.smudge" });
    }

    git_arguments make_git_lfs_pull_args()
    {
        return make_command({ "lfs", "pull" });
    }

    git_arguments make_git_lfs_fetch_args(const bool all)
    {
        git_arguments arguments { make_command({ "lfs", "fetch" }) };
        if (all)
            push_text(arguments, "--all");
        return arguments;
    }

    git_arguments make_git_lfs_track_args(const std::string_view pattern)
    {
        if (pattern.empty() || pattern.find_first_of(std::string_view { "\r\n\0", 3 }) != std::string_view::npos)
            reject("pattern", "pattern must be a single non-empty line");
        git_arguments arguments { make_command({ "lfs", "track", "--" }) };
        push_text(arguments, pattern);
        return arguments;
    }

    git_arguments make_git_check_attr_filter_args()
    {
        return make_command({ "check-attr", "-z", "--stdin", "filter" });
    }
} // namespace cyella
