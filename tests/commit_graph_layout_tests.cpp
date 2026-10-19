#include "domain/commit_graph_layout.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cyella {
    namespace {
        // "sha parent parent..." 형식의 줄을 graph_commit으로 바꾼다. 테스트에서 토폴로지가
        // 한눈에 읽히게 하려는 것이고, `git log --format="%H %P"` 출력과 같은 모양이다.
        graph_commit parse_commit(std::string_view line)
        {
            std::istringstream stream { std::string { line } };
            graph_commit commit {};
            stream >> commit.sha;

            std::string parent;
            while (stream >> parent)
                commit.parent_shas.push_back(parent);

            return commit;
        }

        std::vector<graph_commit> make_commits(std::initializer_list<std::string_view> lines)
        {
            std::vector<graph_commit> commits;
            for (const std::string_view line : lines)
                commits.push_back(parse_commit(line));

            return commits;
        }

        std::vector<graph_row> layout_in_pages(const std::vector<graph_commit>& commits, std::size_t page_size)
        {
            commit_graph_layout layout {};
            std::vector<graph_row> rows;
            for (std::size_t begin = 0; begin < commits.size(); begin += page_size)
            {
                const std::size_t count = std::min(page_size, commits.size() - begin);
                const std::vector<graph_row> page = layout.append(std::span<const graph_commit> { commits.data() + begin, count });
                rows.insert(rows.end(), page.begin(), page.end());
            }

            return rows;
        }

        std::vector<graph_row> layout_all(const std::vector<graph_commit>& commits)
        {
            commit_graph_layout layout {};
            return layout.append(commits);
        }

        std::size_t count_kind(const graph_row& row, graph_edge_kind kind)
        {
            return static_cast<std::size_t>(std::count_if(row.edges.begin(), row.edges.end(), [kind](const graph_edge& edge) { return edge.kind == kind; }));
        }

        bool has_edge(const graph_row& row, const graph_edge& wanted)
        {
            return std::find(row.edges.begin(), row.edges.end(), wanted) != row.edges.end();
        }

        // 디버그용 ASCII 렌더러다. 레인 i는 열 2*i에 놓이고 노드는 `*`, 지나가는 선은 `|`다.
        // 레인을 옮기는 선(합류·병합 부모)은 도착 쪽 열에 선의 모양대로 `/`, `\`를 그린다.
        // 실패했을 때 그래프가 눈으로 읽히게 하려는 도구라서 테스트에만 둔다.
        std::string render_row(const graph_row& row, const std::string& sha)
        {
            std::size_t width = static_cast<std::size_t>(row.lane) * 2 + 1;
            for (const graph_edge& edge : row.edges)
                width = std::max<std::size_t>({ width, static_cast<std::size_t>(edge.from_lane) * 2 + 1, static_cast<std::size_t>(edge.to_lane) * 2 + 1 });

            std::string text(width, ' ');
            for (const graph_edge& edge : row.edges)
                if (edge.kind == graph_edge_kind::pass_through)
                    text[static_cast<std::size_t>(edge.from_lane) * 2] = '|';

            for (const graph_edge& edge : row.edges)
            {
                if (edge.from_lane == edge.to_lane)
                    continue;

                if (edge.kind == graph_edge_kind::incoming)
                    text[static_cast<std::size_t>(edge.from_lane) * 2] = edge.from_lane > edge.to_lane ? '/' : '\\';
                else if (edge.kind == graph_edge_kind::outgoing)
                    text[static_cast<std::size_t>(edge.to_lane) * 2] = edge.to_lane > edge.from_lane ? '\\' : '/';
            }

            text[static_cast<std::size_t>(row.lane) * 2] = '*';
            return text + "  " + sha;
        }

        std::vector<std::string> render(const std::vector<graph_commit>& commits, const std::vector<graph_row>& rows)
        {
            REQUIRE(commits.size() == rows.size());

            std::vector<std::string> lines;
            for (std::size_t index = 0; index < rows.size(); ++index)
                lines.push_back(render_row(rows[index], commits[index].sha));

            return lines;
        }

        using lane_set = std::set<std::pair<std::uint16_t, std::uint8_t>>;

        lane_set top_lanes(const graph_row& row)
        {
            lane_set lanes;
            for (const graph_edge& edge : row.edges)
                if (edge.kind != graph_edge_kind::outgoing)
                    lanes.insert({ edge.from_lane, edge.color });

            return lanes;
        }

        lane_set bottom_lanes(const graph_row& row)
        {
            lane_set lanes;
            for (const graph_edge& edge : row.edges)
                if (edge.kind != graph_edge_kind::incoming)
                    lanes.insert({ edge.to_lane, edge.color });

            return lanes;
        }

        // 어떤 입력에서도 성립해야 하는 구조 불변식이다. 한 행의 아래 경계 선은 다음 행의 위
        // 경계 선과 레인·색이 같아야 선이 끊기지 않는다.
        void check_invariants(const std::vector<graph_row>& rows, const commit_graph_layout& layout)
        {
            std::uint16_t widest = 0;
            for (std::size_t index = 0; index < rows.size(); ++index)
            {
                const graph_row& row = rows[index];
                CHECK(row.color < graph_palette_size);
                CHECK(row.is_root == (count_kind(row, graph_edge_kind::outgoing) == 0));
                CHECK(row.has_unloaded_child == (count_kind(row, graph_edge_kind::incoming) == 0));
                widest = std::max<std::uint16_t>(widest, static_cast<std::uint16_t>(row.lane + 1));

                for (const graph_edge& edge : row.edges)
                {
                    CHECK(edge.color < graph_palette_size);
                    widest = std::max<std::uint16_t>({ widest, static_cast<std::uint16_t>(edge.from_lane + 1), static_cast<std::uint16_t>(edge.to_lane + 1) });
                    if (edge.kind == graph_edge_kind::pass_through)
                        CHECK(edge.from_lane == edge.to_lane);
                }

                if (index == 0)
                    CHECK(top_lanes(row).empty());
                else
                    CHECK(top_lanes(row) == bottom_lanes(rows[index - 1]));
            }

            CHECK(widest == layout.max_lane_count());
            if (!rows.empty())
                CHECK(bottom_lanes(rows.back()).size() == layout.open_lane_count());
        }

        // 의사 난수로 topo 순서 DAG를 만든다. 부모는 늘 더 뒤(큰 인덱스)에 있으므로 자식이
        // 먼저 온다. 가끔 부모를 건너뛰어 가지(자식 없는 끝)를 만들고 자주 병합한다.
        std::vector<graph_commit> make_synthetic_history(std::size_t count, std::uint32_t seed)
        {
            std::vector<std::string> shas;
            shas.reserve(count);
            for (std::size_t index = 0; index < count; ++index)
            {
                char text[48];
                const auto value = static_cast<unsigned>(index);
                std::snprintf(text, sizeof text, "%08x%08x%08x%08x%08x", value, value * 2654435761u, value * 40503u + 7u, ~value, value ^ 0x9e3779b9u);
                shas.emplace_back(text);
            }

            std::uint32_t state = seed;
            const auto next = [&state]() {
                state = state * 1664525u + 1013904223u;
                return static_cast<std::size_t>(state >> 8);
            };

            std::vector<graph_commit> commits;
            commits.reserve(count);
            for (std::size_t index = 0; index < count; ++index)
            {
                graph_commit commit { shas[index], {} };
                const std::size_t first = next() % 100 < 85 ? index + 1 : index + 1 + next() % 8;
                if (first < count)
                    commit.parent_shas.push_back(shas[first]);

                if (next() % 100 < 20)
                {
                    const std::size_t second = index + 2 + next() % 30;
                    if (second < count && second != first)
                        commit.parent_shas.push_back(shas[second]);
                }

                if (next() % 100 < 1)
                {
                    const std::size_t third = index + 2 + next() % 60;
                    if (third < count && std::find(commit.parent_shas.begin(), commit.parent_shas.end(), shas[third]) == commit.parent_shas.end())
                        commit.parent_shas.push_back(shas[third]);
                }

                commits.push_back(std::move(commit));
            }

            return commits;
        }

        // 아래 네 토폴로지는 실제 git 저장소를 만들어 `git log --topo-order --format="%h %p" --all`로
        // 얻은 것을 짧은 가짜 SHA로 바꾸어 적었다. 순서와 부모 순서(첫 부모 = 병합한 쪽)는 git 그대로다.
        std::vector<graph_commit> git_branch_merge()
        {
            return make_commits({ "M c f2", "f2 f1", "f1 b", "c b", "b a", "a" });
        }

        std::vector<graph_commit> git_criss_cross()
        {
            return make_commits({ "m3 m2", "m2 m1 y1", "y2 y1 m1", "m1 root", "y1 root", "root" });
        }

        std::vector<graph_commit> git_octopus_three_parents()
        {
            return make_commits({ "after octo", "octo m1 b1 b2", "b2 root", "b1 root", "m1 root", "root" });
        }

        std::vector<graph_commit> git_orphan_branch()
        {
            return make_commits({ "o2 o1", "o1", "b a", "a" });
        }
    } // namespace

    TEST_CASE("commit_graph_layout lays out a linear history in one lane", "[graph]")
    {
        const auto commits = make_commits({ "c b", "b a", "a" });
        commit_graph_layout layout {};
        const auto rows = layout.append(commits);

        CHECK(render(commits, rows) == std::vector<std::string> { "*  c", "*  b", "*  a" });
        CHECK(layout.max_lane_count() == 1);
        CHECK(layout.row_count() == 3);
        CHECK(layout.open_lane_count() == 0);
        for (const graph_row& row : rows)
        {
            CHECK(row.lane == 0);
            CHECK(row.color == rows.front().color);
        }

        CHECK(rows[0].has_unloaded_child);
        CHECK_FALSE(rows[1].has_unloaded_child);
        CHECK(rows[2].is_root);
        CHECK_FALSE(rows[0].is_root);
        CHECK_FALSE(rows[0].is_merge);
        check_invariants(rows, layout);
    }

    TEST_CASE("commit_graph_layout draws a branch and its merge", "[graph]")
    {
        const auto commits = git_branch_merge();
        commit_graph_layout layout {};
        const auto rows = layout.append(commits);

        CHECK(render(commits, rows) == std::vector<std::string> { "* \\  M", "| *  f2", "| *  f1", "* |  c", "* /  b", "*  a" });
        CHECK(layout.max_lane_count() == 2);

        CHECK(rows[0].is_merge);
        CHECK(rows[0].color == 0);
        CHECK(rows[1].color == 1);
        CHECK(rows[2].color == rows[1].color);
        CHECK(rows[4].color == rows[0].color);
        check_invariants(rows, layout);

        // 병합 부모로 가는 선은 새 레인의 색을 쓴다.
        CHECK(has_edge(rows[0], { 0, 1, 1, graph_edge_kind::outgoing }));
        // 갈라진 자리에서는 두 레인이 합류한다. 합류선의 색은 오는 쪽 레인의 색이다.
        CHECK(has_edge(rows[4], { 1, 0, 1, graph_edge_kind::incoming }));
    }

    TEST_CASE("commit_graph_layout handles criss-cross merges", "[graph]")
    {
        const auto commits = git_criss_cross();
        commit_graph_layout layout {};
        const auto rows = layout.append(commits);

        CHECK(render(commits, rows) == std::vector<std::string> { "*  m3", "* \\  m2", "/ | *  y2", "* | |  m1", "| * /  y1", "* /  root" });
        CHECK(layout.max_lane_count() == 3);
        CHECK(rows[2].is_merge);
        CHECK(rows[2].lane == 2);
        // 이미 다른 자식이 기다리는 부모(m1)로 가는 병합 선은 그 레인의 색을 쓴다.
        CHECK(has_edge(rows[2], { 2, 0, rows[0].color, graph_edge_kind::outgoing }));
        check_invariants(rows, layout);
    }

    TEST_CASE("commit_graph_layout fans out an octopus merge", "[graph]")
    {
        const auto commits = git_octopus_three_parents();
        commit_graph_layout layout {};
        const auto rows = layout.append(commits);

        CHECK(render(commits, rows) == std::vector<std::string> { "*  after", "* \\ \\  octo", "| | *  b2", "| * |  b1", "* | |  m1", "* / /  root" });
        CHECK(layout.max_lane_count() == 3);
        CHECK(rows[1].is_merge);
        CHECK(count_kind(rows[1], graph_edge_kind::outgoing) == 3);

        // 세 부모 선은 서로 다른 색이다.
        std::set<std::uint8_t> colors;
        for (const graph_edge& edge : rows[1].edges)
            colors.insert(edge.color);

        CHECK(colors.size() == 3);
        check_invariants(rows, layout);
    }

    TEST_CASE("commit_graph_layout reuses the lane for an orphan branch", "[graph]")
    {
        const auto commits = git_orphan_branch();
        commit_graph_layout layout {};
        const auto rows = layout.append(commits);

        CHECK(render(commits, rows) == std::vector<std::string> { "*  o2", "*  o1", "*  b", "*  a" });
        CHECK(rows[1].is_root);
        CHECK(rows[2].has_unloaded_child);
        // 새 선은 다음 색을 받는다.
        CHECK(rows[2].color != rows[0].color);
        CHECK(layout.max_lane_count() == 1);
        check_invariants(rows, layout);
    }

    TEST_CASE("commit_graph_layout runs two unrelated roots side by side", "[graph]")
    {
        const auto commits = make_commits({ "x1 x0", "y1 y0", "x0", "y0" });
        commit_graph_layout layout {};
        const auto rows = layout.append(commits);

        CHECK(render(commits, rows) == std::vector<std::string> { "*  x1", "| *  y1", "* |  x0", "  *  y0" });
        CHECK(layout.max_lane_count() == 2);
        check_invariants(rows, layout);
    }

    TEST_CASE("commit_graph_layout keeps a loaded tip open until its base arrives in a later page", "[graph]")
    {
        const auto commits = git_branch_merge();
        commit_graph_layout layout {};

        const auto first_page = layout.append(std::span<const graph_commit> { commits.data(), 3 });
        CHECK(layout.open_lane_count() == 2);
        CHECK(layout.max_lane_count() == 2);
        // f1의 부모 b는 아직 안 왔지만 f1이 그리는 선은 b의 레인으로 이어진다.
        CHECK(count_kind(first_page[2], graph_edge_kind::outgoing) == 1);

        const auto second_page = layout.append(std::span<const graph_commit> { commits.data() + 3, 3 });
        CHECK(layout.open_lane_count() == 0);
        // b는 앞 페이지에서 기다리던 레인으로 합류하므로 자식 없음으로 표시되지 않는다.
        CHECK_FALSE(second_page[1].has_unloaded_child);
        CHECK(second_page[1].lane == 0);

        std::vector<graph_row> rows = first_page;
        rows.insert(rows.end(), second_page.begin(), second_page.end());
        CHECK(rows == layout_all(commits));
        check_invariants(rows, layout);
    }

    TEST_CASE("commit_graph_layout flags commits whose children are not loaded", "[graph]")
    {
        // 페이지가 역사 중간에서 시작한 경우다. c와 f2는 자식이 로드되지 않았다.
        const auto commits = make_commits({ "c b", "f2 f1", "b a", "f1 b", "a" });
        const auto rows = layout_all(commits);

        CHECK(rows[0].has_unloaded_child);
        CHECK(rows[1].has_unloaded_child);
        CHECK_FALSE(rows[2].has_unloaded_child);
        CHECK_FALSE(rows[3].has_unloaded_child);
    }

    TEST_CASE("commit_graph_layout gives identical rows for any page size", "[graph]")
    {
        const std::vector<std::vector<graph_commit>> histories { git_branch_merge(), git_criss_cross(), git_octopus_three_parents(), git_orphan_branch(), make_synthetic_history(600, 12345) };

        for (const auto& commits : histories)
        {
            const auto whole = layout_all(commits);
            CHECK(layout_in_pages(commits, 1) == whole);
            CHECK(layout_in_pages(commits, 3) == whole);
            CHECK(layout_in_pages(commits, 7) == whole);
        }
    }

    TEST_CASE("commit_graph_layout never changes emitted rows when pages are appended", "[graph]")
    {
        const auto commits = make_synthetic_history(400, 777);
        commit_graph_layout layout {};
        std::vector<graph_row> rows;

        for (std::size_t begin = 0; begin < commits.size(); begin += 50)
        {
            const auto page = layout.append(std::span<const graph_commit> { commits.data() + begin, 50 });
            rows.insert(rows.end(), page.begin(), page.end());

            // 행 캐시와 앞부분만으로 처음부터 다시 계산한 결과가 같아야 한다.
            const std::vector<graph_commit> prefix { commits.begin(), commits.begin() + static_cast<std::ptrdiff_t>(begin + 50) };
            REQUIRE(rows == layout_all(prefix));
        }

        check_invariants(rows, layout);
    }

    TEST_CASE("commit_graph_layout reuses a free lane after a branch ends", "[graph]")
    {
        const auto commits = make_commits({ "x2 x1", "y2 y1", "z2 z1", "y1", "n1 n0", "n0", "x1", "z1" });
        commit_graph_layout layout {};
        const auto rows = layout.append(commits);

        CHECK(render(commits, rows) == std::vector<std::string> { "*  x2", "| *  y2", "| | *  z2", "| * |  y1", "| * |  n1", "| * |  n0", "*   |  x1", "    *  z1" });
        // 가운데 레인(1)이 y1에서 끝나고 새 선 n1이 그 자리를 다시 쓴다. 폭은 3으로 남는다.
        CHECK(rows[3].lane == 1);
        CHECK(rows[4].lane == 1);
        CHECK(layout.max_lane_count() == 3);
        // 재사용한 레인은 새 선이라 다음 색을 받는다.
        CHECK(rows[4].color != rows[1].color);
        check_invariants(rows, layout);
    }

    TEST_CASE("commit_graph_layout does not shift lanes when a left lane frees up", "[graph]")
    {
        const auto commits = make_commits({ "x2 x1", "y2 y1", "x1", "n1 n0", "y1", "n0" });
        commit_graph_layout layout {};
        const auto rows = layout.append(commits);

        // 레인 0이 비어도 y의 선은 레인 1에 그대로 남고 새 선이 비어 있는 0을 쓴다.
        CHECK(render(commits, rows) == std::vector<std::string> { "*  x2", "| *  y2", "* |  x1", "* |  n1", "| *  y1", "*  n0" });
        CHECK(rows[3].lane == 0);
        CHECK(rows[4].lane == 1);
        CHECK(layout.max_lane_count() == 2);
        check_invariants(rows, layout);
    }

    TEST_CASE("commit_graph_layout gives simultaneous lines distinct colors", "[graph]")
    {
        std::vector<graph_commit> commits;
        for (int index = 0; index < 10; ++index)
            commits.push_back(parse_commit("t" + std::to_string(index) + " p" + std::to_string(index)));

        const auto rows = layout_all(commits);
        std::set<std::uint8_t> first_eight;
        for (std::size_t index = 0; index < graph_palette_size; ++index)
            first_eight.insert(rows[index].color);

        CHECK(first_eight.size() == graph_palette_size);
        // 팔레트보다 선이 많으면 겹치는 것을 받아들이되 범위 안에서 돈다.
        CHECK(rows[8].color < graph_palette_size);
        CHECK(rows[9].color < graph_palette_size);
    }

    TEST_CASE("commit_graph_layout treats duplicate parents as one", "[graph]")
    {
        const auto commits = make_commits({ "m a a", "a" });
        const auto rows = layout_all(commits);

        CHECK_FALSE(rows[0].is_merge);
        CHECK(count_kind(rows[0], graph_edge_kind::outgoing) == 1);
    }

    TEST_CASE("commit_graph_layout connects the WIP row to the HEAD lane", "[graph][wip]")
    {
        auto commits = git_branch_merge();
        commits.insert(commits.begin(), make_wip_commit("M"));
        commit_graph_layout layout {};
        const auto rows = layout.append(commits);

        CHECK(render(commits, rows) == std::vector<std::string> { "*  WIP", "* \\  M", "| *  f2", "| *  f1", "* |  c", "* /  b", "*  a" });
        CHECK(rows[0].lane == rows[1].lane);
        CHECK(rows[0].color == rows[1].color);
        CHECK(rows[0].has_unloaded_child);
        CHECK_FALSE(rows[1].has_unloaded_child);
        // WIP 행의 outgoing과 HEAD 행의 incoming이 같은 레인·색으로 이어진다.
        CHECK(has_edge(rows[0], { 0, 0, rows[0].color, graph_edge_kind::outgoing }));
        CHECK(has_edge(rows[1], { 0, 0, rows[0].color, graph_edge_kind::incoming }));
        check_invariants(rows, layout);
    }

    TEST_CASE("commit_graph_layout keeps the WIP lane open down to a HEAD that is not at the top", "[graph][wip]")
    {
        auto commits = make_commits({ "m2 m1", "f2 f1", "m1 root", "f1 root", "root" });
        commits.insert(commits.begin(), make_wip_commit("f2"));
        commit_graph_layout layout {};
        const auto rows = layout.append(commits);

        CHECK(render(commits, rows) == std::vector<std::string> { "*  WIP", "| *  m2", "* |  f2", "| *  m1", "* |  f1", "* /  root" });
        CHECK(rows[2].lane == rows[0].lane);
        CHECK(rows[2].color == rows[0].color);
        check_invariants(rows, layout);
    }

    TEST_CASE("make_wip_commit builds a root row for an unborn HEAD", "[graph][wip]")
    {
        const std::vector<graph_commit> commits { make_wip_commit("") };
        const auto rows = layout_all(commits);

        CHECK(commits[0].sha == graph_wip_sha);
        CHECK(commits[0].parent_shas.empty());
        CHECK(rows[0].is_root);
        CHECK(make_wip_commit("abc").parent_shas == std::vector<std::string> { "abc" });
    }

    TEST_CASE("commit_graph_layout keeps lane continuity on synthetic histories", "[graph]")
    {
        for (const std::uint32_t seed : { 1u, 2u, 3u })
        {
            const auto commits = make_synthetic_history(2000, seed);
            commit_graph_layout layout {};
            const auto rows = layout.append(commits);
            check_invariants(rows, layout);
        }
    }

    // 성능 측정이다. 숨김 태그라 기본 실행과 ctest(tests/CMakeLists.txt)에서는 빠지고
    // 직접 부른다:  cyella_tests "[perf]"
    // Release에서만 200 ms 상한을 검사한다. Debug는 시간을 출력만 한다.
    TEST_CASE("commit_graph_layout lays out 100k merge-heavy commits quickly", "[.][perf]")
    {
        const auto commits = make_synthetic_history(100000, 42);
        std::size_t merges { 0 };
        for (const graph_commit& commit : commits)
            merges += commit.parent_shas.size() > 1 ? 1 : 0;

        commit_graph_layout layout {};
        const auto begin = std::chrono::steady_clock::now();
        const auto rows = layout.append(commits);
        const auto elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();

        std::printf("[perf] 100000 commits (%zu merges), max lanes %u: %.1f ms\n", merges, static_cast<unsigned>(layout.max_lane_count()), elapsed_ms);
        CHECK(rows.size() == 100000);
#ifdef NDEBUG
        CHECK(elapsed_ms < 200.0);
#endif
    }

    // 실제 저장소의 `git log --topo-order --format="%H %P" --all > file` 로그로 시간을 잰다.
    // 환경 변수 CYELLA_GRAPH_LOG에 파일 경로를 주면 돌고 없으면 건너뛴다.
    TEST_CASE("commit_graph_layout lays out a real git log file", "[.][perf][real]")
    {
        char* path = nullptr;
        std::size_t path_length { 0 };
        _dupenv_s(&path, &path_length, "CYELLA_GRAPH_LOG");
        const std::unique_ptr<char, decltype(&std::free)> path_owner { path, &std::free };
        if (path == nullptr)
            SKIP("CYELLA_GRAPH_LOG is not set");

        std::ifstream file { path };
        REQUIRE(file.is_open());

        std::vector<graph_commit> commits;
        std::string line;
        while (std::getline(file, line))
            if (!line.empty())
                commits.push_back(parse_commit(line));

        commit_graph_layout layout {};
        const auto begin = std::chrono::steady_clock::now();
        const auto rows = layout.append(commits);
        const auto elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();

        std::printf("[perf] real log %zu commits, max lanes %u: %.1f ms\n", commits.size(), static_cast<unsigned>(layout.max_lane_count()), elapsed_ms);
        check_invariants(rows, layout);
    }
} // namespace cyella
