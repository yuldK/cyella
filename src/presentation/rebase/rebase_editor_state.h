#pragma once

#include "domain/interactive_rebase.h"
#include "domain/operation_status.h"

#include "luil/text/text_edit.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cyella {
    // 인터랙티브 rebase 편집기의 화면 상태다. 열려 있는 동안만 있고 닫으면 버린다. git 상태의 정본은 workspace다.

    // 메시지 초안 하나다. 열쇠는 reword 줄이면 `r:<sha>`, squash 묶음이면 묶음 첫 줄의 `g:<sha>`다.
    struct rebase_message_draft
    {
        luil::text::text_edit_state subject {};
        luil::text::text_edit_state body {};
        // 사용자가 고쳤다. 고치지 않은 squash 초안은 묶음이 바뀔 때마다 git과 같은 규칙으로 다시 만든다.
        bool edited { false };
    };

    enum class rebase_editor_phase
    {
        // 범위(`load_rebase_plan`)를 읽는 중이다.
        loading,
        editing,
        // `interactive_rebase`를 냈고 끝나기를 기다린다.
        running,
        // 범위를 읽지 못했다. `error`가 이유다.
        load_failed,
    };

    // 편집기의 켬·끔 칸이다.
    enum class rebase_option
    {
        // `fixup!`·`squash!`·`amend!` 커밋을 대상 뒤로 옮겨 둔 목록으로 시작한다. 끄면 원래 순서의 pick이다.
        autosquash,
        // 작업 트리 변경을 보관했다가 끝나면 다시 꺼낸다 (`--autostash`).
        autostash,
        // 병합 커밋을 빼고 이력을 한 줄로 펴도 된다는 확인이다.
        flatten_merges,
    };

    struct rebase_editor_state
    {
        std::string repository_id {};
        // 요청 값이다 (브랜치 이름이나 SHA). branch가 비면 HEAD다.
        std::string onto {};
        std::string branch {};
        std::u8string onto_label {};
        std::u8string branch_label {};
        rebase_editor_phase phase { rebase_editor_phase::loading };
        operation_id plan_operation { 0 };
        operation_id run_operation { 0 };
        // 범위를 읽지 못한 이유나, 실행 전에 거절된 이유다.
        std::u8string error {};
        rebase_plan plan {};
        // 지금 편집 중인 목록이다. 메시지는 비워 두고 `drafts`가 든다 (시작할 때 채운다).
        std::vector<rebase_todo_entry> todo {};
        std::map<std::string, rebase_message_draft> drafts {};
        // 고른 줄의 SHA다.
        std::string selected {};
        bool autosquash { true };
        bool autostash { false };
        bool flatten_confirmed { false };
        // 열 때 작업 트리나 index에 변경이 있었다. autostash 칸이 선다.
        bool worktree_dirty { false };
        float list_scroll { 0.0f };
        float side_scroll { 0.0f };
    };
} // namespace cyella
