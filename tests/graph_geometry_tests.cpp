// 그래프 칸의 기하(레인 x, 선분마다의 곡선 제어점, WIP 점선, 위쪽 짧은 선)와 라벨 짝짓기다.
#include "presentation/graph_geometry.h"
#include "presentation/graph_index.h"

#include <catch2/catch_test_macros.hpp>

using namespace cyella;

namespace {
    commit_info commit_of(std::string sha, std::vector<std::string> parents)
    {
        commit_info commit {};
        commit.sha = std::move(sha);
        commit.parents = std::move(parents);
        return commit;
    }
} // namespace

TEST_CASE("lane x is the lane center inside the padded graph column", "[presentation][graph]")
{
    CHECK(graph_lane_x(0) == graph_lane_padding + graph_lane_width / 2.0f);
    CHECK(graph_lane_x(1) - graph_lane_x(0) == graph_lane_width);
    CHECK(graph_lane_x(3) == 3.0f * 16.0f + 8.0f + 6.0f);
    CHECK(graph_column_width(0) == graph_column_width(1));
    CHECK(graph_column_width(4) == 4.0f * graph_lane_width + 2.0f * graph_lane_padding);
}

TEST_CASE("pass-through edges are vertical lines across the whole row", "[presentation][graph]")
{
    const graph_curve curve { graph_edge_curve({ .from_lane = 2, .to_lane = 2, .kind = graph_edge_kind::pass_through }) };
    CHECK(curve.straight);
    CHECK(curve.start == graph_point { graph_lane_x(2), 0.0f });
    CHECK(curve.end == graph_point { graph_lane_x(2), graph_row_height });
}

TEST_CASE("incoming edges run from the top of from_lane to the node", "[presentation][graph]")
{
    const graph_curve straight { graph_edge_curve({ .from_lane = 1, .to_lane = 1, .kind = graph_edge_kind::incoming }) };
    CHECK(straight.straight);
    CHECK(straight.start == graph_point { graph_lane_x(1), 0.0f });
    CHECK(straight.end == graph_point { graph_lane_x(1), graph_row_height / 2.0f });

    const graph_curve merge { graph_edge_curve({ .from_lane = 2, .to_lane = 0, .kind = graph_edge_kind::incoming }) };
    CHECK_FALSE(merge.straight);
    CHECK(merge.start == graph_point { graph_lane_x(2), 0.0f });
    CHECK(merge.end == graph_point { graph_lane_x(0), graph_row_height / 2.0f });
    // 제어점은 두 끝의 가운데 높이에서 각 끝의 x를 지킨다 — 행 경계의 접선이 세로다.
    CHECK(merge.control1 == graph_point { graph_lane_x(2), graph_row_height / 4.0f });
    CHECK(merge.control2 == graph_point { graph_lane_x(0), graph_row_height / 4.0f });
}

TEST_CASE("outgoing edges run from the node to the bottom of to_lane", "[presentation][graph]")
{
    const graph_curve branch { graph_edge_curve({ .from_lane = 0, .to_lane = 3, .kind = graph_edge_kind::outgoing }, 30.0f) };
    CHECK_FALSE(branch.straight);
    CHECK(branch.start == graph_point { graph_lane_x(0), 15.0f });
    CHECK(branch.end == graph_point { graph_lane_x(3), 30.0f });
    CHECK(branch.control1 == graph_point { graph_lane_x(0), 22.5f });
    CHECK(branch.control2 == graph_point { graph_lane_x(3), 22.5f });
}

TEST_CASE("the WIP chain is dashed from the WIP row down to the HEAD row", "[presentation][graph]")
{
    const graph_edge wip_out { .from_lane = 0, .to_lane = 0, .kind = graph_edge_kind::outgoing };
    const graph_edge pass { .from_lane = 0, .to_lane = 0, .kind = graph_edge_kind::pass_through };
    const graph_edge other_pass { .from_lane = 1, .to_lane = 1, .kind = graph_edge_kind::pass_through };
    const graph_edge into_head { .from_lane = 0, .to_lane = 0, .kind = graph_edge_kind::incoming };
    const graph_edge head_out { .from_lane = 0, .to_lane = 0, .kind = graph_edge_kind::outgoing };

    CHECK(graph_edge_is_wip_chain(wip_out, 0, 0, 3));
    CHECK(graph_edge_is_wip_chain(pass, 1, 0, 3));
    CHECK_FALSE(graph_edge_is_wip_chain(other_pass, 1, 0, 3));
    CHECK(graph_edge_is_wip_chain(into_head, 3, 0, 3));
    CHECK_FALSE(graph_edge_is_wip_chain(head_out, 3, 0, 3));
    CHECK_FALSE(graph_edge_is_wip_chain(pass, 4, 0, 3));
}

