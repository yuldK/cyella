#pragma once

#include "presentation/palette/palette_model.h"

#include "luil/text/text_edit.h"

#include <string>

namespace cyella {
    // 명령 팔레트가 열려 있는 동안만 사는 화면 상태다 (`ui_state::palette`). 닫으면 입력도 모드도 버린다.
    struct palette_state
    {
        palette_mode mode { palette_mode::commands };
        luil::text::text_edit_state filter {};
        // 화살표가 가리키는 항목의 키다. 비었거나 목록에 없으면 첫 활성 항목이다.
        std::u8string cursor {};
        float scroll { 0.0f };
        bool reveal_cursor { true };
    };
} // namespace cyella
