#pragma once

#include "presentation/view_snapshot.h"

#include "luil/ui/ui_element.h"

#include <memory>

namespace cyella::ui {
    // 그래프 위에 서는 커밋 검색 막대의 높이다 (논리 픽셀).
    inline constexpr float graph_search_bar_height { 36.0f };

    // 검색 칸, 결과 수, 앞·뒤·닫기 단추다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_graph_search_bar(const graph_search_view& search);
} // namespace cyella::ui
