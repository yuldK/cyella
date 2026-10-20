// 커밋 그래프 목록의 열 폭 맞추기다. 목록이 좁으면 메시지 열이 하한을 지키고 그래프·브랜치/태그 열이 줄어든다.
#include "presentation/ui/graph_row_element.h"

#include <catch2/catch_test_macros.hpp>

using namespace cyella;

TEST_CASE("graph columns give way to the message column in a narrow list", "[presentation][graph]")
{
    // 폭을 모르면 기본 폭이다.
    const ui::graph_columns unknown { ui::make_graph_columns(20, false) };
    CHECK(unknown.refs == 160.0f);
    CHECK(unknown.graph == ui::graph_columns::maximum_graph);

    // 넓으면 그대로다.
    const ui::graph_columns wide { ui::make_graph_columns(3, false, 1400.0f) };
    CHECK(wide.refs == 160.0f);
    CHECK(wide.author == 110.0f);

    // 좁으면 그래프 → 브랜치/태그 순으로 줄어 메시지 열이 하한을 지킨다.
    const float width { 600.0f };
    const ui::graph_columns narrow { ui::make_graph_columns(12, false, width) };
    const float summary { width - narrow.refs - narrow.graph - narrow.author - narrow.date };
    CHECK(summary >= 220.0f);
    CHECK(narrow.graph < ui::graph_columns::maximum_graph);
    CHECK(narrow.refs >= 112.0f);
    CHECK(narrow.refs < 160.0f);

    // 브랜치/태그 열은 가장 넓은 라벨 줄만큼만 선다 (하한 112, 상한 기본 폭).
    CHECK(ui::make_graph_columns(3, false, 1400.0f, 0.0f).refs == 112.0f);
    CHECK(ui::make_graph_columns(3, false, 1400.0f, 120.0f).refs > 120.0f);
    CHECK(ui::make_graph_columns(3, false, 1400.0f, 400.0f).refs == 160.0f);

    // 아주 좁아도 하한 밑으로는 줄지 않는다.
    const ui::graph_columns tiny { ui::make_graph_columns(12, false, 200.0f) };
    CHECK(tiny.refs == 112.0f);
    CHECK(tiny.author == 76.0f);
    CHECK(tiny.date == 70.0f);
}
