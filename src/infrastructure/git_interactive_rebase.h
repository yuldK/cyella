#pragma once

#include "infrastructure/git_operation_context.h"

#include <string>
#include <string_view>
#include <vector>

// 인터랙티브 rebase의 실행기 쪽이다. git_operation_runner의 구현 파일끼리만 쓰는 내부 헤더다.
// 결정은 `docs/decisions/ADR-005-interactive-rebase.md`에 있다.
namespace cyella {
    // `GIT_EDITOR`·`GIT_SEQUENCE_EDITOR`의 값이다. git은 이 값에 셸 특수 문자(`|&;<>()$`\"' \t\n*?[#~=%`와
    // 백슬래시)가 있으면 `sh -c`로, 없으면 바로 실행한다. 그래서 구분자를 `/`로 바꾸고, 그래도 특수 문자
    // (`Program Files`의 공백 등)가 남으면 작은따옴표로 감싼다 (안의 `'`는 `'\''`). 자료(준비 폴더)는 명령이 아니라
    // 환경 변수로 넘기므로 이 값에는 실행 파일 경로만 있다.
    [[nodiscard]] std::u8string make_editor_command(std::u8string_view helper_path);

    // `log --reverse --topo-order --max-parents=1 --right-only --cherry-pick -z --format=<본문 포함> <onto>...<branch>
    // --`. git이 `rebase -i`의 todo를 만들 때 고르는 커밋과 같다 (원격에 이미 같은 패치가 있는 커밋과 병합 커밋이
    // 빠진다). branch를 비우면 HEAD다.
    [[nodiscard]] git_arguments make_git_rebase_range_args(std::string_view onto, std::string_view branch);
    // `rev-list --count --merges <onto>..<branch> --`.
    [[nodiscard]] git_arguments make_git_rebase_merge_count_args(std::string_view onto, std::string_view branch);
    // `rev-list <onto>..<branch> --not --remotes --`: 어느 원격 브랜치에도 없는 커밋이다.
    [[nodiscard]] git_arguments make_git_rebase_unpushed_args(std::string_view onto, std::string_view branch);
    // `rev-parse --verify --quiet --end-of-options <revision>^{commit}`.
    [[nodiscard]] git_arguments make_git_resolve_commit_args(std::string_view revision);
    // `rebase -i --empty=drop --no-autosquash --no-update-refs [--autostash] -- <onto> [<branch>]`.
    //  - `--empty=drop`: `-i`의 기본은 비게 된 커밋에서 멈추는 것이다. 멈추면 충돌 파일 없이 진행 중 상태가 남는다.
    //  - `--no-autosquash`·`--no-update-refs`: 사용자 설정이 git이 만드는 목록을 바꾸지 않게 한다 (목록은 앱이 정한다).
    [[nodiscard]] git_arguments make_git_interactive_rebase_args(std::string_view onto, std::string_view branch, bool autostash);

    [[nodiscard]] operation_outcome run_load_rebase_plan(git_operation_context& context, const load_rebase_plan_request& request);
    [[nodiscard]] operation_outcome run_interactive_rebase(git_operation_context& context, const interactive_rebase_request& request);

    // 진행 중인 rebase가 cyella의 메시지를 들고 있으면(`rebase-merge/cyella/active`) continue·skip에 걸 환경
    // 변수다 (`GIT_EDITOR`=편집기 도우미). 아니면 비어 있다.
    [[nodiscard]] std::vector<process_environment_override> interactive_rebase_continue_environment(git_operation_context& context);
} // namespace cyella
