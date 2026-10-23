#pragma once

#include "presentation/shell_layout.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/view_snapshot.h"

#include "luil/ui/virtual_list_element.h"

#include <memory>
#include <vector>

namespace cyella::ui {
    // 토스트와 알림 센터다.
    //  - 토스트는 주 tree에 둔다. popup 창은 불투명한 직사각형이라 둥근 모서리·부드러운 그림자·흐려지기를 그릴 수 없다.
    //    diff 웹뷰는 Skia 표면 아래의 composition visual이라 주 tree가 그 위에 그대로 그린다 (luil webview-composition-design.md).
    //  - 알림 센터는 popup이다. 바깥을 누르면 닫혀야 하는데, 주 tree에서 그 누름을 받으려면 창 전체를 덮어야 하고 그러면 웹뷰가 가려진다.

    // 쌓을 자리가 정해진 토스트 한 장이다. 놓는 차례대로 담는다 (덜 나온 것이 앞, 그래서 밑에 그려진다).
    struct placed_toast
    {
        std::unique_ptr<luil::ui_element> element {};
        shell_rect rect {};
        float presence { 1.0f };
    };

    // 오른쪽 아래에 쌓는 토스트다. 최신이 아래(상태 줄 가까이)이고 위로 쌓인다.
    // `bottom`은 토스트가 넘지 않을 아래 끝이다 (논리 픽셀, 작업 로그가 열려 있으면 그 위). 떠오르는 카드는 그 아래 끝 너머에서 올라온다.
    [[nodiscard]] std::vector<placed_toast> build_toasts(const view_snapshot& view, float bottom);

    // 상태 줄의 종이 연 알림 센터다. 오른쪽 끝, 상태 줄 바로 위에 붙는다. `list`는 배치 뒤 스크롤 상한을 읽을 목록이다.
    [[nodiscard]] built_popup build_notification_center_popup(const notification_center_view& center, const view_snapshot& view, float bottom, float scale, const luil::virtual_list_element** list);

    // 알림 한 장의 높이다 (논리 픽셀). 담는 쪽이 이 높이로 놓는다.
    [[nodiscard]] float notification_card_height(const notification_view& value, float width, bool center);
} // namespace cyella::ui
