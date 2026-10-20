#pragma once

#include "presentation/dialog_state.h"
#include "presentation/form_builders.h"
#include "presentation/form_view.h"

#include <string>

namespace cyella {
    // worktree 추가와 LFS 추적 패턴 대화 상자다. `form_builders`와 같은 규칙의 순수 함수들이다.

    // worktree 추가 대화 상자의 방식이다 (`dialog_state::choice`).
    enum class worktree_dialog_mode : std::size_t
    {
        // 시작 위치(`dialog.subject`, 비면 HEAD)에서 새 브랜치를 만들어 체크아웃한다.
        new_branch = 0,
        // 이미 있는 로컬 브랜치를 체크아웃한다.
        existing_branch = 1,
        // 시작 위치를 detached로 체크아웃한다.
        detached = 2,
    };

    // 칸: 0 = 새 worktree의 폴더, 1 = 브랜치 이름(방식에 따라 새 이름 또는 기존 이름, detached면 칸이 없다).
    [[nodiscard]] form_view build_add_worktree_form(const dialog_state& dialog, const form_build_context& context);
    // 칸: 0 = 추적 패턴.
    [[nodiscard]] form_view build_lfs_track_form(const dialog_state& dialog, const form_build_context& context);

    // 단추를 켤 수 있는가. 이유가 있으면 `reason`에 한글 한 줄로 채운다.
    [[nodiscard]] bool add_worktree_ready(const dialog_state& dialog, const form_inputs& inputs, const repository_snapshot& snapshot, std::u8string* reason = nullptr);
    [[nodiscard]] bool lfs_track_ready(const dialog_state& dialog, const form_inputs& inputs, std::u8string* reason = nullptr);

    // 대화 상자의 칸과 방식에서 낼 operation이다. ready가 아니면 쓰지 않는다.
    [[nodiscard]] worktree_add_request make_worktree_add_request(const dialog_state& dialog, const form_inputs& inputs);
    [[nodiscard]] lfs_track_request make_lfs_track_request(const form_inputs& inputs);

    // 저장소 폴더 옆에 둘 새 worktree의 기본 폴더다 (`E:/a/repo` + `feat/x` → `E:/a/repo-feat-x`). 구분자는 저장소 경로를 따른다.
    [[nodiscard]] std::u8string default_worktree_folder(std::u8string_view repository_path, std::string_view label);
} // namespace cyella
