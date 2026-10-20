#pragma once

#include "presentation/rebase/rebase_editor_state.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 편집기 상태를 바꾸는 순수 규칙이다. controller가 메시지마다 부르고, test는 상태를 직접 만들어 부른다.

    // 목록 항목의 키다 (`rebase-row:<sha>`). UI 자동화와 끌어 놓기가 이 키로 줄을 가리킨다.
    [[nodiscard]] std::u8string rebase_row_key(std::string_view sha);
    // 키에서 SHA를 꺼낸다. 이 편집기의 키가 아니면 값이 없다.
    [[nodiscard]] std::optional<std::string> rebase_row_sha(std::u8string_view key);

    // 범위를 받아 편집을 시작한다. 표지(`fixup!` …) 커밋이 있으면 `autosquash`대로 자동 정렬한 목록이다. 첫 줄을 고른다.
    void start_rebase_editing(rebase_editor_state& state, rebase_plan plan);
    // 자동 squash 정렬을 켜고 끈다. 목록을 그 순서로 다시 만들고, 고친 메시지 초안은 남긴다.
    void set_rebase_autosquash(rebase_editor_state& state, bool enabled);

    // 줄의 자리다. 없으면 값이 없다.
    [[nodiscard]] std::optional<std::size_t> rebase_row_index(const rebase_editor_state& state, std::string_view sha);
    // 고른 줄의 동작을 바꾼다. squash·fixup은 합칠 앞 커밋이 없으면(첫 줄) 바꾸지 않고 false다.
    bool set_rebase_action(rebase_editor_state& state, std::string_view sha, rebase_action action);
    // 줄을 delta만큼 옮긴다. 끝을 넘으면 옮기지 않고 false다.
    bool move_rebase_row(rebase_editor_state& state, std::string_view sha, int delta);
    // `moved`를 `target` 자리로 옮긴다. 아래로 옮기면 target 뒤, 위로 옮기면 target 앞에 선다.
    bool reorder_rebase_rows(rebase_editor_state& state, std::string_view moved, std::string_view target);

    // 초안의 열쇠다. reword 줄이면 `r:<sha>`, squash가 든 묶음에 속한 줄이면 묶음 첫 줄의 `g:<sha>`, 둘 다 아니면 값이 없다.
    // reword 줄이 squash 묶음의 첫 줄이면 reword 초안이 먼저다 (그 메시지는 묶음 메시지의 첫 덩어리가 된다).
    [[nodiscard]] std::optional<std::string> rebase_draft_key(const rebase_editor_state& state, std::string_view sha);
    // reword 줄마다 초안을 두고, squash 묶음마다 git과 같은 규칙의 합친 메시지 초안을 둔다. 고치지 않은 묶음 초안은 다시 만든다.
    void sync_rebase_drafts(rebase_editor_state& state);
    // 초안을 원래 글로 되돌린다.
    void reset_rebase_draft(rebase_editor_state& state, const std::string& key);

    // 초안의 글이다: 제목, 빈 줄, 본문.
    [[nodiscard]] std::string rebase_draft_message(const rebase_message_draft& draft);
    // 메시지를 제목 칸과 본문 칸으로 가른다 (첫 줄, 그 뒤 빈 줄들을 건넌 나머지).
    [[nodiscard]] rebase_message_draft make_rebase_draft(std::string_view message);

    // 실행할 목록이다. reword 줄과 squash 묶음의 마지막 줄에 초안 메시지를 채운다.
    [[nodiscard]] std::vector<rebase_todo_entry> rebase_final_todo(const rebase_editor_state& state);

    // 여러 줄 본문에서 caret을 윗줄·아랫줄의 같은 열(코드 포인트 수)로 옮긴다. 첫 줄의 ↑은 맨 앞, 끝 줄의 ↓은 맨 끝이다.
    void move_text_caret_line(luil::text::text_edit_state& state, bool down, bool extend);
    // 여러 줄 본문에서 Home·End를 그 줄의 처음·끝으로 옮긴다.
    void move_text_caret_line_edge(luil::text::text_edit_state& state, bool end, bool extend);
} // namespace cyella