TEST_CASE("only unlabeled non-WIP rows with unloaded children get an upward stub", "[presentation][graph]")
{
    graph_row row {};
    row.has_unloaded_child = true;
    CHECK(graph_row_needs_upward_stub(row, false, false));
    // WIP 행은 위가 없다.
    CHECK_FALSE(graph_row_needs_upward_stub(row, true, false));
    // ref 끝은 자식이 없는 것이 정상이다.
    CHECK_FALSE(graph_row_needs_upward_stub(row, false, true));
    row.has_unloaded_child = false;
    CHECK_FALSE(graph_row_needs_upward_stub(row, false, false));
}

TEST_CASE("labels merge a local branch with its remote twin and mark local-only tags", "[presentation][graph]")
{
    repository_summary summary {};
    summary.remotes = { { .name = "origin" } };
    std::vector<ref> refs {
        { .kind = ref_kind::local, .name = "feature", .target_sha = "b" },
        { .kind = ref_kind::local, .name = "main", .target_sha = "a", .upstream = "origin/main", .is_current = true },
        { .kind = ref_kind::remote, .name = "origin/main", .target_sha = "a" },
        { .kind = ref_kind::remote, .name = "origin/feature", .target_sha = "b" },
        { .kind = ref_kind::remote, .name = "origin/HEAD", .target_sha = "a" },
        { .kind = ref_kind::remote, .name = "origin/old", .target_sha = "c" },
        { .kind = ref_kind::tag, .name = "v1", .target_sha = "a", .remotes = { "origin" } },
        { .kind = ref_kind::tag, .name = "v2-local", .target_sha = "a" },
    };
    const ref_label_map labels { build_ref_label_map(refs, summary) };

    const std::vector<ref_label>& at_a { labels.at("a") };
    REQUIRE(at_a.size() == 3);
    CHECK(at_a[0].kind == ref_label_kind::local);
    CHECK(at_a[0].current);
    CHECK(at_a[0].also_remote);
    CHECK(at_a[0].remote_name == u8"origin/main");
    CHECK(at_a[1].text == u8"v1");
    CHECK_FALSE(at_a[1].local_only);
    CHECK(at_a[2].text == u8"v2-local");
    CHECK(at_a[2].local_only);

    // upstream이 없어도 같은 이름·같은 커밋의 원격 브랜치와 합친다.
    const std::vector<ref_label>& at_b { labels.at("b") };
    REQUIRE(at_b.size() == 1);
    CHECK(at_b[0].also_remote);

    const std::vector<ref_label>& at_c { labels.at("c") };
    REQUIRE(at_c.size() == 1);
    CHECK(at_c[0].kind == ref_label_kind::remote);
    CHECK(at_c[0].ref_name == "origin/old");
}

TEST_CASE("a local branch without upstream pairs only with the remote branch named after a remote", "[presentation][graph]")
{
    repository_summary summary {};
    summary.remotes = { { .name = "origin" } };

    SECTION("a remote branch that only ends with the same name is not its twin")
    {
        const std::vector<ref> refs {
            { .kind = ref_kind::local, .name = "feature", .target_sha = "a" },
            { .kind = ref_kind::remote, .name = "origin/team/feature", .target_sha = "a" },
        };
        const ref_label_map labels { build_ref_label_map(refs, summary) };
        const std::vector<ref_label>& at_a { labels.at("a") };
        REQUIRE(at_a.size() == 2);
        CHECK_FALSE(at_a[0].also_remote);
        // 별개의 원격 브랜치 칩이 따로 선다.
        CHECK(at_a[1].kind == ref_label_kind::remote);
        CHECK(at_a[1].ref_name == "origin/team/feature");
    }

    SECTION("a local name with a slash still pairs with its own remote branch")
    {
        const std::vector<ref> refs {
            { .kind = ref_kind::local, .name = "team/feature", .target_sha = "a" },
            { .kind = ref_kind::remote, .name = "origin/team/feature", .target_sha = "a" },
        };
        const ref_label_map labels { build_ref_label_map(refs, summary) };
        const std::vector<ref_label>& at_a { labels.at("a") };
        REQUIRE(at_a.size() == 1);
        CHECK(at_a[0].also_remote);
        CHECK(at_a[0].remote_name == u8"origin/team/feature");
    }

    SECTION("with several remotes the first listed remote wins, not the first by name")
    {
        summary.remotes = { { .name = "origin" }, { .name = "backup" } };
        const std::vector<ref> refs {
            { .kind = ref_kind::local, .name = "x", .target_sha = "a" },
            { .kind = ref_kind::remote, .name = "backup/x", .target_sha = "a" },
            { .kind = ref_kind::remote, .name = "origin/x", .target_sha = "a" },
        };
        const ref_label_map labels { build_ref_label_map(refs, summary) };
        const std::vector<ref_label>& at_a { labels.at("a") };
        REQUIRE(at_a.size() == 2);
        CHECK(at_a[0].also_remote);
        CHECK(at_a[0].remote_name == u8"origin/x");
        CHECK(at_a[1].ref_name == "backup/x");
    }

    SECTION("without a remote list the part before the first slash is the remote name")
    {
        summary.remotes.clear();
        const std::vector<ref> refs {
            { .kind = ref_kind::local, .name = "feature", .target_sha = "a" },
            { .kind = ref_kind::remote, .name = "origin/team/feature", .target_sha = "a" },
            { .kind = ref_kind::remote, .name = "origin/feature", .target_sha = "a" },
        };
        const ref_label_map labels { build_ref_label_map(refs, summary) };
        const std::vector<ref_label>& at_a { labels.at("a") };
        REQUIRE(at_a.size() == 2);
        CHECK(at_a[0].also_remote);
        CHECK(at_a[0].remote_name == u8"origin/feature");
        CHECK(at_a[1].ref_name == "origin/team/feature");
    }
}

