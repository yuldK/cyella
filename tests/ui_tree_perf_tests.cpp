// snapshot에서 tree를 짓고 배치하는 데 드는 시간이다 (10만 행). 숨김 태그 `[perf]`라 ctest에는 없다:
// cyella_tests.exe "[perf]"
#include "presentation/ui/build_ui_tree.h"

#include "view_snapshot_support.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

using namespace cyella;
using namespace cyella::test::snapshot;

TEST_CASE("building and arranging the tree for 100k graph rows", "[.][perf]")
{
    snapshot_harness harness {};
    const repository_tab_id tab { harness.open(u8"C:\\work\\big", "repo-1") };
    load(*harness.sessions["repo-1"], linear_history(100000), 2);
    harness.ui.tabs[tab].graph_scroll = 50000.0f * 28.0f;
    const repository_session& session { *harness.sessions["repo-1"] };
    REQUIRE(session.graph_rows().size() == 100001);

    using clock = std::chrono::steady_clock;
    const auto milliseconds = [](const clock::duration duration) { return std::chrono::duration<double, std::milli>(duration).count(); };

    // cache 없이 모델을 frame마다 새로 짓는 경우다 (짧은 키라도 10만 번의 push_back).
    auto start { clock::now() };
    const std::vector<luil::virtual_list_item> fresh { build_graph_items(session.graph_commits()) };
    const double build_items { milliseconds(clock::now() - start) };

    // cache한 모델을 config로 복사하는 경우다 (`virtual_list_config::items`는 frame마다 값 하나).
    start = clock::now();
    const std::vector<luil::virtual_list_item> copied { fresh };
    const double copy_items { milliseconds(clock::now() - start) };

    auto cached { std::make_shared<const std::vector<luil::virtual_list_item>>(fresh) };
    double snapshot_total { 0.0 };
    double tree_total { 0.0 };
    constexpr int rounds { 10 };
    for (int round { 0 }; round < rounds; ++round)
    {
        start = clock::now();
        view_inputs frame {};
        frame.width = 1440.0f;
        frame.height = 900.0f;
        frame.tabs = &harness.tabs;
        frame.settings = &harness.settings;
        frame.ui = &harness.ui;
        frame.tab_repositories = &harness.tab_repositories;
        frame.find_session = [&harness](const std::string& id) -> const repository_session* { return harness.sessions[id].get(); };
        frame.graph_items = [&cached](const repository_session&) { return cached; };
        const view_snapshot view { build_view_snapshot(frame) };
        snapshot_total += milliseconds(clock::now() - start);

        start = clock::now();
        const cyella::ui::built_tree built { cyella::ui::build_ui_tree(view) };
        tree_total += milliseconds(clock::now() - start);
        REQUIRE(built.tree != nullptr);
        REQUIRE(built.feedback.graph_maximum.has_value());
    }
    std::printf(
        "[perf] 100k rows: build items %.2f ms, copy items %.2f ms, snapshot %.2f ms/frame, tree+arrange %.2f ms/frame\n", build_items, copy_items, snapshot_total / rounds, tree_total / rounds);
    CHECK(copied.size() == fresh.size());
}
