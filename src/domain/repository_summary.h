#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

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