TEST_CASE("detached HEAD gets its own label and tags are not local-only without remotes", "[presentation][graph]")
{
    repository_summary summary {};
    summary.is_detached = true;
    summary.head_sha = "d";
    const ref_label_map labels { build_ref_label_map({ { .kind = ref_kind::tag, .name = "v1", .target_sha = "d" } }, summary) };
    const std::vector<ref_label>& at_d { labels.at("d") };
    REQUIRE(at_d.size() == 2);
    CHECK(at_d[0].kind == ref_label_kind::head);
    CHECK_FALSE(at_d[1].local_only);
}

TEST_CASE("graph item keys stay inside the short string buffer", "[presentation][graph]")
{
    const std::u8string key { graph_item_key("0123456789abcdef0123456789abcdef01234567") };
    CHECK(key == u8"0123456789abcde");
    // 짧은 문자열 버퍼 안이다 (MSVC는 15, libc++는 22 byte).
    CHECK(key.capacity() <= std::u8string {}.capacity());
    CHECK(graph_item_key("WIP") == u8"WIP");
}

TEST_CASE("graph index keeps only parents that come after their child", "[presentation][graph]")
{
    // 더 읽는 사이에 ref가 바뀌어 부모(p)가 자식(x·y)보다 앞에 온 목록이다. 부모 색인을 배열 첨자로 쓰는 질의가
    // 범위 밖을 건드리지 않고 그 연결을 없는 것으로 본다.
    const graph_index reversed { std::vector<commit_info> { commit_of("p", {}), commit_of("x", { "p" }), commit_of("y", { "p" }) } };
    CHECK(reversed.count("x", "y") == std::uint32_t { 1 });
    CHECK(reversed.count("y", "x") == std::uint32_t { 1 });
    CHECK(reversed.range("x", "y") == std::vector<std::size_t> { 2 });
    CHECK(reversed.range("", "y") == std::vector<std::size_t> { 2 });
    CHECK(reversed.is_ancestor("p", "x") == false);
    CHECK(reversed.is_ancestor("x", "p") == false);

    // 같은 SHA가 두 번 들어 있어도 뒤쪽 사본의 부모(앞쪽 색인)는 담지 않는다. 자기 자신을 부모로 가리키는 값도 같다.
    const graph_index duplicated { std::vector<commit_info> { commit_of("a", { "b" }), commit_of("b", {}), commit_of("a", { "b" }), commit_of("s", { "s" }) } };
    CHECK(duplicated.count("b", "a") == std::uint32_t { 1 });
    CHECK(duplicated.count("a", "s") == std::uint32_t { 1 });
    CHECK(duplicated.is_ancestor("b", "a") == true);

    // 정상 순서(자식이 부모보다 앞)는 그대로 센다.
    const graph_index linear { std::vector<commit_info> { commit_of("c", { "b" }), commit_of("b", { "a" }), commit_of("a", {}) } };
    CHECK(linear.count("a", "c") == std::uint32_t { 2 });
    CHECK(linear.range("a", "c") == std::vector<std::size_t> { 0, 1 });
    CHECK(linear.is_ancestor("a", "c") == true);
}
