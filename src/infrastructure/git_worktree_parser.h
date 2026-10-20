#pragma once

#include "domain/ref.h"
#include "domain/worktree.h"

#include <span>
#include <string_view>
#include <vector>

namespace cyella {
    // `git worktree list --porcelain`의 출력을 해석한다. 항목은 빈 줄로 나뉘고, 첫 항목이 주 작업 트리다.
    //
    //   worktree <경로>
    //   HEAD <sha>
    //   branch refs/heads/<이름>   (detached면 `detached`, bare 저장소면 `bare`)
    //   locked [<이유>]
    //   prunable [<이유>]
    //
    // 모르는 줄은 건너뛴다(앞으로 git이 줄을 더해도 깨지지 않는다). `is_current`는 채우지 않는다.
    [[nodiscard]] std::vector<worktree_info> parse_git_worktree_list(std::string_view bytes);

    // 로컬 브랜치 ref마다 다른 worktree에 체크아웃되어 있으면 `worktree_path`를 채운다. `current_path_key`와 같은
    // 키(`normalize_path_key`)의 worktree는 이 저장소 자신이므로 건너뛴다. 비교는 대소문자와 구분자를 무시한다.
    void apply_worktree_branches(std::span<ref> refs, std::span<const worktree_info> worktrees, std::u8string_view current_path_key);
} // namespace cyella
