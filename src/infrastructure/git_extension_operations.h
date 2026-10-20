#pragma once

#include "infrastructure/git_operation_context.h"

#include <optional>
#include <string_view>
#include <vector>

// submodule·worktree·Git LFS의 operation과 refresh가 읽는 값이다. `git_operation_runner`의 구현 파일끼리만 쓰는
// 내부 헤더이며 새 operation은 여기서 나뉜다 — 기존 읽기·쓰기 분기를 늘리지 않으려는 것이다.
namespace cyella {
    // submodule·worktree·Git LFS 요청이면 실행해서 끝맺음을 돌려주고, 아니면 값이 없다. `run_operation`이 lane 분기 전에 부른다.
    [[nodiscard]] std::optional<operation_outcome> run_extension_operation(git_operation_context& context);

    // refresh가 snapshot에 덧붙이는 값이다. refs와 staged·unstaged·conflicts를 채운 **뒤에** 불러야 한다(worktree 표시와
    // submodule의 내용 변경·LFS 표시가 그 값을 읽는다). 읽기에 실패해도 refresh를 멈추지 않고 그 항목을 비워 둔다.
    void read_extension_snapshot(git_operation_context& context, repository_snapshot& snapshot);

    // ---- submodule (git_submodule_operations.cpp)
    [[nodiscard]] std::vector<submodule_info> read_submodules(git_operation_context& context, const repository_snapshot& snapshot);
    [[nodiscard]] operation_outcome run_submodule_update(git_operation_context& context, const submodule_update_request& request);
    [[nodiscard]] operation_outcome run_submodule_sync(git_operation_context& context, const submodule_sync_request& request);

    // ---- worktree (git_worktree_operations.cpp)
    // 주 작업 트리가 첫 항목이다. `is_current`는 이 job의 저장소 경로로 채운다. 실패하면 비어 있다.
    [[nodiscard]] std::vector<worktree_info> read_worktrees(git_operation_context& context);
    [[nodiscard]] operation_outcome run_worktree_add(git_operation_context& context, const worktree_add_request& request);
    [[nodiscard]] operation_outcome run_worktree_remove(git_operation_context& context, const worktree_remove_request& request);
    [[nodiscard]] operation_outcome run_worktree_prune(git_operation_context& context);

    // ---- Git LFS (git_lfs_operations.cpp)
    // 저장소가 LFS를 쓰는지(`.gitattributes`의 `filter=lfs`)만 먼저 본다. 쓰지 않으면 git-lfs를 실행하지 않는다.
    [[nodiscard]] bool repository_uses_lfs(git_operation_context& context);
    [[nodiscard]] lfs_repository_state read_lfs_state(git_operation_context& context);
    // 경로가 `filter=lfs`면 `is_lfs`를 켠다. LFS를 쓰는 저장소에서만 부른다.
    void mark_lfs_changes(git_operation_context& context, std::vector<file_change>& changes);
    // diff 한 파일이 LFS 포인터의 diff면 `lfs`를 채운다. 포인터를 읽는 것만으로 알 수 있고, 실제 내용이 로컬에 있는지는
    // 객체 저장소와 작업 트리 파일로 판정한다.
    void annotate_lfs_diff(git_operation_context& context, const diff_file_request& request, std::vector<diff_file>& files);
    [[nodiscard]] operation_outcome run_lfs_pull(git_operation_context& context);
    [[nodiscard]] operation_outcome run_lfs_fetch(git_operation_context& context, const lfs_fetch_request& request);
    [[nodiscard]] operation_outcome run_lfs_track(git_operation_context& context, const lfs_track_request& request);
} // namespace cyella
