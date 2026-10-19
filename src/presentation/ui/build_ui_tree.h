#pragma once

#include "presentation/frame_feedback.h"
#include "presentation/view_snapshot.h"

#include "luil/ui/ui_tree.h"

#include <memory>

namespace cyella::ui {
    struct built_tree
    {
        std::shared_ptr<const luil::ui_tree> tree {};
        // 배치한 뒤에 읽은 흘리는 창의 치수다. controller가 다음 스크롤을 다듬는 데 쓴다.
        frame_feedback feedback {};
    };

    // snapshot에서 배치가 끝난 불변 tree를 짓는다. 같은 snapshot이면 같은 tree다. tree는 한 번만 배치된다
    // (`luil::make_arranged_tree`) — `virtual_list_element`가 기대는 계약이다.
    [[nodiscard]] built_tree build_ui_tree(const view_snapshot& view);
} // namespace cyella::ui
