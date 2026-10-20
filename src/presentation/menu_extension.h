#pragma once

#include "presentation/menu_model.h"

namespace cyella {
    // submodule·worktree·Git LFS 대상의 우클릭 메뉴다. `build_context_menu`가 새 대상 종류를 여기로 넘긴다.
    // 대상의 경로(`target.name`)로 `menu_context`의 submodule·worktree 목록에서 현재 상태를 다시 찾으므로, 메뉴를 연 뒤
    // 상태가 바뀌어도 고른 키를 실행하는 쪽이 같은 규칙으로 항목을 되찾는다.
    //
    // 항목 키
    //  - submodule: `open`, `update`, `copy_path`
    //  - SUBMODULES 머리: `update_all`, `sync_all`
    //  - worktree: `open`, `remove`, `copy_path`
    //  - WORKTREES 머리: `add`, `prune`
    //  - GIT LFS 머리: `pull`, `fetch`, `fetch_all`, `track`
    //  - LFS 파일: `pull`, `copy_path`
    [[nodiscard]] menu_model build_extension_menu(const menu_target& target, const menu_context& context);

    // 파일 경로의 확장자로 만든 추적 패턴이다 (`art/logo.PSD` → `*.PSD`). 확장자가 없으면 빈 문자열이다.
    [[nodiscard]] std::string lfs_pattern_for_path(const std::string& path);
} // namespace cyella
