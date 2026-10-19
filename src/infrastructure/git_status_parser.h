#pragma once

#include "domain/file_change.h"
#include "domain/repository_summary.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // `# branch.*` 머리말이 주는 브랜치 상태다.
    struct git_branch_status
    {
        // 첫 커밋 전(`(initial)`)이면 비어 있다.
        std::string oid {};
        bool is_initial { false };
        // detached이면 비어 있다.
        std::string head {};
        bool is_detached { false };
        std::string upstream {};
        // `branch.ab`는 upstream이 있고 추적 ref가 살아 있을 때만 나온다.
        bool has_ahead_behind { false };
        std::uint32_t ahead { 0 };
        std::uint32_t behind { 0 };
    };

    struct git_status_result
    {
        git_branch_status branch {};
        // index 쪽 변경이다. 이름 변경·복사는 original_path가 채워진다.
        std::vector<file_change> staged {};
        // 작업 트리 쪽 변경이다. untracked는 상태 `?`로 여기에 들어간다.
        std::vector<file_change> unstaged {};
        // 병합 충돌 파일이다. staged·unstaged에는 넣지 않는다.
        std::vector<file_change> conflicts {};
        // `--ignored`로 요청했을 때만 나온다(상태 `!`).
        std::vector<file_change> ignored {};
        // 모양이 맞지 않아 건너뛴 항목 수다. 0이 아니면 git 출력 형식이 바뀐 것이다.
        std::size_t skipped_entry_count { 0 };
    };

    // `git status --porcelain=v2 --branch -z --untracked-files=all [--ignored]`의 출력을
    // 해석한다. 입력은 NUL로 끝나는 레코드이며, 이름 변경·복사(`2`)는 원래 경로가
    // 다음 NUL 레코드로 따로 온다. `-z`가 아닌 출력은 해석하지 않는다.
    [[nodiscard]] git_status_result parse_git_status(std::string_view bytes);

    // 상태 결과로 저장소 요약의 브랜치·변경 수 필드를 채운다. 경로·표시 이름과
    // 진행 중 작업 상태는 건드리지 않는다.
    void apply_git_status(repository_summary& summary, const git_status_result& status);
} // namespace cyella
