#pragma once

#include "domain/repository_summary.h"
#include "presentation/menu_model.h"
#include "presentation/selector_model.h"

#include "luil/ui/text_input_state.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cyella {
    // 모달 입력 상자(대화 상자) 하나를 그리는 데 필요한 값이다. 브랜치·태그·push·확인·체크아웃 정책·인증 질문이 모두
    // 이 모양 하나로 그려진다 — 종류마다 갈리는 것은 어떤 칸과 단추가 서는가뿐이다. snapshot builder가 상태에서
    // 이 값을 짓는 순수 함수이고, `ui::build_ui_tree`는 이 값만 읽어 그린다.

    enum class form_tone
    {
        normal,
        accent,
        danger,
    };

    struct form_note
    {
        std::u8string text {};
        bool warning { false };
        bool code { false };
    };

    struct form_field
    {
        std::u8string label {};
        luil::text_input_view view {};
        std::u8string placeholder {};
        // 입력을 점으로 가린다 (passphrase, 비밀번호).
        bool secret { false };
        bool reveal { false };
        // 이 칸에 처음 초점이 선다.
        bool focus { false };
    };

    struct form_choice
    {
        std::u8string label {};
    };

    struct form_check
    {
        std::u8string label {};
        bool checked { false };
        bool enabled { true };
    };

    struct form_button
    {
        std::u8string key {};
        std::u8string label {};
        form_tone tone { form_tone::normal };
        bool enabled { true };
        // Enter가 가는 단추다.
        bool default_button { false };
    };

    struct form_view
    {
        // 대화 상자를 가르는 owner다. UIA가 이 이름으로 찾는다.
        std::u8string id {};
        std::u8string title {};
        std::vector<form_note> notes {};
        std::vector<form_field> fields {};
        std::u8string choices_label {};
        std::vector<form_choice> choices {};
        std::size_t chosen { 0 };
        // 선택지가 칸의 뜻을 정하면(원격 추가의 "게시할 곳") 칸보다 앞에 둔다. 아니면 칸 뒤다.
        bool choices_first { false };
        std::vector<form_check> checks {};
        // 선택지·체크 아래의 경고나 안내다.
        std::vector<form_note> warnings {};
        // 실행할 명령줄 미리보기다.
        std::u8string preview {};
        std::u8string error {};
        std::vector<form_button> buttons {};
        // Esc와 닫기 단추가 보내는 단추 키다. 비어 있으면 닫을 수 없다.
        std::u8string dismiss_key {};
        // 비밀 칸에 "보이기" 단추를 단다.
        bool can_reveal { false };
        float scroll { 0.0f };
    };

    // ------------------------------------------------------------------ 메뉴

    struct menu_item_view
    {
        menu_entry entry {};
    };

    // 열린 우클릭 메뉴다. 좌표는 논리 픽셀이다 (창 좌표 ÷ 배율).
    struct menu_view
    {
        std::u8string title {};
        std::vector<menu_entry> entries {};
        float x { 0.0f };
        float y { 0.0f };
    };

    // 툴바 선택 상자의 목록이다. 거르는 칸이 위에 서고 항목은 우클릭 메뉴와 같은 `menu_entry`다. 좌표는 논리 픽셀이다.
    struct selector_view
    {
        selector_kind kind { selector_kind::repository };
        float x { 0.0f };
        float y { 0.0f };
        float width { 0.0f };
        luil::text_input_view filter {};
        std::u8string placeholder {};
        std::vector<menu_entry> entries {};
    };

    // ------------------------------------------------------------------ 충돌 해결

    struct conflict_file_view
    {
        std::string path {};
        std::u8string name {};
        std::u8string folder {};
        // git의 두 글자 코드(UU, AA, DU …)다.
        std::u8string code {};
        std::u8string meaning {};
    };

    // 멈춘 rebase·merge·cherry-pick·revert를 풀어 가는 화면이다. 3-way 편집기는 없고 파일 목록과 동작이 전부다.
    struct conflict_view
    {
        repository_operation_state kind { repository_operation_state::none };
        std::u8string title {};
        // "커밋 2/3 적용 중: 제목" 같은 진행 줄이다.
        std::u8string progress {};
        std::u8string detail {};
        std::vector<conflict_file_view> files {};
        // 충돌이 모두 풀려 계속할 수 있다.
        bool can_continue { false };
        bool can_skip { false };
        // 직전 동작이 아직 도는 중이다. 단추를 누를 수 없다.
        bool busy { false };
        // 직전 계속 시도가 다시 충돌이나 오류로 끝났다.
        std::u8string message {};
        std::optional<std::u8string> merge_tool {};
        // 인터랙티브 rebase면 지금 멈춘 할 일과 남은 메시지 안내다 ("할 일 2/3 · pick abc1234 …"). 아니면 비어 있다.
        std::u8string todo_note {};
    };
} // namespace cyella
