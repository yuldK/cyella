#pragma once

#include "infrastructure/git_command_builder.h"

#include <string>
#include <string_view>

namespace cyella {
    // `worktree list --porcelain`.
    [[nodiscard]] git_arguments make_git_worktree_list_args();

    struct git_worktree_add_options
    {
        // 새 작업 트리의 폴더다. 절대 경로를 권한다.
        std::string path {};
        // 이미 있는 로컬 브랜치를 체크아웃한다. new_branch와 detach보다 약하다.
        std::string branch {};
        // 비우지 않으면 이 이름의 새 브랜치를 start_point에서 만들어 체크아웃한다(`-b`).
        std::string new_branch {};
        // new_branch나 detach의 시작 커밋이다. 비우면 HEAD다.
        std::string start_point {};
        // 브랜치 없이 시작 커밋을 detached로 체크아웃한다.
        bool detach { false };
    };

    // `worktree add [-b <new> | --detach] -- <path> [<branch | start_point>]`.
    [[nodiscard]] git_arguments make_git_worktree_add_args(const git_worktree_add_options& options);

    // `worktree remove [--force [--force]] -- <path>`. force는 변경이 있는 작업 트리를 지우고, 잠긴 작업 트리는
    // `--force`가 두 번 있어야 지워지므로 locked일 때 둘을 붙인다.
    [[nodiscard]] git_arguments make_git_worktree_remove_args(std::string_view path, bool force, bool locked);

    // `worktree prune --verbose`.
    [[nodiscard]] git_arguments make_git_worktree_prune_args();
} // namespace cyella
