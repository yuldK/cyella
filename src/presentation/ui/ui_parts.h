#pragma once

#include "presentation/shell_layout.h"
#include "presentation/view_snapshot.h"

#include "luil/ui/scroll_area_element.h"
#include "luil/ui/ui_element.h"
#include "luil/ui/virtual_list_element.h"

#include <memory>

namespace cyella::ui {
    // 배치가 끝난 뒤 치수를 읽을 element들이다. tree가 소유하고 여기는 빌리기만 한다.
    struct feedback_sources
    {
        const luil::virtual_list_element* graph { nullptr };
        const luil::virtual_list_element* sidebar { nullptr };
        const luil::virtual_list_element* detail { nullptr };
        const luil::virtual_list_element* diff { nullptr };
        const luil::virtual_list_element* log { nullptr };
        const luil::scroll_area_element* start { nullptr };
    };

    // 셸의 판마다 짓는 함수다. 자리는 셸이 정하므로 각 판은 받은 자리 전체를 채운다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_start_page(const view_snapshot& view, const start_page_snapshot& start, feedback_sources& sources);
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_sidebar(const repository_view& repository, feedback_sources& sources);
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_rail(const repository_view& repository);
    // narrow의 아이콘 막대에서 연 구역의 떠 있는 판이다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_rail_popup(const repository_view& repository, sidebar_section section);
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_graph_area(const view_snapshot& view, const repository_view& repository, feedback_sources& sources);
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_detail(const view_snapshot& view, const repository_view& repository, bool drawer, feedback_sources& sources);
} // namespace cyella::ui
