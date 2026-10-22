#pragma once

#include "presentation/ui/build_ui_tree.h"
#include "presentation/view_snapshot.h"

#include "luil/ui/virtual_list_element.h"

#include <vector>

namespace cyella::ui {
    // 토스트와 알림 센터 popup이다. popup은 자기 창이라 diff 웹뷰(native 창) 위에도 보인다.

    // 오른쪽 아래에 쌓는 토스트다. 최신이 아래(상태 줄 가까이)이고 위로 쌓인다.
    // `bottom`은 토스트가 넘지 않을 아래 끝이다 (논리 픽셀, 작업 로그가 열려 있으면 그 위).
    [[nodiscard]] std::vector<built_popup> build_toast_popups(const view_snapshot& view, float bottom, float scale);

    // 상태 줄의 종이 연 알림 센터다. 오른쪽 끝, 상태 줄 바로 위에 붙는다. `list`는 배치 뒤 스크롤 상한을 읽을 목록이다.
    [[nodiscard]] built_popup build_notification_center_popup(const notification_center_view& center, const view_snapshot& view, float bottom, float scale, const luil::virtual_list_element** list);

    // 알림 한 장의 높이다 (논리 픽셀). 담는 쪽이 이 높이로 놓는다.
    [[nodiscard]] float notification_card_height(const notification_view& value, float width, bool center);
} // namespace cyella::ui
