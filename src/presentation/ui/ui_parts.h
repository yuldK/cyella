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
        const luil::virtual_list_element* diff { nullptr };
        const luil::virtual_list_element* log { nullptr };
        const luil::scroll_area_element* start { nullptr };
        // 커밋 상세의 메시지·메타 칸이다. 넘칠 때만 흘리는 창이 선다.
        const luil::scroll_area_element* commit_meta { nullptr };
        const luil::scroll_area_element* settings { nullptr };
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
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_form_dialog(const form_view& form, float logical_width, float logical_height);
    // 우클릭 메뉴 popup이다. 좌표는 논리 픽셀이고 크기는 항목 글로 짐작한다.
    [[nodiscard]] built_popup build_menu_popup(const menu_view& menu, float scale);
    // 툴바 저장소·브랜치 선택 상자의 목록 popup이다. 거르는 칸이 위에 서고 항목은 메뉴 항목이다. 좌표는 논리 픽셀이다.
    [[nodiscard]] built_popup build_selector_popup(const selector_view& selector, float scale);
    // 끄는 동안 대상 위에 뜨는 문구다. 창 전체를 덮는 투명한 element이고 누름을 받지 않는다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_drag_hint_overlay(std::shared_ptr<const drop_hint_data> hints);
    // 그래프 자리를 덮는 충돌 해결 화면이다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_conflict_screen(const repository_view& repository, const conflict_view& conflict, float scroll, feedback_sources& sources);
} // namespace cyella::ui
