#pragma once

#include "domain/interactive_rebase.h"
#include "presentation/rebase/rebase_editor_state.h"

#include <string>

namespace cyella {
    // 인터랙티브 rebase 편집기의 화면 메시지다. `rebase_editor_controller`가 처리한다.

    // 편집기를 연다. 메뉴(끌어 놓기·커밋·브랜치)가 낸다. branch가 비면 현재 HEAD다.
    struct open_rebase_editor_intent
    {
        std::string repository_id {};
        std::string onto {};
        std::string branch {};
        std::u8string onto_label {};
        std::u8string branch_label {};
        bool worktree_dirty { false };
    };

    struct close_rebase_editor_intent
    {};

    // 커밋 상세 판의 "여기서부터 인터랙티브 rebase…" 단추다 (커밋 우클릭 메뉴와 같은 일). 그 커밋 위의 커밋들을 편집한다.
    struct open_interactive_rebase_here_intent
    {
        std::string sha {};
    };

    // 줄을 골랐다. `row_key`는 목록 항목의 키(`rebase-row:<sha>`)다.
    struct rebase_select_intent
    {
        std::u8string row_key {};
    };

    // 줄의 동작을 바꾼다. `row_key`가 비면 고른 줄이다 (선택지·p r s f d 키).
    struct rebase_set_action_intent
    {
        std::u8string row_key {};
        rebase_action action { rebase_action::pick };
    };

    // 줄을 한 칸 옮긴다 (Alt+↑/↓). `row_key`가 비면 고른 줄이다.
    struct rebase_move_intent
    {
        std::u8string row_key {};
        int delta { 0 };
    };

    // 끌어 놓았다. `moved`를 `target` 자리로 옮긴다 (luil 목록의 reorder와 같은 뜻).
    struct rebase_reorder_intent
    {
        std::u8string moved {};
        std::u8string target {};
    };

    struct rebase_toggle_intent
    {
        rebase_option option { rebase_option::autosquash };
    };

    // 고른 줄의 메시지 초안을 원래 글(또는 git이 합칠 글)로 되돌린다.
    struct rebase_reset_message_intent
    {};

    // 본문 칸에서 ↑/↓를 눌렀다. 같은 열의 윗줄·아랫줄로 caret을 옮긴다 (`extend`면 선택을 늘린다).
    struct rebase_body_line_intent
    {
        bool down { false };
        bool extend { false };
    };

    // 목록과 오른쪽 판의 스크롤이다.
    struct rebase_scroll_intent
    {
        bool side { false };
        float value { 0.0f };
        bool absolute { false };
    };

    struct rebase_start_intent
    {};
} // namespace cyella
