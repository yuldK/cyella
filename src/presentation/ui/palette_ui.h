#pragma once

#include "presentation/palette/palette_view.h"

#include "luil/ui/ui_element.h"

#include <memory>

namespace cyella::ui {
    // 명령 팔레트 대화 상자다. 화면 전체를 덮고 위쪽 가운데에 거르는 칸과 목록을 놓는다 (`luil::modal_host_element`).
    // 거르는 칸이 뜨는 순간 초점을 받고, Esc나 바깥 누름은 `close_palette_intent`를 낸다.
    // 목록은 `virtual_list_element`다. 행을 누르거나(UIA Select) Enter를 치면 `palette_item_action`이 낸 액션이 실행된다.
    // 거르는 칸에서 Enter를 누르면 정책이 읽을 수 있게 커서 항목의 액션을 보이지 않는 표식(`palette_enter_element`)에 싣는다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_palette_dialog(const palette_view& view, float window_width, float window_height);
} // namespace cyella::ui
