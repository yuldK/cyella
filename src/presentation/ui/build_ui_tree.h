#pragma once

#include "presentation/frame_feedback.h"
#include "presentation/view_snapshot.h"

#include "luil/theme/ui_style.h"
#include "luil/ui/ui_tree.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cyella::ui {
    // 창 밖으로 나갈 수 있는 popup 하나다 (우클릭 메뉴). 자리는 주 창 client 기준 논리 픽셀이고, 닫기는 driver가 정한다.
    struct built_popup
    {
        std::u8string id {};
        float x { 0.0f };
        float y { 0.0f };
        float width { 0.0f };
        float height { 0.0f };
        std::shared_ptr<const luil::ui_tree> tree {};
        // 툴바 선택 상자의 목록이다. 검색 칸이 있어 창 활성이 바뀌어도 남고, 닫힘은 `close_selector_intent`다.
        bool selector { false };
        bool font { false };
        bool ref_list { false };
        // 알림 센터다. 바깥 누름과 Esc에 닫힌다.
        bool notifications { false };
        // 표면 모서리다 (luil `ui_popup::corner`). 비면 앱 스타일(OS 기본)이다.
        //  - 둥근 창은 OS가 잘라 내고 같은 모양의 테두리와 그림자를 얹는다. 둥글게 못 하는 환경에서는 직각 창과 1px 선이다.
        std::optional<luil::popup_corner> corner {};
    };

    struct built_tree
    {
        std::shared_ptr<const luil::ui_tree> tree {};
        std::vector<built_popup> popups {};
        // 배치한 뒤에 읽은 흘리는 창의 치수다. controller가 다음 스크롤을 다듬는 데 쓴다.
        frame_feedback feedback {};
    };

    // snapshot에서 배치가 끝난 불변 tree를 짓는다. 같은 snapshot이면 같은 tree다. tree는 한 번만 배치된다
    // (`luil::make_arranged_tree`) — `virtual_list_element`가 기대는 계약이다.
    [[nodiscard]] built_tree build_ui_tree(const view_snapshot& view);
} // namespace cyella::ui
