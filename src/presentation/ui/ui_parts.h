#pragma once

#include "presentation/shell_layout.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/view_snapshot.h"

#include "luil/ui/scroll_area_element.h"
#include "luil/ui/ui_element.h"
#include "luil/ui/virtual_list_element.h"

#include <memory>

namespace cyella::ui {
    class repository_tabs_element;
    // 배치가 끝난 뒤 치수를 읽을 element들이다. tree가 소유하고 여기는 빌리기만 한다.
    struct feedback_sources
    {
        const luil::virtual_list_element* graph { nullptr };
        const luil::virtual_list_element* sidebar { nullptr };
        const luil::virtual_list_element* detail { nullptr };
        const luil::scroll_area_element* detail_area { nullptr };
        const luil::virtual_list_element* diff { nullptr };
        const luil::virtual_list_element* file_history { nullptr };
        const luil::virtual_list_element* log { nullptr };
        const luil::scroll_area_element* start { nullptr };
        // 커밋 상세의 메시지·메타 칸이다. 넘칠 때만 흘리는 창이 선다.
        const luil::scroll_area_element* commit_meta { nullptr };
        const luil::scroll_area_element* working { nullptr };
        const luil::scroll_area_element* settings { nullptr };
        const luil::scroll_area_element* form { nullptr };
        const luil::virtual_list_element* palette { nullptr };
        const repository_tabs_element* tabs { nullptr };
    };

    // 셸의 판마다 짓는 함수다. 자리는 셸이 정하므로 각 판은 받은 자리 전체를 채운다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_start_page(const view_snapshot& view, const start_page_snapshot& start, feedback_sources& sources);
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_sidebar(const repository_view& repository, feedback_sources& sources);
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_rail(const repository_view& repository);
    // narrow의 아이콘 막대에서 연 구역의 떠 있는 판이다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_rail_popup(const repository_view& repository, sidebar_section section);
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_graph_area(const view_snapshot& view, const repository_view& repository, float width, feedback_sources& sources);
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_detail(const view_snapshot& view, const repository_view& repository, bool drawer, float width, float height, feedback_sources& sources);

    // 대화 상자·메뉴·끌어 놓기·충돌 해결이다.
    // 모달 입력 상자다. 화면 전체를 덮고 가운데에 대화 상자를 놓는다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_form_dialog(const form_view& form, float logical_width, float logical_height, feedback_sources& sources);
    // 우클릭 메뉴 popup이다. 좌표는 논리 픽셀이고 크기는 항목 글로 짐작한다.
    [[nodiscard]] built_popup build_menu_popup(const menu_view& menu, float scale);
    // 커밋 하나의 브랜치·태그를 종류별로 묶은 카드다. 투명 popup(`ui_popup::transparent`)에 서서 꼬리로 "+N" 칩을 가리킨다.
    //  - 창 모양이 tree의 알파라 꼬리와 그림자를 tree가 그린다. popup은 카드를 그림자가 닿는 만큼(옆 3, 위 2, 아래 4) 넓힌
    //    사각형이고, 칩 쪽은 꼬리 끝에서 끝난다 (그림자가 칩 줄을 덮어 칩 누름을 삼키지 않게). 그림자 띠는 입력을 받으므로 짧다.
    //  - `area`는 카드가 넘지 않을 자리다 (논리 픽셀). 칩 아래 자리가 모자라면 칩 위에 서고 꼬리가 아래를 가리킨다. 호스트가
    //    popup을 밀었으면(`ref_list_state::placed_shift`·`shadow_trim`·`flipped`) 꼬리를 거꾸로 옮기거나, 칩에서 먼 쪽 그림자
    //    여백을 깎거나, 칩 반대쪽으로 뒤집는다.
    //  - 그 커밋에 참조가 없으면 popup tree가 비어 있다.
    struct placed_ref_list
    {
        built_popup popup {};
        // 카드(몸통과 꼬리)의 자리다 (논리 픽셀, 주 창 client 기준).
        shell_rect rect {};
        // 꼬리 끝의 x와, 꼬리가 위를 가리키는가다 (논리 픽셀, 주 창 client 기준).
        float tail_x { 0.0f };
        bool tail_up { true };
    };
    [[nodiscard]] placed_ref_list build_ref_list_card(const ref_list_state& state, const std::vector<ref_label>& labels, float scale, const shell_rect& area);
    // 툴바 저장소·브랜치 선택 상자의 목록 popup이다. 거르는 칸이 위에 서고 항목은 메뉴 항목이다. 좌표는 논리 픽셀이다.
    [[nodiscard]] built_popup build_selector_popup(const selector_view& selector, float scale);
    // 끄는 동안 대상 위에 뜨는 문구다. 창 전체를 덮는 투명한 element이고 누름을 받지 않는다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_drag_hint_overlay(std::shared_ptr<const drop_hint_data> hints);
    // 그래프 자리를 덮는 충돌 해결 화면이다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_conflict_screen(const repository_view& repository, const conflict_view& conflict, float width, float scroll, feedback_sources& sources);
} // namespace cyella::ui
