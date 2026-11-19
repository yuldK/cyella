#pragma once

#include "application/git_operation.h"
#include "domain/operation_status.h"
#include "presentation/menu_model.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cyella {
    // 대화 상자·메뉴·끌어 놓기·충돌 해결이 내는 화면 메시지다. `ui_intents.h`의 짝이고, controller가 처리한다.

    // ------------------------------------------------------------------ 메뉴

    // 우클릭이다. 좌표는 창 좌표의 물리 픽셀이다 (`ui_action_context`가 준 값).
    struct open_context_menu_intent
    {
        menu_target target {};
        float x { 0.0f };
        float y { 0.0f };
    };

    struct close_menu_intent
    {};

    // 커밋 하나의 참조 목록 카드를 누름·키·보조 기술로 연다. 사각형은 카드가 가리킬 "+N" 칩의 자리다 (주 창 표면 좌표, 물리 픽셀).
    //  - 같은 카드가 호버로 떠 있으면 누름으로 고정한다 (떠나도 남는다). 닫는 것은 "+N"이 상태를 보고 `close_ref_list_intent`로 낸다.
    struct open_ref_list_intent
    {
        std::string sha {};
        float x { 0.0f };
        float y { 0.0f };
        float width { 0.0f };
        float height { 0.0f };
        // 그 행의 항목 키다 (`graph_item_key`). 호버 판정이 "카드가 이 행의 것인가"를 이것으로 가린다.
        std::u8string row_key {};
    };

    struct close_ref_list_intent
    {
        // 카드 popup 밖을 눌러 닫자는 것이다 (`popup_dismiss_reason::pointer_press_outside`). 포인터가 "+N" 칸 안(머무름이 선 뒤
        // 떠나지 않은 칸)이면 그 누름이 스스로 처리하므로(칩·빈 칸은 고르기·메뉴, "+N"은 열기·고정·닫기) controller가 이것을 버린다.
        bool pressed_outside { false };
    };

    // 포인터가 "+N"이 선 브랜치/태그 칸의 자리에 들어오거나 나갔다 (칸의 hover 영역). 칸을 다른 것이 덮어도 자리 안이면 안이다.
    //  - 사각형은 그 칸의 "+N" 칩 자리다 (주 창 표면 좌표, 물리 픽셀). 호버로 연 카드도 누름으로 연 카드처럼 이 칩을 가리킨다.
    struct ref_cell_hover_intent
    {
        std::u8string row_key {};
        std::string sha {};
        float x { 0.0f };
        float y { 0.0f };
        float width { 0.0f };
        float height { 0.0f };
        bool inside { false };
    };

    // 포인터가 그 칸 위에 정말 머문다 (칸의 머무름, 지연 0). 덮개가 hover를 가져갔거나 누름·끌기 중이면 서지 않거나 끝난다.
    struct ref_cell_dwell_intent
    {
        std::u8string row_key {};
        bool live { false };
    };

    // 포인터가 참조 목록 카드 위에 들어오거나 나갔다 (카드 popup의 hover 영역).
    struct ref_card_hover_intent
    {
        bool inside { false };
    };

    // 호스트가 작업 영역에 맞춰 카드 popup을 실제로 세운 자리다 (논리 픽셀, 주 창 client 기준). `requested_*`는 그때 청한 자리다.
    //  - `serial`은 그 카드를 연 번호다 (`ref_list_state::serial`). 앞서 닫은 카드의 늦은 보고를 버린다.
    //  - `margin_*`는 그때 popup이 카드 몸통 위·아래에 둔 그림자 여백이다. 그 안의 밀림은 그림자만 넘친 것이라 뒤집지 않는다.
    struct ref_list_placed_intent
    {
        std::uint64_t serial { 0 };
        float requested_x { 0.0f };
        float requested_y { 0.0f };
        float x { 0.0f };
        float y { 0.0f };
        float margin_above { 0.0f };
        float margin_below { 0.0f };
    };

    struct scroll_ref_list_intent
    {
        float value { 0.0f };
        bool absolute { false };
    };

    struct menu_select_intent
    {
        std::u8string key {};
    };

    // ------------------------------------------------------------------ 끌어 놓기

    // 브랜치를 다른 브랜치나 커밋에 놓았다. 좌표는 메뉴를 놓을 자리(물리 픽셀)다.
    struct drop_intent
    {
        drop_party source {};
        drop_party destination {};
        float x { 0.0f };
        float y { 0.0f };
    };

    // 커밋 행을 두 번 눌렀다. 그 커밋의 브랜치로 체크아웃하고, 없으면 detached 체크아웃을 묻는다.
    struct activate_commit_intent
    {
        std::string sha {};
    };

    // ------------------------------------------------------------------ 모달 입력 상자

    // 선택지(라디오)를 골랐다. `index`는 `form_view::choices`의 자리다.
    struct form_choose_intent
    {
        std::size_t index { 0 };
    };

    // 체크 칸을 뒤집었다. 인증 상자의 0번은 "입력 보이기"다.
    struct form_check_intent
    {
        std::size_t index { 0 };
    };

    // 단추를 눌렀다. `key`는 `form_button::key`다. Esc와 닫기 단추는 `form_view::dismiss_key`를 보낸다.
    struct form_button_intent
    {
        std::u8string key {};
    };

    // 칸에서 Enter를 눌렀다. 기본 단추가 켜져 있으면 그것을 누른 것과 같다.
    struct form_enter_intent
    {};

    struct form_scroll_intent
    {
        float value { 0.0f };
        bool absolute { false };
    };

    // ------------------------------------------------------------------ 열기

    // 작업 트리의 변경을 버린다. 확인 상자를 먼저 연다.
    struct discard_files_intent
    {
        // 추적 중인 파일이다. 작업 트리 변경을 index로 되돌린다 (`discard_files_request`).
        std::vector<std::string> paths {};
        // 추적하지 않는 파일이다. git으로는 되돌릴 수 없어 확인 상자가 "휴지통으로 보냅니다"를 말하고 `trash_files_request`를 낸다.
        std::vector<std::string> untracked {};
    };

    // ------------------------------------------------------------------ 알림·충돌

    // 알림의 조치 단추를 눌렀다. `id`는 실패 알림의 operation이고(안내는 0), `notification`은 누른 알림이다.
    // `notification`이 0이면 키(와 operation)가 맞는 가장 최근 알림이다.
    struct notification_action_intent
    {
        std::u8string key {};
        operation_id id { 0 };
        std::uint64_t notification { 0 };
    };

    enum class conflict_action
    {
        continue_sequence,
        skip_sequence,
        abort_sequence,
    };

    struct conflict_action_intent
    {
        conflict_action action { conflict_action::continue_sequence };
    };
} // namespace cyella
