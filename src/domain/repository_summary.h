#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 저장소가 한 작업을 끝내지 못한 채 멈춰 있는 상태다. 충돌을 풀고 계속하거나
    // 중단해야 하는 화면의 근거다.
    enum class repository_operation_state
    {
        none,
        rebase,
        merge,
        cherry_pick,
        revert,
    };

    // 진행 중 작업의 표지가 되는 git 디렉터리 안의 이름이다. 존재 여부만 본다.
    // 파일 시스템 조회는 호출자가 하고, 판정은 순수 함수가 한다.
    inline constexpr std::array<std::string_view, 5> repository_operation_marker_names {
        std::string_view { "rebase-merge" },
        std::string_view { "rebase-apply" },
        std::string_view { "MERGE_HEAD" },
        std::string_view { "CHERRY_PICK_HEAD" },
        std::string_view { "REVERT_HEAD" },
    };

    // 멈춰 있는 작업의 진행 위치다. 충돌 해결 화면의 "커밋 2/3 적용 중"과 탭 표시가 읽는다.
    // rebase는 `<git_dir>/rebase-merge/{msgnum,end,onto,head-name,message}`(apply 방식은
    // `rebase-apply/{next,last,onto,head-name}`)에서, 그 밖은 `MERGE_MSG` 첫 줄에서 읽는다.
    struct in_progress_operation
    {
        repository_operation_state kind { repository_operation_state::none };
        // 1부터 센 현재 단계다. 단계가 없는 작업(merge)이나 읽지 못하면 0이다.
        std::uint32_t step { 0 };
        std::uint32_t total { 0 };
        // 지금 적용 중인 커밋의 제목 줄이다.
        std::string current_summary {};
        // rebase가 올라가는 대상 커밋 SHA다. rebase가 아니면 비어 있다.
        std::string onto {};
        // rebase 중인 브랜치의 전체 이름(`refs/heads/feature`)이다. detached rebase면 비어 있다.
        std::string head_name {};
        // 아래는 인터랙티브 rebase의 값이다. `rebase-merge/done`의 마지막 커밋 줄이 지금 처리 중인 할 일이다.
        // 명령어(`pick`, `reword`, `squash`, `fixup`, `drop`)와 SHA 전체다. 읽지 못하면 비어 있다.
        std::string current_action {};
        std::string current_sha {};
        // `rebase -i`로 시작했다 (`rebase-merge/interactive`).
        bool interactive { false };
        // cyella가 맡긴 reword·squash 메시지가 남아 있다 (`rebase-merge/cyella`). 계속하면 앱이 그 메시지를 쓴다.
        bool cyella_messages { false };
    };

    // `git remote -v`의 원격 하나다. 사이드바 REMOTE 묶음과 push 대화 상자의 원격 선택이 쓴다.
    struct remote_info
    {
        std::string name {};
        std::string fetch_url {};
        std::string push_url {};
    };

    // 탭 하나가 보여 줄 저장소 요약이다.
    struct repository_summary
    {
        std::string path {};
        std::string display_name {};
        // detached이거나 첫 커밋 전이면 비어 있다.
        std::string current_branch {};
        bool is_detached { false };
        // 첫 커밋 전이면 비어 있다.
        std::string head_sha {};
        std::string upstream {};
        bool has_upstream { false };
        std::uint32_t ahead { 0 };
        std::uint32_t behind { 0 };
        repository_operation_state operation_state { repository_operation_state::none };
        // staged, unstaged, untracked, 충돌 파일을 경로 기준으로 센 수다.
        std::uint32_t changed_file_count { 0 };
        std::uint32_t conflict_count { 0 };
        // `operation_state`와 같은 종류에 단계·대상을 더한 값이다. 둘은 항상 같은 kind를 가진다.
        in_progress_operation in_progress {};
        // `rev-parse --absolute-git-dir`의 값이다. 진행 중 표지와 index.lock의 위치가 여기서 정해진다.
        std::string git_dir {};
        std::vector<remote_info> remotes {};
        // HEAD가 upstream에서 닿는 커밋이다(`merge-base --is-ancestor HEAD <upstream>`). ahead
        // 수만으로는 amend할 커밋이 이미 올라갔는지 알 수 없어 따로 둔다. upstream이 없으면 false다.
        bool head_pushed { false };
    };

    // 존재하는 표지 경로 목록으로 진행 중인 작업을 판정한다. 경로는 git 디렉터리
    // 안의 이름(`rebase-merge`)이든 `.git/rebase-merge` 같은 전체 경로든, 구분자가
    // `/`이든 `\`이든 마지막 구성 요소로 비교한다.
    //
    // rebase는 내부에서 cherry-pick을 거치고 다른 표지를 남길 수 있어 가장 먼저 본다.
    // 같은 이유로 cherry-pick과 revert를 merge보다 앞에 둔다.
    [[nodiscard]] repository_operation_state detect_repository_operation_state(std::span<const std::string> existing_marker_paths) noexcept;

    [[nodiscard]] std::string_view repository_operation_state_name(repository_operation_state state) noexcept;
} // namespace cyella
