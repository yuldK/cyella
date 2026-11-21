#pragma once

#include "presentation/frame_feedback.h"
#include "presentation/view_snapshot.h"

#include "luil/app/app_host.h"
#include "luil/theme/ui_style.h"
#include "luil/ui/ui_tree.h"

#include <functional>
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
        // 알림 센터다. 바깥 누름과 Esc에 닫힌다.
        bool notifications { false };
        // 참조 목록 카드다. 바깥 누름은 닫자고 청하고(포인터가 "+N" 칸 안이면 controller가 버린다), Esc·앵커가 사라짐에 닫히고,
        // 휠·창 이동·크기·활성 전환에는 남는다 (driver의 닫힘 계기 대응).
        bool ref_list { false };
        // diff 웹뷰 위에 띄우는 토스트다. 어느 닫힘 계기에도 남는다 (물러나는 것은 알림의 시한과 닫기 단추가 정한다).
        bool toasts { false };
        // tree의 픽셀 알파로 창 모양과 입력 자리를 정한다 (luil `ui_popup::transparent`). 꼬리와 그림자를 tree가 그린다.
        bool transparent { false };
        // popup 둘레의 경계다 (luil `ui_popup::border`). 둥근 카드가 제 테두리를 긋는 popup만 끈다.
        bool border { true };
        // 호스트가 작업 영역에 맞춰 세운 최종 자리를 받을 메시지다 (luil `ui_popup::placed`). 비면 받지 않는다.
        std::function<luil::app_message(const luil::rect_f&)> placed {};
        // 표면 모서리다 (luil `ui_popup::corner`). 비면 앱 스타일(OS 기본)이다.
        //  - 둥근 창은 OS가 잘라 내고 같은 모양의 테두리와 그림자를 얹는다. 둥글게 못 하는 환경에서는 직각 창과 1px 선이다.
        std::optional<luil::popup_corner> corner {};
        // 열 때 첫 초점을 받는 칸이다. 어느 요청(`popup_focus_target`)이 이 popup의 것인지와 그 칸의 id다.
        //  - popup tree 안의 가둠 진입(`focus_entry`)은 저절로 서지 않는다. 활성 표면의 가둠만 진입하는데 popup은 활성화를
        //    받지 않는 표면이라서다. 그래서 frame의 초점 요청(`built_tree::focus`)으로 옮긴다.
        std::optional<popup_focus_target> focus {};
        luil::ui_element_id focus_target {};
    };

    struct built_tree
    {
        std::shared_ptr<const luil::ui_tree> tree {};
        std::vector<built_popup> popups {};
        // 이 frame이 실을 초점 요청이다. snapshot의 요청이 가리키는 popup을 지었거나, 글꼴 목록을 닫은 뒤 돌아갈 글꼴 칸이
        // 주 tree에 섰을 때만 값이 있다.
        std::optional<luil::focus_request> focus {};
        // 배치한 뒤에 읽은 흘리는 창의 치수다. controller가 다음 스크롤을 다듬는 데 쓴다.
        frame_feedback feedback {};
    };

    // snapshot에서 배치가 끝난 불변 tree를 짓는다. 같은 snapshot이면 같은 tree다. tree는 한 번만 배치된다
    // (`luil::make_arranged_tree`) — `virtual_list_element`가 기대는 계약이다.
    [[nodiscard]] built_tree build_ui_tree(const view_snapshot& view);
} // namespace cyella::ui
