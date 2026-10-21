#pragma once

#include "presentation/palette/palette_model.h"

#include <string>

namespace cyella {
    // 명령 팔레트가 내는 화면 메시지다. controller가 처리한다.

    // 팔레트를 연다. 이미 열려 있으면 모드를 바꾸고 입력을 비운다.
    struct open_palette_intent
    {
        palette_mode mode { palette_mode::commands };
    };

    struct close_palette_intent
    {};

    // ↑(-1)·↓(+1)로 커서를 옮긴다. 비활성 항목은 건너뛰고 끝에서 멈춘다.
    struct palette_move_intent
    {
        int delta { 0 };
    };

    // 목록의 키보드 커서를 이 항목으로 옮긴다.
    struct palette_cursor_intent
    {
        std::u8string key {};
    };

    struct palette_scroll_intent
    {
        float value { 0.0f };
        bool absolute { false };
    };

    // 항목을 골랐다 (누름·UIA Select·Enter). 키는 `palette_model.h`의 접두를 따른다.
    struct palette_pick_intent
    {
        std::u8string key {};
    };
} // namespace cyella
