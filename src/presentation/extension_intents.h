#pragma once

#include "presentation/view_snapshot.h"

#include <string>

namespace cyella {
    // submodule·worktree·Git LFS 화면이 내는 화면 메시지다. `shell_controller_extension.cpp`가 처리한다.

    // 사이드바 SUBMODULES·WORKTREES·GIT LFS 행을 두 번 눌렀거나 Enter를 쳤다. `path`는 행의 `path`다.
    //  - submodule: 초기화되었으면 새 탭으로 열고, 아니면 초기화하고 업데이트한다.
    //  - worktree: 새 탭으로 연다.
    //  - lfs: 아무것도 하지 않는다.
    struct activate_sidebar_item_intent
    {
        sidebar_item_kind item { sidebar_item_kind::submodule };
        std::string path {};
    };

    // diff 카드의 [서브모듈 열기]다. `path`는 저장소 루트 기준 submodule 경로다.
    struct open_submodule_intent
    {
        std::string path {};
    };
} // namespace cyella
