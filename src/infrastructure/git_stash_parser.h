#pragma once

#include "domain/stash_entry.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace cyella {
    struct git_stash_list
    {
        std::vector<stash_entry> entries {};
        // 모양이 맞지 않아 건너뛴 레코드 수다.
        std::size_t skipped_count { 0 };
    };

    // `git stash list -z --format=%gd%x1f%H%x1f%P%x1f%ct%x1f%gs`의 출력을 해석한다.
    // 형식은 git_command_builder의 make_git_stash_list_args가 정한다.
    //
    // 메시지의 `WIP on <branch>: ...`와 `On <branch>: ...`는 git이 stash를 만들 때 쓴
    // 고정 문구다. 브랜치 이름과 메시지를 나누는 데 쓰며, 다른 모양이면 branch를
    // 비워 두고 문구 전체를 message로 둔다.
    [[nodiscard]] git_stash_list parse_git_stash_list(std::string_view bytes);
} // namespace cyella
