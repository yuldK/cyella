#pragma once

#include "domain/interactive_rebase.h"
#include "presentation/rebase/rebase_editor_state.h"

#include "luil/ui/text_input_state.h"
#include "luil/ui/ui_interaction.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace cyella {
    // 인터랙티브 rebase 편집기의 한 frame 값이다. `build_rebase_editor_view`가 상태에서 짓는 순수 함수의 결과이고,
    // `ui::build_rebase_editor`가 이 값만 읽어 luil tree를 짓는다.

    struct rebase_row_view
    {
        // 목록 항목의 키다 (`rebase-row:<sha>`).
        std::u8string key {};
        std::string sha {};
        // 1부터 센 줄 번호다. 목록의 글자 탐색이 p·r·s·f·d를 줄 이름으로 찾지 않도록 이름이 숫자로 시작한다.
        std::size_t number { 0 };
        rebase_action action { rebase_action::pick };
        std::u8string short_sha {};
        std::u8string subject {};
        // 목록 한 줄의 글 전체다 (`1  squash  abc1234  제목 · 원격에 있음`). 화면 읽기도 이 글을 읽는다.
        std::u8string label {};
        bool pushed { false };
        autosquash_marker marker { autosquash_marker::none };
        // squash·fixup으로 앞 커밋에 합쳐지는 줄이다. 들여 그린다.
        bool melded { false };
    };

    struct rebase_preview_view
    {
        std::u8string key {};
        std::u8string subject {};
        // "커밋 2개 합침 · 메시지 바뀜" 같은 꼬리다.
        std::u8string detail {};
        bool changed { false };
    };

    struct rebase_editor_view
    {
        std::u8string title {};
        // "feature의 커밋 5개를 main(abc1234) 위로 다시 쌓습니다." 같은 한 줄이다.
        std::u8string subtitle {};
        rebase_editor_phase phase { rebase_editor_phase::loading };
        // 읽는 중·실행 중·읽기 실패의 글이다.
        std::u8string status_text {};
        std::vector<rebase_row_view> rows {};
        std::u8string selected_key {};
        float list_scroll { 0.0f };
        float side_scroll { 0.0f };

        // 고른 줄이다.
        bool has_selection { false };
        std::u8string selected_title {};
        rebase_action selected_action { rebase_action::pick };
        // 고른 줄을 squash·fixup할 수 있다 (앞에 남는 커밋이 있다).
        bool can_meld { false };
        // 고른 줄을 위·아래로 한 칸 옮길 수 있다 ("위로"·"아래로" 단추, Alt+↑↓).
        bool can_move_up { false };
        bool can_move_down { false };
        // 메시지 칸이 선다 (reword 줄, squash 묶음).
        bool message_editable { false };
        std::u8string message_caption {};
        // 메시지 칸이 없을 때 그 이유, 있을 때의 안내다.
        std::u8string message_note {};
        luil::text_input_view subject {};
        luil::text_input_view body {};
        bool message_edited { false };

        std::vector<rebase_preview_view> preview {};
        std::u8string preview_caption {};
        // 경고 줄이다 (원격에 있는 커밋, 병합 커밋).
        std::vector<std::u8string> warnings {};
        bool needs_flatten_confirm { false };
        bool flatten_confirmed { false };
        bool show_autosquash { false };
        bool autosquash { false };
        bool show_autostash { false };
        bool autostash { false };
        // "커밋 7개 → 4개"다.
        std::u8string summary {};
        // 시작할 수 없는 이유다 (검사 실패, 확인 필요, 실행 전 거절).
        std::u8string problem {};
        bool can_start { false };
    };

    // 편집기 값을 짓는다. `composition`은 IME 조합 중인 칸의 표시 글이다.
    [[nodiscard]] rebase_editor_view build_rebase_editor_view(const rebase_editor_state& state, const std::optional<luil::text_composition_event>& composition);
} // namespace cyella
