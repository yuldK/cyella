#pragma once

#include <string>

namespace cyella {
    // `git worktree list --porcelain`의 항목 하나다. 같은 저장소에서 여러 폴더에 체크아웃한 작업 트리다.
    struct worktree_info
    {
        // 작업 트리 폴더다(`/` 구분, git이 준 그대로).
        std::string path {};
        std::string head_sha {};
        // 체크아웃된 브랜치의 전체 이름(`refs/heads/feat`)이다. detached이거나 bare면 비어 있다.
        std::string branch_ref {};
        // 짧은 이름(`feat`)이다.
        std::string branch {};
        bool detached { false };
        bool bare { false };
        bool locked { false };
        std::string lock_reason {};
        // 폴더가 사라지는 등 `git worktree prune`이 지울 수 있다.
        bool prunable { false };
        std::string prunable_reason {};
        // 목록의 첫 항목, 곧 주 작업 트리다. 지울 수 없다.
        bool is_main { false };
        // 지금 연 탭의 작업 트리다.
        bool is_current { false };
    };
} // namespace cyella
