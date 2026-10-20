#pragma once

#include "presentation/selector_model.h"

#include <string>

namespace cyella {
    // 툴바 저장소·브랜치 선택 상자의 목록이 내는 화면 메시지다. controller가 처리한다.

    // 선택 상자를 눌러 목록을 연다. 같은 상자의 목록이 이미 열려 있으면 닫는다. 좌표는 창 좌표의 물리 픽셀이고 (x, y)는 상자의
    // 왼쪽 아래다. 목록은 그 자리에 붙는다.
    struct open_selector_intent
    {
        selector_kind kind { selector_kind::repository };
        float x { 0.0f };
        float y { 0.0f };
        float width { 0.0f };
    };

    // 바깥을 누르거나 Esc를 눌러 목록을 닫는다.
    struct close_selector_intent
    {};

    // 목록 항목을 골랐다. 키는 `selector_model.h`의 접두(`tab:`, `recent:`, `branch:`)를 따른다.
    struct selector_select_intent
    {
        std::u8string key {};
    };
} // namespace cyella
