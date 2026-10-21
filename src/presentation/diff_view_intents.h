#pragma once

#include "application/diff_view_protocol.h"

namespace cyella {
    struct open_diff_file_app_intent
    {
        std::size_t index { 0 };
    };
    // diff 머리 막대가 내는 메시지다. 웹뷰의 보기 상태는 `diff_view_controller`가 정본이라 driver가 받아 컨트롤러에 옮기고,
    // 웹뷰 안의 같은 단추(`layout_changed`)로 바뀐 값은 driver가 다시 `ui_state::diff_surface`로 올려 보낸다.

    // inline ↔ 좌우 분할.
    struct set_diff_layout_intent
    {
        diff_layout layout { diff_layout::inline_view };
    };

    // 이미지 비교 방식(좌우·겹치기·밀기).
    struct set_diff_image_mode_intent
    {
        image_compare_mode mode { image_compare_mode::side_by_side };
    };
} // namespace cyella
