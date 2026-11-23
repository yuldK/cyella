#pragma once

#include "presentation/view_snapshot.h"

#include "luil/ui/ui_element.h"

#include <memory>

namespace cyella::ui {
    // 그래프 위에 서는 브랜치 필터 띠의 높이다 (논리 픽셀).
    inline constexpr float graph_filter_bar_height { 30.0f };

    // 필터가 걸려 있다는 한 줄과 [모두 보기] 단추다 (ADR-012). 검색 막대 아래, 열 머리 위에 선다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_graph_filter_bar(const graph_filter_view& filter);
} // namespace cyella::ui
