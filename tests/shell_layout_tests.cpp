// 셸의 반응형 배치(폭 경계, 판 자리, 판 폭 다듬기)다. 순수 함수라 창 없이 값만 견준다.
#include "presentation/shell_layout.h"

#include <catch2/catch_test_macros.hpp>

using namespace cyella;

TEST_CASE("layout mode follows the window width breakpoints", "[presentation][layout]")
{
    CHECK(select_layout_mode(1440.0f) == layout_mode::wide);
    CHECK(select_layout_mode(1200.0f) == layout_mode::wide);
    CHECK(select_layout_mode(1199.0f) == layout_mode::medium);
    CHECK(select_layout_mode(760.0f) == layout_mode::medium);
    CHECK(select_layout_mode(759.0f) == layout_mode::narrow);
    CHECK(select_layout_mode(390.0f) == layout_mode::narrow);
}

TEST_CASE("toolbar drops labels below 960 and the repository selector below 480", "[presentation][layout]")
{
    CHECK(toolbar_shows_labels(960.0f));
    CHECK_FALSE(toolbar_shows_labels(959.0f));
    CHECK(toolbar_shows_repository_selector(480.0f));
    CHECK_FALSE(toolbar_shows_repository_selector(479.0f));
}

TEST_CASE("wide layout places sidebar, graph and detail side by side", "[presentation][layout]")
{
    const shell_rects rects { compute_shell_layout({ .width = 1440.0f, .height = 900.0f, .caption_height = 36.0f, .repository = true, .mode = layout_mode::wide }) };

    CHECK(rects.tab_row == shell_rect { 0.0f, 36.0f, 1440.0f, shell_tab_row_height });
    CHECK(rects.toolbar.y == 36.0f + shell_tab_row_height);
    CHECK(rects.banner.empty());
    CHECK(rects.status == shell_rect { 0.0f, 900.0f - shell_status_height, 1440.0f, shell_status_height });
    CHECK(rects.body.y == rects.toolbar.y + shell_toolbar_height);
    CHECK(rects.body.y + rects.body.height == rects.status.y);
    CHECK(rects.sidebar.width == default_sidebar_width);
    CHECK(rects.detail.width == default_detail_width);
    CHECK_FALSE(rects.detail_is_drawer);
    CHECK(rects.sidebar_splitter.x == rects.sidebar.x + rects.sidebar.width);
    CHECK(rects.graph.x == rects.sidebar_splitter.x + shell_splitter_width);
    CHECK(rects.detail_splitter.x == rects.graph.x + rects.graph.width);
    CHECK(rects.detail.x + rects.detail.width == 1440.0f);
}

TEST_CASE("medium layout collapses the detail panel into an overlaying drawer", "[presentation][layout]")
{
    shell_layout_input input { .width = 1000.0f, .height = 800.0f, .repository = true, .mode = layout_mode::medium };
    const shell_rects closed { compute_shell_layout(input) };
    CHECK(closed.detail.empty());
    CHECK(closed.detail_splitter.empty());
    CHECK(closed.graph.x + closed.graph.width == 1000.0f);

    input.drawer_open = true;
    const shell_rects open { compute_shell_layout(input) };
    CHECK(open.detail_is_drawer);
    CHECK(open.detail.width == default_detail_width);
    CHECK(open.detail.x + open.detail.width == 1000.0f);
    // 서랍은 그래프 위에 겹치므로 그래프 자리는 그대로다.
    CHECK(open.graph == closed.graph);
}

TEST_CASE("narrow layout turns the sidebar into an icon rail", "[presentation][layout]")
{
    const shell_rects rects { compute_shell_layout({ .width = 700.0f, .height = 600.0f, .repository = true, .mode = layout_mode::narrow, .drawer_open = true }) };
    CHECK(rects.sidebar.width == shell_rail_width);
    CHECK(rects.sidebar_splitter.empty());
    CHECK(rects.graph.x == shell_rail_width);
    CHECK(rects.graph.width == 700.0f - shell_rail_width);
    CHECK(rects.detail_is_drawer);
    // 서랍은 바깥을 누를 자리를 40px 남긴다.
    CHECK(rects.detail.width <= 700.0f - 40.0f);
}

TEST_CASE("graph keeps its minimum width by shrinking the side panes", "[presentation][layout]")
{
    const shell_rects rects
        = compute_shell_layout({ .width = 1200.0f, .height = 800.0f, .repository = true, .mode = layout_mode::wide, .sidebar_width = maximum_sidebar_width, .detail_width = maximum_detail_width });
    CHECK(rects.graph.width >= minimum_graph_width);
    CHECK(rects.detail.width >= minimum_detail_width);
    CHECK(rects.sidebar.width >= minimum_sidebar_width);
}

TEST_CASE("start tab uses the whole body and the log takes at most half of it", "[presentation][layout]")
{
    const shell_rects start { compute_shell_layout({ .width = 900.0f, .height = 700.0f, .caption_height = 36.0f, .banner_height = shell_banner_height, .log_open = true }) };
    CHECK(start.toolbar.empty());
    CHECK(start.banner.y == 36.0f + shell_tab_row_height);
    CHECK(start.sidebar.empty());
    CHECK(start.graph.empty());
    CHECK(start.log.height == shell_log_height);
    CHECK(start.log.y + start.log.height == start.status.y);
    CHECK(start.body.y + start.body.height == start.log.y);

    const shell_rects tiny { compute_shell_layout({ .width = 480.0f, .height = 300.0f, .caption_height = 36.0f, .log_open = true }) };
    CHECK(tiny.log.height <= (300.0f - shell_status_height - 36.0f - shell_tab_row_height) / 2.0f);
}

TEST_CASE("pane widths are clamped to their range", "[presentation][layout]")
{
    CHECK(clamp_sidebar_width(10.0f) == minimum_sidebar_width);
    CHECK(clamp_sidebar_width(10000.0f) == maximum_sidebar_width);
    CHECK(clamp_detail_width(300.0f) == 300.0f);
    CHECK(clamp_detail_width(0.0f) == minimum_detail_width);
}
