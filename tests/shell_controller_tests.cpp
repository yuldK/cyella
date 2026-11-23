// 셸 controller의 화면 메시지 처리다 (탭 열기·닫기, 최근 목록, 테마, 고르기, 스크롤 다듬기, 입력 초안).
// workspace는 가짜 runner와 수동 dispatcher로 세운다 — job을 실행하지 않고 낸 것만 센다.
#include "application/git_workspace.h"
#include "infrastructure/git_operation_runner.h"
#include "presentation/commands/command_intents.h"
#include "presentation/dialog_intents.h"
#include "presentation/shell_controller.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/cyella_policy.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"

#include "luil/ui/transition.h"
#include "luil/ui/ui_tree.h"

#include "fake_git_runner.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
    using namespace cyella;

    struct controller_harness
    {
        controller_harness()
            : runner { process, u8"C:/Git/cmd/git.exe", files, {} }
            , workspace { runner, dispatcher.dispatcher(), events.sink(), nullptr, {} }
            , controller { make_dependencies(workspace) }
        {
            controller.start({});
        }

        static shell_controller::dependencies make_dependencies(git_workspace& workspace)
        {
            shell_controller::dependencies dependencies {};
            dependencies.workspace = &workspace;
            dependencies.clock = [] { return std::chrono::system_clock::time_point { std::chrono::seconds { 1760000000 } }; };
            dependencies.steady_clock = [] { return steady_now; };
            return dependencies;
        }

        // 토스트가 떠오르고 물러나는 시계다. test가 손으로 앞으로 보낸다.
        static inline std::chrono::steady_clock::time_point steady_now { std::chrono::hours { 1 } };

        [[nodiscard]] std::chrono::steady_clock::time_point steady() const noexcept
        {
            return steady_now;
        }

        void advance(const std::chrono::steady_clock::duration delta) noexcept
        {
            steady_now += delta;
        }

        template<typename intent_type>
        bool send(intent_type intent)
        {
            const bool handled { controller.handle(luil::app_message { std::move(intent) }) };
            controller.after_events();
            return handled;
        }

        test::fake_git_runner process {};
        test::fake_workspace_files files {};
        test::manual_dispatcher dispatcher {};
        test::event_queue events {};
        git_operation_runner runner;
        git_workspace workspace;
        shell_controller controller;
    };

    std::string sha_of(const int index)
    {
        char buffer[41] {};
        std::snprintf(buffer, sizeof(buffer), "%08x%032x", index, index);
        return buffer;
    }

    // 갈래가 있는 이력이다 (자식이 앞): 5 → 3 → 2 → 1(루트)과, 2에서 갈라진 옆 갈래 4.
    std::vector<commit_info> branching_history()
    {
        return {
            { .sha = sha_of(5), .parents = { sha_of(3) }, .subject = "five" },
            { .sha = sha_of(4), .parents = { sha_of(2) }, .subject = "four" },
            { .sha = sha_of(3), .parents = { sha_of(2) }, .subject = "three" },
            { .sha = sha_of(2), .parents = { sha_of(1) }, .subject = "two" },
            { .sha = sha_of(1), .subject = "one" },
        };
    }

    // 새로 고침 하나를 끝까지 넣는다. `wip`이면 작업 트리 변경이 있어 세션이 맨 앞에 WIP 행을 둔다.
    void load_history(repository_session& session, std::vector<commit_info> commits, const bool wip = false)
    {
        const std::uint64_t generation { session.begin_refresh() };
        repository_snapshot snapshot {};
        snapshot.summary.head_sha = commits.front().sha;
        snapshot.summary.changed_file_count = wip ? 1 : 0;
        REQUIRE(session.apply(snapshot_loaded { generation, snapshot }));
        REQUIRE(session.apply(commits_loaded { .generation = generation, .commits = std::move(commits), .page_complete = true, .history_complete = true }));
    }

    // 낸 operation 가운데 그 종류의 수다.
    std::size_t count_operations(const git_workspace& workspace, const operation_kind kind)
    {
        return static_cast<std::size_t>(std::ranges::count_if(workspace.operation_order(), [&](const operation_id id) { return workspace.find_operation(id)->kind == kind; }));
    }
} // namespace

TEST_CASE("resizing reveals the selected tab while manual scrolling keeps its position", "[presentation][controller][tabs]")
{
    controller_harness test {};
    for (int index { 0 }; index < 7; ++index)
        REQUIRE(test.send(new_tab_intent {}));
    const auto wide { ui::build_ui_tree(test.controller.make_view(1440.0f, 900.0f, 1.0f)) };
    test.controller.apply_feedback(wide.feedback);
    REQUIRE(test.send(scroll_tabs_intent { 0.0f, true }));
    const view_snapshot manual { test.controller.make_view(1440.0f, 900.0f, 1.0f) };
    CHECK_FALSE(manual.reveal_selected_tab);
    CHECK(manual.tab_scroll == 0.0f);
    const auto unchanged { ui::build_ui_tree(manual) };
    CHECK(unchanged.feedback.tab_scroll == 0.0f);
    test.controller.apply_feedback(unchanged.feedback);
    const view_snapshot narrow { test.controller.make_view(480.0f, 900.0f, 1.0f) };
    CHECK(narrow.reveal_selected_tab);
    const auto resized { ui::build_ui_tree(narrow) };
    REQUIRE(resized.feedback.tab_scroll.has_value());
    CHECK(*resized.feedback.tab_scroll > 0.0f);
}

TEST_CASE("opening a repository replaces the start tab and records it as recent", "[presentation][controller]")
{
    controller_harness test {};
    REQUIRE(test.controller.tabs().tabs().size() == 1);
    CHECK(test.controller.tabs().selected().kind == repository_tab_kind::start);

    test.controller.open_repository(u8"C:\\work\\repo");
    REQUIRE(test.controller.tabs().tabs().size() == 1);
    CHECK(test.controller.tabs().selected().kind == repository_tab_kind::repository);
    CHECK_FALSE(test.controller.selected_repository_id().empty());
    REQUIRE(test.controller.settings().recent_repositories.size() == 1);
    CHECK(test.controller.settings().recent_repositories[0].opened_at == u8"2025-10-09T08:53:20Z");
    CHECK(test.controller.settings().open_tabs == std::vector<std::u8string> { u8"C:\\work\\repo" });
    CHECK(test.controller.settings().active_tab == u8"C:\\work\\repo");

    // 같은 저장소(대소문자·구분자 무시)는 새 탭을 만들지 않는다.
    test.controller.open_repository(u8"c:/work/REPO/");
    CHECK(test.controller.tabs().tabs().size() == 1);
}

TEST_CASE("start with a relative path leaves no tab or open_tabs entry", "[presentation][controller]")
{
    test::fake_git_runner process {};
    test::fake_workspace_files files {};
    test::manual_dispatcher dispatcher {};
    test::event_queue events {};
    git_operation_runner runner { process, u8"C:/Git/cmd/git.exe", files, {} };
    git_workspace workspace { runner, dispatcher.dispatcher(), events.sink(), nullptr, {} };
    shell_controller controller { controller_harness::make_dependencies(workspace) };

    // 상대 경로는 앱의 현재 폴더에 따라 뜻이 바뀌어 다음 실행에서 다른 곳을 연다. 경고만 하고 남기지 않는다.
    controller.start(u8"work\\repo");
    REQUIRE(controller.tabs().tabs().size() == 1);
    CHECK(controller.tabs().selected().kind == repository_tab_kind::start);
    CHECK(workspace.repository_ids().empty());
    CHECK(controller.settings().open_tabs.empty());
    CHECK(controller.settings().recent_repositories.empty());
    CHECK_FALSE(controller.state().notifications.empty());

    controller.open_repository(u8"C:relative");
    CHECK(workspace.repository_ids().empty());
    CHECK(controller.settings().open_tabs.empty());
}

TEST_CASE("new tab, cycling and closing keep the workspace in step", "[presentation][controller]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\one");
    REQUIRE(test.send(new_tab_intent {}));
    CHECK(test.controller.tabs().tabs().size() == 2);
    CHECK(test.controller.tabs().selected().kind == repository_tab_kind::start);
    test.controller.open_repository(u8"C:\\work\\two");
    CHECK(test.controller.tabs().tabs().size() == 2);
    CHECK(test.workspace.repository_ids().size() == 2);

    REQUIRE(test.send(cycle_tab_intent { true }));
    CHECK(test.controller.tabs().selected().path == u8"C:\\work\\one");
    REQUIRE(test.send(close_selected_tab_intent {}));
    CHECK(test.controller.tabs().tabs().size() == 1);
    CHECK(test.workspace.repository_ids().size() == 1);
    CHECK(test.controller.settings().open_tabs == std::vector<std::u8string> { u8"C:\\work\\two" });
}

TEST_CASE("recent list pin and remove go to settings", "[presentation][controller]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    REQUIRE(test.send(pin_recent_intent { u8"C:\\work\\repo", true }));
    CHECK(test.controller.settings().recent_repositories[0].pinned);
    REQUIRE(test.send(remove_recent_intent { u8"C:\\work\\repo" }));
    CHECK(test.controller.settings().recent_repositories.empty());
}

TEST_CASE("the theme button cycles system, light and dark", "[presentation][controller]")
{
    controller_harness test {};
    REQUIRE(test.send(cycle_theme_intent {}));
    CHECK(test.controller.settings().appearance.theme == theme_preference::light);
    REQUIRE(test.send(cycle_theme_intent {}));
    CHECK(test.controller.settings().appearance.theme == theme_preference::dark);
    REQUIRE(test.send(cycle_theme_intent {}));
    CHECK(test.controller.settings().appearance.theme == theme_preference::system);
}

TEST_CASE("workspace navigation works for a clean repository and preserves the commit draft", "[presentation][controller][workspace]")
{
    controller_harness test {};
    REQUIRE(test.send(set_repository_layout_intent { false }));
    test.controller.open_repository(u8"C:\\work\\repo");
    REQUIRE(test.send(select_workspace_intent { true }));
    const auto id { test.controller.tabs().selected().id };
    CHECK(test.controller.state().tabs.at(id).selected_sha == "WIP");
    luil::text_edit_request insert {};
    insert.target = to_luil_target(text_target::commit_subject);
    insert.command = luil::text::text_edit_command::insert;
    insert.text = u8"작성 중인 메시지";
    REQUIRE(test.send(edit_text_intent { insert }));
    REQUIRE(test.send(select_workspace_intent { false }));
    CHECK(test.controller.state().tabs.at(id).selected_sha.empty());
    REQUIRE(test.send(select_workspace_intent { true }));
    CHECK(test.controller.state().tabs.at(id).commit_subject.text == u8"작성 중인 메시지");
    CHECK_FALSE(test.controller.state().drawer_open);
}

TEST_CASE("selecting a commit submits show_commit and WIP does not", "[presentation][controller]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    repository_session* const session { test.workspace.find_session(test.controller.selected_repository_id()) };
    REQUIRE(session != nullptr);
    const std::uint64_t generation { session->begin_refresh() };
    repository_snapshot snapshot {};
    snapshot.summary.head_sha = sha_of(2);
    snapshot.summary.changed_file_count = 1;
    REQUIRE(session->apply(snapshot_loaded { generation, snapshot }));
    std::vector<commit_info> commits { { .sha = sha_of(2), .parents = { sha_of(1) } }, { .sha = sha_of(1) } };
    REQUIRE(session->apply(commits_loaded { .generation = generation, .commits = commits, .page_complete = true, .history_complete = true }));

    REQUIRE(test.send(select_commit_intent { u8"WIP" }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).selected_sha == "WIP");
    // WIP 행은 show_commit을 내지 않는다. (보이는 커밋의 서명을 읽는 자동 operation은 나갈 수 있다.)
    for (const operation_id id : test.workspace.operation_order())
        CHECK(test.workspace.find_operation(id)->kind != operation_kind::show_commit);
    const std::size_t before { test.dispatcher.pending() };

    REQUIRE(test.send(select_commit_intent { graph_item_key(sha_of(1)) }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).selected_sha == sha_of(1));
    CHECK(test.dispatcher.pending() == before + 1);

    // 상세가 서랍이 아닌 wide 배치에서는 서랍을 열지 않는다.
    CHECK_FALSE(test.controller.state().drawer_open);
    REQUIRE(test.send(select_workspace_intent { true }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).selected_sha == "WIP");
    REQUIRE(test.send(select_workspace_intent { false }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).selected_sha == sha_of(1));
}

TEST_CASE("choosing a changed file in the drawer closes it so the diff is not covered", "[presentation][controller]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    repository_session* const session { test.workspace.find_session(test.controller.selected_repository_id()) };
    REQUIRE(session != nullptr);
    const std::uint64_t generation { session->begin_refresh() };
    repository_snapshot snapshot {};
    snapshot.summary.head_sha = sha_of(2);
    REQUIRE(session->apply(snapshot_loaded { generation, snapshot }));
    std::vector<commit_info> commits { { .sha = sha_of(2), .parents = { sha_of(1) } }, { .sha = sha_of(1) } };
    REQUIRE(session->apply(commits_loaded { .generation = generation, .commits = commits, .page_complete = true, .history_complete = true }));

    // medium 배치(논리 폭 1000)에서는 상세가 서랍이다. 커밋을 고르면 서랍이 열린다.
    static_cast<void>(test.controller.make_view(1000.0f, 800.0f, 1.0f));
    REQUIRE(test.send(select_commit_intent { graph_item_key(sha_of(1)) }));
    REQUIRE(test.controller.state().drawer_open);

    // 서랍의 변경 파일을 고르면 그래프 자리에 diff가 열리고 서랍은 닫힌다.
    REQUIRE(test.send(select_file_intent { diff_file_request { .path = "a.txt", .view = diff_view::commit, .sha = sha_of(1) } }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).diff.has_value());
    CHECK_FALSE(test.controller.state().drawer_open);
}

TEST_CASE("the commit file menu opens the diff and copies the path forms", "[presentation][controller]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    const menu_target target { .kind = menu_target_kind::commit_file, .name = "src/app/a.cpp", .sha = sha_of(1) };

    REQUIRE(test.send(open_context_menu_intent { target, 0.0f, 0.0f }));
    REQUIRE(test.send(menu_select_intent { u8"open_diff" }));
    const auto& diff { test.controller.state().tabs.at(test.controller.tabs().selected().id).diff };
    REQUIRE(diff.has_value());
    CHECK(diff->path == "src/app/a.cpp");
    CHECK(diff->view == diff_view::commit);
    CHECK(diff->sha == sha_of(1));

    const auto copied = [&test, &target](const std::u8string& key) {
        REQUIRE(test.send(open_context_menu_intent { target, 0.0f, 0.0f }));
        REQUIRE(test.send(menu_select_intent { key }));
        return test.controller.take_clipboard_text().value_or(u8"");
    };
    CHECK(copied(u8"copy_path") == u8"src/app/a.cpp");
    CHECK(copied(u8"copy_name") == u8"a.cpp");
    const std::string prefix { sha_of(1).substr(0, 7) };
    CHECK(copied(u8"copy_revision_path") == std::u8string { prefix.begin(), prefix.end() } + u8":src/app/a.cpp");
}

TEST_CASE("ctrl+click compares the anchor with the clicked commit, older on the left", "[presentation][controller][compare]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    repository_session* const session { test.workspace.find_session(test.controller.selected_repository_id()) };
    REQUIRE(session != nullptr);
    load_history(*session, branching_history());
    const auto tab = [&test]() -> const tab_ui_state& { return test.controller.state().tabs.at(test.controller.tabs().selected().id); };

    // 앞서 고른 커밋이 없으면 Ctrl+클릭도 그냥 고르기다.
    REQUIRE(test.send(select_commit_intent { .key = graph_item_key(sha_of(3)), .control = true }));
    CHECK(tab().selected_sha == sha_of(3));
    CHECK_FALSE(tab().compare.has_value());

    // 다른 갈래의 4를 Ctrl+클릭한다. 그래프 뒤쪽의 3이 오래된 쪽이라 왼쪽이다.
    const std::size_t ranges_before { count_operations(test.workspace, operation_kind::show_range) };
    REQUIRE(test.send(select_commit_intent { .key = graph_item_key(sha_of(4)), .control = true }));
    REQUIRE(tab().compare.has_value());
    CHECK(tab().compare->anchor == sha_of(3));
    CHECK(tab().compare->kind == commit_compare_kind::pair);
    CHECK(tab().compare->request == show_range_request { .base = sha_of(3), .tip = sha_of(4), .range = false });
    CHECK(tab().selected_sha == sha_of(4));
    CHECK(count_operations(test.workspace, operation_kind::show_range) == ranges_before + 1);

    // 거듭 누르면 기준은 그대로이고 마지막 커밋만 바뀐다. 기준보다 새 커밋이면 기준이 왼쪽이다.
    REQUIRE(test.send(select_commit_intent { .key = graph_item_key(sha_of(1)), .control = true }));
    REQUIRE(tab().compare.has_value());
    CHECK(tab().compare->anchor == sha_of(3));
    CHECK(tab().compare->request == show_range_request { .base = sha_of(1), .tip = sha_of(3), .range = false });

    // 그래프는 고른 두 행을 함께 칠하고, 상세 패널은 결과가 오기 전까지 읽는 중이다.
    view_snapshot view { test.controller.make_view(1440.0f, 900.0f, 1.0f) };
    REQUIRE(view.repository.has_value());
    CHECK(view.repository->graph.selection_keys == std::vector<std::u8string> { graph_item_key(sha_of(3)), graph_item_key(sha_of(1)) });
    REQUIRE(view.repository->commit.has_value());
    REQUIRE(view.repository->commit->compare.has_value());
    CHECK_FALSE(view.repository->commit->compare->range);
    CHECK(view.repository->commit->compare->older_subject == u8"one");
    CHECK(view.repository->commit->compare->newer_subject == u8"three");
    CHECK(view.repository->commit->loading);

    // 결과가 오면 변경 파일이 보이고, 파일을 고르면 비교의 diff를 연다.
    range_detail detail { .request = tab().compare->request, .base = sha_of(1), .files = { file_change { .path = "a.txt", .status = 'M' } }, .commit_count = std::nullopt };
    operation_id last_range { 0 };
    for (const operation_id id : test.workspace.operation_order())
        if (test.workspace.find_operation(id)->kind == operation_kind::show_range)
            last_range = id;
    REQUIRE(session->apply(last_range, range_detail_loaded { detail }));
    view = test.controller.make_view(1440.0f, 900.0f, 1.0f);
    REQUIRE(view.repository->commit.has_value());
    CHECK_FALSE(view.repository->commit->loading);
    CHECK(view.repository->commit->sha == sha_of(3));
    REQUIRE(view.repository->commit->files.size() == 1);
    CHECK(view.repository->commit->compare->base == sha_of(1));
}

TEST_CASE("shift+click takes the range between an ancestor and its descendant", "[presentation][controller][compare]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    repository_session* const session { test.workspace.find_session(test.controller.selected_repository_id()) };
    REQUIRE(session != nullptr);
    load_history(*session, branching_history());
    const auto tab = [&test]() -> const tab_ui_state& { return test.controller.state().tabs.at(test.controller.tabs().selected().id); };

    REQUIRE(test.send(select_commit_intent { graph_item_key(sha_of(2)) }));
    REQUIRE(test.send(select_commit_intent { .key = graph_item_key(sha_of(5)), .shift = true }));
    REQUIRE(tab().compare.has_value());
    CHECK(tab().compare->kind == commit_compare_kind::range);
    // 범위의 왼쪽은 가장 오래된 커밋의 첫 부모다.
    CHECK(tab().compare->request == show_range_request { .base = sha_of(1), .tip = sha_of(5), .range = true });
    // 그래프의 강조는 `1..5`의 커밋이다. 옆 갈래 4는 범위에 없다.
    const view_snapshot view { test.controller.make_view(1440.0f, 900.0f, 1.0f) };
    REQUIRE(view.repository.has_value());
    CHECK(view.repository->graph.selection_keys == std::vector<std::u8string> { graph_item_key(sha_of(5)), graph_item_key(sha_of(3)), graph_item_key(sha_of(2)) });
    REQUIRE(view.repository->commit.has_value());
    REQUIRE(view.repository->commit->compare.has_value());
    CHECK(view.repository->commit->compare->range);
    CHECK(view.repository->commit->compare->count == 3);

    SECTION("a range from the root commit compares with the empty tree")
    {
        REQUIRE(test.send(select_commit_intent { .key = graph_item_key(sha_of(1)), .shift = true }));
        REQUIRE(tab().compare.has_value());
        CHECK(tab().compare->anchor == sha_of(2));
        CHECK(tab().compare->kind == commit_compare_kind::range);
        CHECK(tab().compare->request == show_range_request { .base = {}, .tip = sha_of(2), .range = true });
    }

    SECTION("shift+click on another branch falls back to comparing two commits")
    {
        REQUIRE(test.send(select_commit_intent { graph_item_key(sha_of(3)) }));
        REQUIRE(test.send(select_commit_intent { .key = graph_item_key(sha_of(4)), .shift = true }));
        REQUIRE(tab().compare.has_value());
        CHECK(tab().compare->kind == commit_compare_kind::pair);
        CHECK(tab().compare->request == show_range_request { .base = sha_of(3), .tip = sha_of(4), .range = false });
    }
}

TEST_CASE("dragging over graph rows selects from the pressed commit to the row under the pointer", "[presentation][controller][compare]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    repository_session* const session { test.workspace.find_session(test.controller.selected_repository_id()) };
    REQUIRE(session != nullptr);
    load_history(*session, branching_history());
    const auto tab = [&test]() -> const tab_ui_state& { return test.controller.state().tabs.at(test.controller.tabs().selected().id); };
    // 목록 위 끝이 창 좌표 100(물리 픽셀)이고 배율 1.5다. 행 하나는 28 논리 픽셀이다.
    test.controller.apply_feedback(frame_feedback { .graph_maximum = 0.0f, .graph_top = 100.0f, .scale = 1.5f });
    const std::vector<commit_info>& commits { session->graph_commits() };
    const auto row_y = [&commits](const std::string& sha) {
        const auto index { static_cast<float>(std::ranges::find(commits, sha, &commit_info::sha) - commits.begin()) };
        return 100.0f + (index * graph_row_height + graph_row_height / 2.0f) * 1.5f;
    };

    // 2를 누르고 위의 5까지 끈다. 2가 5의 조상이라 범위다 (5를 Shift+클릭한 것과 같다).
    REQUIRE(test.send(select_commit_intent { graph_item_key(sha_of(2)) }));
    REQUIRE(test.send(graph_sweep_intent { row_y(sha_of(5)) }));
    REQUIRE(tab().compare.has_value());
    CHECK(tab().compare->anchor == sha_of(2));
    CHECK(tab().selected_sha == sha_of(5));
    CHECK(tab().compare->kind == commit_compare_kind::range);

    // 같은 행 안의 이동은 비교를 다시 내지 않는다.
    const std::size_t ranges { count_operations(test.workspace, operation_kind::show_range) };
    REQUIRE(test.send(graph_sweep_intent { row_y(sha_of(5)) + 5.0f }));
    CHECK(count_operations(test.workspace, operation_kind::show_range) == ranges);

    // 목록 밖으로 끌면 끝 행에 멈춘다 (위는 첫 행, 아래는 마지막 행).
    REQUIRE(test.send(graph_sweep_intent { 4000.0f }));
    CHECK(tab().selected_sha == commits.back().sha);
    REQUIRE(test.send(graph_sweep_intent { -50.0f }));
    CHECK(tab().selected_sha == commits.front().sha);

    // 누른 행으로 돌아오면 그 커밋 하나다.
    REQUIRE(test.send(graph_sweep_intent { row_y(sha_of(2)) }));
    CHECK(tab().selected_sha == sha_of(2));
    CHECK_FALSE(tab().compare.has_value());
}

TEST_CASE("a plain click, Esc, WIP and a refresh that drops a commit end the comparison", "[presentation][controller][compare]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    repository_session* const session { test.workspace.find_session(test.controller.selected_repository_id()) };
    REQUIRE(session != nullptr);
    load_history(*session, branching_history(), true);
    const auto tab = [&test]() -> const tab_ui_state& { return test.controller.state().tabs.at(test.controller.tabs().selected().id); };
    const auto compare_three_and_five = [&test] {
        REQUIRE(test.send(select_commit_intent { graph_item_key(sha_of(3)) }));
        REQUIRE(test.send(select_commit_intent { .key = graph_item_key(sha_of(5)), .control = true }));
    };

    SECTION("a plain click selects one commit")
    {
        compare_three_and_five();
        REQUIRE(tab().compare.has_value());
        // 마지막에 누른 커밋을 그냥 눌러도 비교가 풀리고 그 커밋 하나가 남는다.
        REQUIRE(test.send(select_commit_intent { graph_item_key(sha_of(5)) }));
        CHECK_FALSE(tab().compare.has_value());
        CHECK(tab().selected_sha == sha_of(5));
        const view_snapshot view { test.controller.make_view(1440.0f, 900.0f, 1.0f) };
        CHECK(view.repository->graph.selection_keys.empty());
        REQUIRE(view.repository->commit.has_value());
        CHECK_FALSE(view.repository->commit->compare.has_value());
    }

    SECTION("Esc closes the comparison diff first and then the comparison")
    {
        compare_three_and_five();
        REQUIRE(test.send(select_file_intent { diff_file_request { .path = "a.txt", .view = diff_view::range, .sha = sha_of(5), .base = sha_of(3) } }));
        REQUIRE(test.send(close_overlays_intent {}));
        CHECK_FALSE(tab().diff.has_value());
        CHECK(tab().compare.has_value());
        REQUIRE(test.send(close_overlays_intent {}));
        CHECK_FALSE(tab().compare.has_value());
        CHECK(tab().selected_sha == sha_of(5));
    }

    SECTION("WIP is never part of a comparison")
    {
        compare_three_and_five();
        REQUIRE(test.send(select_commit_intent { .key = u8"WIP", .control = true }));
        CHECK_FALSE(tab().compare.has_value());
        CHECK(tab().selected_sha == "WIP");
        // WIP에서 Ctrl+클릭해도 비교가 서지 않는다.
        REQUIRE(test.send(select_commit_intent { .key = graph_item_key(sha_of(2)), .control = true }));
        CHECK_FALSE(tab().compare.has_value());
        CHECK(tab().selected_sha == sha_of(2));
    }

    SECTION("a refresh without the chosen commit ends it, one that keeps both does not")
    {
        compare_three_and_five();
        load_history(*session, branching_history());
        test.controller.after_events();
        CHECK(tab().compare.has_value());

        std::vector<commit_info> rewritten { branching_history() };
        rewritten.erase(rewritten.begin());
        load_history(*session, rewritten);
        test.controller.after_events();
        CHECK_FALSE(tab().compare.has_value());
    }
}

TEST_CASE("the commit file menu of a comparison opens the range diff", "[presentation][controller][compare]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    const menu_target target { .kind = menu_target_kind::commit_file, .name = "a.txt", .sha = sha_of(5), .base = sha_of(3) };
    REQUIRE(test.send(open_context_menu_intent { target, 0.0f, 0.0f }));
    REQUIRE(test.send(menu_select_intent { u8"open_diff" }));
    const auto& diff { test.controller.state().tabs.at(test.controller.tabs().selected().id).diff };
    REQUIRE(diff.has_value());
    CHECK(diff->view == diff_view::range);
    CHECK(diff->sha == sha_of(5));
    CHECK(diff->base == sha_of(3));
}

TEST_CASE("scroll is clamped to the last arranged maximum", "[presentation][controller]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    test.controller.apply_feedback({ .graph_maximum = 100.0f, .graph_viewport = 400.0f });
    REQUIRE(test.send(scroll_intent { .target = scroll_target::graph, .value = 80.0f }));
    REQUIRE(test.send(scroll_intent { .target = scroll_target::graph, .value = 80.0f }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).graph_scroll == 100.0f);
    REQUIRE(test.send(scroll_intent { .target = scroll_target::graph, .value = -500.0f }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).graph_scroll == 0.0f);
    REQUIRE(test.send(scroll_intent { .target = scroll_target::graph, .value = 40.0f, .absolute = true }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).graph_scroll == 40.0f);
    test.controller.apply_feedback({ .working_maximum = 75.0f });
    REQUIRE(test.send(scroll_intent { .target = scroll_target::working_area, .value = 1000.0f, .absolute = true }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).working_scroll == 75.0f);
    REQUIRE(test.send(scroll_intent { .target = scroll_target::working_area, .value = -1000.0f }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).working_scroll == 0.0f);
}

TEST_CASE("text edits land in the targeted draft and stay on one line", "[presentation][controller]")
{
    controller_harness test {};
    luil::text_edit_request insert {};
    insert.target = to_luil_target(text_target::start_search);
    insert.command = luil::text::text_edit_command::insert;
    insert.text = u8"cy\nella";
    REQUIRE(test.send(edit_text_intent { insert }));
    CHECK(test.controller.state().start_search.text == u8"cy ella");

    luil::text_edit_request clear {};
    clear.target = to_luil_target(text_target::start_search);
    clear.command = luil::text::text_edit_command::clear;
    REQUIRE(test.send(edit_text_intent { clear }));
    CHECK(test.controller.state().start_search.text.empty());
}

TEST_CASE("the commit body preserves line breaks and Ctrl+Home and Ctrl+End reach the ends of the text", "[presentation][controller][working]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    luil::text_edit_request request {};
    request.target = to_luil_target(text_target::commit_body);
    request.command = luil::text::text_edit_command::insert;
    request.text = u8"first line\nsecond line";
    REQUIRE(test.send(edit_text_intent { request }));
    const auto& tabs { test.controller.state().tabs };
    REQUIRE(tabs.size() == 1);
    CHECK(tabs.begin()->second.commit_body.text == u8"first line\nsecond line");
    // 여러 줄 칸의 Home·End는 luil이 줄 끝으로 옮긴다. 줄 처음·끝 명령으로 오는 것은 Ctrl+Home·End라 글 전체의 처음·끝이다.
    request.command = luil::text::text_edit_command::move_line_start;
    request.text.clear();
    REQUIRE(test.send(edit_text_intent { request }));
    CHECK(tabs.begin()->second.commit_body.caret == 0);
    request.command = luil::text::text_edit_command::move_line_end;
    REQUIRE(test.send(edit_text_intent { request }));
    CHECK(tabs.begin()->second.commit_body.caret == tabs.begin()->second.commit_body.text.size());
    request.command = luil::text::text_edit_command::move_line_start;
    REQUIRE(test.send(edit_text_intent { request }));
    REQUIRE(test.send(commit_body_line_intent { true, false }));
    CHECK(tabs.begin()->second.commit_body.caret == 11);
}

TEST_CASE("text pasted into the commit body turns CRLF and CR into line breaks", "[presentation][controller][working]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    luil::text_edit_request request {};
    request.target = to_luil_target(text_target::commit_body);
    request.command = luil::text::text_edit_command::insert;
    request.text = u8"a\r\nb\rc\td";
    REQUIRE(test.send(edit_text_intent { request }));
    CHECK(test.controller.state().tabs.begin()->second.commit_body.text == u8"a\nb\nc\td");
}

TEST_CASE("toolbar actions without a dialog submit operations and push without a remote opens the add remote dialog", "[presentation][controller]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    const std::size_t before { test.dispatcher.pending() };
    REQUIRE(test.send(toolbar_intent { toolbar_action::fetch }));
    CHECK(test.dispatcher.pending() == before + 1);
    // upstream이 없는 Push는 대화 상자가 필요하다. 이 저장소는 원격이 없어 원격을 더하는 상자가 열린다.
    REQUIRE(test.send(toolbar_intent { toolbar_action::push }));
    CHECK(test.dispatcher.pending() == before + 1);
    REQUIRE(test.controller.state().dialog.has_value());
    CHECK(test.controller.state().dialog->kind == dialog_kind::add_remote);
    REQUIRE(test.send(show_notice_intent { u8"원격을 더하세요.", false }));
    REQUIRE(test.controller.state().latest_notification() != nullptr);
    // ×로 닫은 토스트는 물러난 뒤 지워진다.
    REQUIRE(test.send(dismiss_notification_intent {}));
    test.controller.tick(test.steady() + toast_leave_duration);
    CHECK(test.controller.state().latest_notification() == nullptr);
}

TEST_CASE("notices rise as toasts, leave smoothly when they time out and stay in the notification center", "[presentation][controller][notifications]")
{
    controller_harness test {};
    REQUIRE(test.send(show_notice_intent { u8"첫 안내", false }));
    const notification* first { test.controller.state().latest_notification() };
    REQUIRE(first != nullptr);
    CHECK(first->severity == notification_severity::info);
    CHECK(first->toast);
    CHECK(first->toast_shown_at == test.steady());
    REQUIRE(first->toast_until.has_value());
    const auto expires { *first->toast_until };

    // 떠오르는 동안은 frame마다 다시 짓고, 다 떠오르면 내려갈 시각까지 잠잔다.
    REQUIRE(test.controller.next_tick().has_value());
    CHECK(*test.controller.next_tick() == test.steady() + luil::transition_frame);
    CHECK(test.controller.make_view(1440.0f, 900.0f, 1.0f).toasts.front().presence == 0.0f);
    test.advance(toast_enter_duration / 2);
    const float rising { test.controller.make_view(1440.0f, 900.0f, 1.0f).toasts.front().presence };
    CHECK(rising > 0.0f);
    CHECK(rising < 1.0f);
    test.advance(toast_enter_duration);
    CHECK(test.controller.make_view(1440.0f, 900.0f, 1.0f).toasts.front().presence == 1.0f);
    REQUIRE(test.controller.next_tick().has_value());
    CHECK(*test.controller.next_tick() == expires);

    // 시간이 되면 물러나기 시작하고, 물러남이 끝나야 토스트에서 빠진다. 센터에는 남는다.
    test.controller.tick(expires);
    REQUIRE(test.controller.state().latest_notification()->toast_leaving_at.has_value());
    CHECK(test.controller.state().latest_notification()->toast);
    test.controller.tick(expires + toast_leave_duration);
    CHECK_FALSE(test.controller.state().latest_notification()->toast);
    CHECK_FALSE(test.controller.next_tick().has_value());
    view_snapshot view { test.controller.make_view(1440.0f, 900.0f, 1.0f) };
    CHECK(view.toasts.empty());
    CHECK(view.status.notifications == 1);
    CHECK(view.status.unread_notifications == 1);

    // 떠 있는 동안 같은 안내가 또 오면 하나로 합치고, 다시 떠오르지 않는다. 내려간 것은 센터에 그대로 남는다.
    REQUIRE(test.send(show_notice_intent { u8"첫 안내", false }));
    const auto risen_at { test.controller.state().latest_notification()->toast_shown_at };
    test.advance(std::chrono::seconds { 1 });
    REQUIRE(test.send(show_notice_intent { u8"첫 안내", false }));
    CHECK(test.controller.state().notifications.size() == 2);
    CHECK(test.controller.state().latest_notification()->toast_shown_at == risen_at);

    // 종을 누르면 센터가 열리고 모두 읽은 것이 되며 토스트는 내려간다.
    REQUIRE(test.send(toggle_notification_center_intent {}));
    view = test.controller.make_view(1440.0f, 900.0f, 1.0f);
    CHECK(view.toasts.empty());
    REQUIRE(view.notification_center.has_value());
    CHECK(view.notification_center->entries.size() == 2);
    CHECK(view.status.unread_notifications == 0);
    const ui::built_tree built { ui::build_ui_tree(view) };
    CHECK(std::any_of(built.popups.begin(), built.popups.end(), [](const ui::built_popup& popup) { return popup.notifications; }));
    // 센터가 열려 있는 동안 온 알림은 토스트로 뜨지 않고 센터에 바로 보인다.
    REQUIRE(test.send(show_notice_intent { u8"새 안내", true }));
    CHECK_FALSE(test.controller.state().latest_notification()->toast);
    CHECK(test.controller.state().latest_notification()->read);

    // Esc는 센터를 먼저 닫는다. 바깥 누름으로 닫힌 바로 뒤의 종 누름은 그 누름이라 다시 열지 않는다.
    REQUIRE(test.send(close_overlays_intent {}));
    CHECK_FALSE(test.controller.state().notifications_open);
    REQUIRE(test.send(toggle_notification_center_intent {}));
    REQUIRE(test.send(close_notification_center_intent { .dismissed = true }));
    REQUIRE(test.send(toggle_notification_center_intent {}));
    CHECK_FALSE(test.controller.state().notifications_open);
    REQUIRE(test.send(toggle_notification_center_intent {}));
    CHECK(test.controller.state().notifications_open);

    // 센터는 카드마다 심각도 원과 [×]를 가진 카드를 쌓고, 머리에서 종류를 거른다. 다시 열면 거르기는 전체로 돌아간다.
    {
        const ui::built_tree center_tree { ui::build_ui_tree(test.controller.make_view(1440.0f, 900.0f, 1.0f)) };
        const auto popup { std::find_if(center_tree.popups.begin(), center_tree.popups.end(), [](const ui::built_popup& value) { return value.notifications; }) };
        REQUIRE(popup != center_tree.popups.end());
        CHECK(popup->corner == luil::popup_corner::round);
        const std::u8string card { u8"notification-" + ui::number_text(test.controller.state().notifications.back().id) };
        CHECK(popup->tree->find({ ui::kind_panel, card }) != nullptr);
        CHECK(popup->tree->find({ ui::kind_button, card + u8"-close" }) != nullptr);
        CHECK(popup->tree->find({ ui::kind_segmented, u8"notification-filter" }) != nullptr);
    }
    REQUIRE(test.send(filter_notifications_intent { notification_filter::warning }));
    view = test.controller.make_view(1440.0f, 900.0f, 1.0f);
    REQUIRE(view.notification_center.has_value());
    REQUIRE(view.notification_center->entries.size() == 1);
    CHECK(view.notification_center->entries.front().severity == notification_severity::warning);
    REQUIRE(test.send(toggle_notification_center_intent {}));
    REQUIRE(test.send(toggle_notification_center_intent {}));
    CHECK(test.controller.state().notifications_filter == notification_filter::all);

    // 센터의 항목은 ×로 바로 지운다.
    REQUIRE(test.send(dismiss_notification_intent { test.controller.state().notifications.front().id }));
    CHECK(test.controller.state().notifications.size() == 2);
    REQUIRE(test.send(clear_notifications_intent {}));
    CHECK(test.controller.state().notifications.empty());
}

TEST_CASE("closing a toast lets it leave first and then removes it, and Esc sends every toast away", "[presentation][controller][notifications]")
{
    controller_harness test {};
    REQUIRE(test.send(show_notice_intent { u8"첫 안내", false }));
    REQUIRE(test.send(show_notice_intent { u8"두 번째 안내", true }));
    test.advance(toast_enter_duration);
    const std::uint64_t closed { test.controller.state().notifications.front().id };

    REQUIRE(test.send(dismiss_notification_intent { closed }));
    // 물러나는 동안은 아직 목록에 있고 덜 나와 있다.
    REQUIRE(test.controller.state().notifications.size() == 2);
    test.advance(toast_leave_duration / 2);
    view_snapshot view { test.controller.make_view(1440.0f, 900.0f, 1.0f) };
    REQUIRE(view.toasts.size() == 2);
    CHECK(view.toasts.back().id == closed);
    CHECK(view.toasts.back().presence < 1.0f);
    CHECK(view.toasts.front().presence == 1.0f);
    test.controller.tick(test.steady() + toast_leave_duration);
    REQUIRE(test.controller.state().notifications.size() == 1);
    CHECK(test.controller.state().notifications.front().id != closed);

    // Esc는 남은 토스트를 물러나게 할 뿐 센터에서 지우지 않는다.
    REQUIRE(test.send(close_overlays_intent {}));
    test.controller.tick(test.steady() + toast_leave_duration);
    REQUIRE(test.controller.state().notifications.size() == 1);
    CHECK_FALSE(test.controller.state().notifications.front().toast);
}

TEST_CASE("toasts float above the body as rounded cards and the status bar bell shows unread notifications", "[presentation][controller][notifications]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    ui::built_tree built { ui::build_ui_tree(test.controller.make_view(1440.0f, 900.0f, 1.0f)) };
    const luil::ui_element* bell { built.tree->find({ ui::kind_button, u8"status-notifications" }) };
    REQUIRE(bell != nullptr);
    CHECK(built.popups.empty());

    REQUIRE(test.send(show_notice_intent { u8"안내입니다.", true }));
    REQUIRE(test.send(show_notice_intent { u8"두 번째 안내입니다.", false }));
    test.advance(toast_enter_duration);
    const view_snapshot view { test.controller.make_view(1440.0f, 900.0f, 1.0f) };
    built = ui::build_ui_tree(view);
    // 토스트는 popup 창이 아니라 주 tree의 카드다 (둥근 모서리와 부드러운 그림자·흐려지기를 그린다).
    CHECK(built.popups.empty());
    REQUIRE(view.toasts.size() == 2);
    const luil::ui_element* newest { built.tree->find({ ui::kind_panel, u8"toast-" + ui::number_text(view.toasts[0].id) }) };
    const luil::ui_element* older { built.tree->find({ ui::kind_panel, u8"toast-" + ui::number_text(view.toasts[1].id) }) };
    REQUIRE(newest != nullptr);
    REQUIRE(older != nullptr);
    // 최신이 상태 줄 가까이(아래)에 오고 앞의 것이 그 위에 쌓인다. 둘 다 오른쪽 끝에서 조금 떨어져 뜬다.
    CHECK(newest->bounds().y > older->bounds().y);
    CHECK(newest->bounds().y + newest->bounds().height < 900.0f - shell_status_height);
    CHECK(newest->bounds().x + newest->bounds().width < 1440.0f);
    CHECK(newest->bounds().x > 720.0f);
    CHECK(built.tree->find({ ui::kind_button, u8"toast-" + ui::number_text(view.toasts[0].id) + u8"-close" }) != nullptr);
    CHECK(built.tree->find({ ui::kind_button, u8"status-notifications" })->tooltip().find(u8"2") != std::u8string::npos);
}

TEST_CASE("pane resizing is clamped", "[presentation][controller]")
{
    controller_harness test {};
    REQUIRE(test.send(resize_pane_intent { .sidebar = true, .delta = 10000.0f }));
    CHECK(test.controller.state().sidebar_width == maximum_sidebar_width);
    REQUIRE(test.send(resize_pane_intent { .sidebar = false, .delta = -10000.0f }));
    CHECK(test.controller.state().detail_width == minimum_detail_width);
}

TEST_CASE("pane widths are saved after the drag settles and restored on start", "[presentation][controller]")
{
    controller_harness test {};
    REQUIRE(test.send(resize_pane_intent { .sidebar = true, .delta = 40.0f }));
    REQUIRE(test.send(resize_pane_intent { .sidebar = false, .delta = -30.0f }));
    // 끄는 동안에는 쓰지 않는다. 쓸 시각만 예약한다.
    CHECK_FALSE(test.controller.settings().ui.pane_widths.sidebar.has_value());
    REQUIRE(test.controller.next_tick().has_value());
    test.controller.tick(*test.controller.next_tick() - std::chrono::milliseconds { 1 });
    CHECK_FALSE(test.controller.settings().ui.pane_widths.sidebar.has_value());

    test.controller.tick(*test.controller.next_tick());
    CHECK_FALSE(test.controller.next_tick().has_value());
    CHECK(test.controller.settings().ui.pane_widths.sidebar == default_sidebar_width + 40.0f);
    CHECK(test.controller.settings().ui.pane_widths.detail == default_detail_width - 30.0f);

    // 앱을 닫을 때는 기다리지 않고 바로 쓴다.
    REQUIRE(test.send(resize_pane_intent { .sidebar = true, .delta = 8.0f }));
    test.controller.flush();
    CHECK(test.controller.settings().ui.pane_widths.sidebar == default_sidebar_width + 48.0f);
    CHECK_FALSE(test.controller.next_tick().has_value());
}

TEST_CASE("the commit message height starts from what is shown, stays in range and is saved", "[presentation][controller]")
{
    controller_harness test {};
    CHECK_FALSE(test.controller.state().commit_meta_height.has_value());
    // 끈 적이 없으면 지금 보이는 높이에서 이어 끈다.
    REQUIRE(test.send(resize_commit_meta_intent { .delta = 20.0f, .current = 150.0f, .minimum = 64.0f, .maximum = 400.0f }));
    CHECK(test.controller.state().commit_meta_height == 170.0f);
    // 그다음부터는 저장된 높이가 기준이다 (같은 tree에서 나온 메시지가 여럿이어도 끈 만큼 다 더해진다).
    REQUIRE(test.send(resize_commit_meta_intent { .delta = 20.0f, .current = 150.0f, .minimum = 64.0f, .maximum = 400.0f }));
    CHECK(test.controller.state().commit_meta_height == 190.0f);
    REQUIRE(test.send(resize_commit_meta_intent { .delta = 10000.0f, .current = 190.0f, .minimum = 64.0f, .maximum = 400.0f }));
    CHECK(test.controller.state().commit_meta_height == 400.0f);
    // 작은 창에서는 범위 밖의 저장값을 먼저 범위로 다듬고 끈다. 끄는 즉시 손잡이가 움직인다.
    REQUIRE(test.send(resize_commit_meta_intent { .delta = -10.0f, .current = 200.0f, .minimum = 64.0f, .maximum = 200.0f }));
    CHECK(test.controller.state().commit_meta_height == 190.0f);
    REQUIRE(test.send(resize_commit_meta_intent { .delta = -10000.0f, .current = 190.0f, .minimum = 64.0f, .maximum = 200.0f }));
    CHECK(test.controller.state().commit_meta_height == 64.0f);

    CHECK_FALSE(test.controller.settings().ui.pane_widths.commit_meta.has_value());
    test.controller.flush();
    CHECK(test.controller.settings().ui.pane_widths.commit_meta == 64.0f);
}

namespace {
    // 정해 둔 설정을 돌려주고 저장한 마지막 값을 기억하는 저장소다.
    class memory_settings_store final : public cyella::app_settings_store
    {
    public:
        [[nodiscard]] cyella::app_settings_load_result load(std::u8string_view) noexcept override
        {
            cyella::app_settings_load_result result {};
            result.settings = initial;
            return result;
        }

        [[nodiscard]] cyella::app_settings_save_result save(std::u8string_view, const cyella::app_settings& settings, std::u8string_view) noexcept override
        {
            ++saves;
            cyella::app_settings_save_result result {};
            // 디스크가 잠깐 막힌 경우다. 앞의 몇 번만 실패한다.
            if (failing_saves > 0)
            {
                --failing_saves;
                return result;
            }
            saved = settings;
            result.succeeded = true;
            return result;
        }

        cyella::app_settings initial {};
        cyella::app_settings saved {};
        int failing_saves { 0 };
        int saves { 0 };
    };
} // namespace

TEST_CASE("saved pane widths are restored and clamped on start", "[presentation][controller]")
{
    using namespace cyella;
    memory_settings_store store {};
    store.initial.ui.pane_widths.sidebar = 300.0f;
    store.initial.ui.pane_widths.detail = 9999.0f;
    store.initial.ui.pane_widths.commit_meta = 210.0f;
    app_settings_service service { store, u8"memory" };
    static_cast<void>(service.load());

    shell_controller::dependencies dependencies {};
    dependencies.settings = &service;
    shell_controller controller { std::move(dependencies) };
    controller.start({});
    CHECK(controller.state().sidebar_width == 300.0f);
    CHECK(controller.state().detail_width == maximum_detail_width);
    // 메시지 칸 높이는 판 높이를 모르는 여기서 다듬지 않는다. 그릴 때 범위로 다듬는다.
    CHECK(controller.state().commit_meta_height == 210.0f);

    REQUIRE(controller.handle(luil::app_message { resize_pane_intent { .sidebar = true, .delta = 10.0f } }));
    controller.flush();
    CHECK(store.saved.ui.pane_widths.sidebar == 310.0f);
    CHECK(store.saved.ui.pane_widths.commit_meta == 210.0f);
}

TEST_CASE("flush writes settings a failed save left behind", "[presentation][controller]")
{
    using namespace cyella;
    memory_settings_store store {};
    app_settings_service service { store, u8"memory" };
    static_cast<void>(service.load());

    shell_controller::dependencies dependencies {};
    dependencies.settings = &service;
    shell_controller controller { std::move(dependencies) };
    controller.start({});

    // 바꾸는 동안의 저장은 모두 실패하고, 닫기 전에는 디스크가 돌아왔다.
    store.failing_saves = 1000;
    REQUIRE(controller.handle(luil::app_message { cycle_theme_intent {} }));
    REQUIRE(service.has_unsaved_changes());
    store.failing_saves = 0;
    const int saves_before { store.saves };

    // 판 폭을 끄는 중이 아니어도 밀린 변경을 닫기 전에 다시 쓴다.
    controller.flush();
    CHECK(store.saves > saves_before);
    CHECK_FALSE(service.has_unsaved_changes());
    CHECK(store.saved.appearance.theme == theme_preference::light);
}

namespace {
    // 저장 횟수를 세는 설정 저장소와 손으로 앞으로 보내는 시계를 단 controller다. 판 폭이 언제 몇 번 쓰이는지 본다.
    struct saving_controller
    {
        saving_controller()
            : service { store, u8"memory" }
            , controller { prepare(service, now) }
        {
            controller.start({});
        }

        static cyella::shell_controller::dependencies prepare(cyella::app_settings_service& service, std::chrono::steady_clock::time_point& now)
        {
            static_cast<void>(service.load());
            cyella::shell_controller::dependencies dependencies {};
            dependencies.settings = &service;
            dependencies.steady_clock = [&now] { return now; };
            return dependencies;
        }

        template<typename intent_type>
        void send(intent_type intent)
        {
            REQUIRE(controller.handle(luil::app_message { std::move(intent) }));
        }

        // 기다릴 시각이 왔으면 driver처럼 그 시각에 tick한다.
        void tick_due()
        {
            const std::optional<std::chrono::steady_clock::time_point> due { controller.next_tick() };
            if (due.has_value() == false)
                return;
            now = std::max(now, *due);
            controller.tick(now);
        }

        memory_settings_store store {};
        cyella::app_settings_service service;
        std::chrono::steady_clock::time_point now { std::chrono::hours { 1 } };
        cyella::shell_controller controller;
    };
} // namespace

TEST_CASE("releasing a pane handle saves the final widths at once and only once", "[presentation][controller][resize]")
{
    using namespace cyella;
    saving_controller test {};
    const int start { test.store.saves };

    test.send(resize_pane_intent { .sidebar = true, .delta = 12.0f });
    test.now += std::chrono::milliseconds { 16 };
    test.send(resize_pane_intent { .sidebar = true, .delta = 8.0f });
    CHECK(test.store.saves == start);
    test.now += std::chrono::milliseconds { 16 };
    test.send(finish_pane_resize_intent {});
    // 놓는 순간 기다리지 않고 쓴다. 남은 예약이 없어 뒤에 한 번 더 쓰지 않는다.
    CHECK(test.store.saves == start + 1);
    CHECK(test.store.saved.ui.pane_widths.sidebar == default_sidebar_width + 20.0f);
    CHECK_FALSE(test.controller.next_tick().has_value());
    test.now += std::chrono::seconds { 1 };
    test.controller.tick(test.now);
    test.controller.flush();
    CHECK(test.store.saves == start + 1);

    // 끄다 쉬면 그 사이 예약이 중간 폭을 쓰지만, 놓을 때 끝난 폭을 바로 한 번 더 쓴다.
    test.now += std::chrono::seconds { 2 };
    test.send(resize_pane_intent { .sidebar = true, .delta = 10.0f });
    test.now += std::chrono::milliseconds { 600 };
    test.controller.tick(test.now);
    CHECK(test.store.saves == start + 2);
    test.send(resize_pane_intent { .sidebar = true, .delta = 5.0f });
    test.now += std::chrono::milliseconds { 16 };
    test.send(finish_pane_resize_intent {});
    CHECK(test.store.saves == start + 3);
    CHECK(test.store.saved.ui.pane_widths.sidebar == default_sidebar_width + 35.0f);
    CHECK_FALSE(test.controller.next_tick().has_value());

    // 메시지 칸 높이의 손잡이도 같다.
    test.now += std::chrono::seconds { 2 };
    test.send(resize_commit_meta_intent { .delta = 20.0f, .current = 150.0f, .minimum = 64.0f, .maximum = 400.0f });
    test.send(finish_pane_resize_intent {});
    CHECK(test.store.saves == start + 4);
    CHECK(test.store.saved.ui.pane_widths.commit_meta == 170.0f);
    CHECK_FALSE(test.controller.next_tick().has_value());
}

TEST_CASE("holding an arrow key on a pane handle does not write once per repeat", "[presentation][controller][resize]")
{
    using namespace cyella;
    saving_controller test {};
    const int start { test.store.saves };

    // 키를 눌러 두면 되풀이마다 바꾸기와 끝 알림이 짝으로 온다 (luil `split_handle_config::finish`).
    for (int repeat { 0 }; repeat < 30; ++repeat)
    {
        test.send(resize_pane_intent { .sidebar = false, .delta = 4.0f });
        test.send(finish_pane_resize_intent {});
        test.controller.tick(test.now);
        // 처음 한 번은 바로 쓰고, 이어지는 되풀이는 모은다.
        CHECK(test.store.saves == start + 1);
        test.now += std::chrono::milliseconds { 33 };
    }
    // 키를 뗀 뒤 조용해지면 끝난 폭을 한 번 쓴다.
    test.tick_due();
    CHECK(test.store.saves == start + 2);
    CHECK(test.store.saved.ui.pane_widths.detail == default_detail_width + 120.0f);
    CHECK_FALSE(test.controller.next_tick().has_value());
}

TEST_CASE("a held arrow key writes once more when the first repeat waits out the burst window", "[presentation][controller][resize]")
{
    using namespace cyella;
    // OS는 첫 누름과 첫 되풀이 사이를 사용자 설정만큼(Windows 기본 약 500 ms, 최대 약 1 s) 띄운다.
    // 그 틈이 500 ms를 넘기면 첫 되풀이도 앞 알림과 묶이지 않아 바로 쓴다. 쓰기 수는 틈에 따라 둘 또는 셋이다.
    const auto writes_for = [](const std::chrono::milliseconds initial_delay) {
        saving_controller test {};
        const int start { test.store.saves };
        test.send(resize_pane_intent { .sidebar = false, .delta = 4.0f });
        test.send(finish_pane_resize_intent {});
        test.now += initial_delay;
        for (int repeat { 0 }; repeat < 20; ++repeat)
        {
            test.send(resize_pane_intent { .sidebar = false, .delta = 4.0f });
            test.send(finish_pane_resize_intent {});
            test.controller.tick(test.now);
            test.now += std::chrono::milliseconds { 33 };
        }
        test.tick_due();
        CHECK(test.store.saved.ui.pane_widths.detail == default_detail_width + 84.0f);
        CHECK_FALSE(test.controller.next_tick().has_value());
        return test.store.saves - start;
    };
    // 첫 누름, 뗀 뒤의 예약.
    CHECK(writes_for(std::chrono::milliseconds { 250 }) == 2);
    // 첫 누름, 첫 되풀이, 뗀 뒤의 예약.
    CHECK(writes_for(std::chrono::milliseconds { 500 }) == 3);
    CHECK(writes_for(std::chrono::milliseconds { 1000 }) == 3);
}

TEST_CASE("a second drag released soon after the first leaves the write to the schedule", "[presentation][controller][resize]")
{
    using namespace cyella;
    saving_controller test {};
    const int start { test.store.saves };

    test.send(resize_pane_intent { .sidebar = true, .delta = 10.0f });
    test.send(finish_pane_resize_intent {});
    CHECK(test.store.saves == start + 1);

    // 끝 알림은 키와 끌기를 가리지 않는다. 앞 알림에서 500 ms 안에 놓은 끌기도 바로 쓰지 않는다.
    test.now += std::chrono::milliseconds { 300 };
    test.send(resize_pane_intent { .sidebar = true, .delta = 6.0f });
    test.now += std::chrono::milliseconds { 150 };
    const std::chrono::steady_clock::time_point last_resize { test.now };
    test.send(resize_pane_intent { .sidebar = true, .delta = 4.0f });
    test.send(finish_pane_resize_intent {});
    CHECK(test.store.saves == start + 1);

    // 마지막 바꾸기에서 500 ms 뒤의 예약이 끝난 폭을 한 번 쓴다.
    REQUIRE(test.controller.next_tick().has_value());
    CHECK(*test.controller.next_tick() == last_resize + std::chrono::milliseconds { 500 });
    test.tick_due();
    CHECK(test.store.saves == start + 2);
    CHECK(test.store.saved.ui.pane_widths.sidebar == default_sidebar_width + 20.0f);
    CHECK_FALSE(test.controller.next_tick().has_value());
}

TEST_CASE("pane widths are still saved when the release never arrives", "[presentation][controller][resize]")
{
    using namespace cyella;
    saving_controller test {};
    const int start { test.store.saves };

    // 끄는 도중 손잡이가 tree에서 사라지면 luil은 끝을 알리지 않는다. 예약한 시각이 대신 쓴다.
    test.send(resize_pane_intent { .sidebar = true, .delta = 30.0f });
    REQUIRE(test.controller.next_tick().has_value());
    test.controller.tick(*test.controller.next_tick() - std::chrono::milliseconds { 1 });
    CHECK(test.store.saves == start);
    test.tick_due();
    CHECK(test.store.saves == start + 1);
    CHECK(test.store.saved.ui.pane_widths.sidebar == default_sidebar_width + 30.0f);
    CHECK_FALSE(test.controller.next_tick().has_value());
}

TEST_CASE("a pane release that changed nothing writes nothing", "[presentation][controller][resize]")
{
    using namespace cyella;
    saving_controller test {};
    const int start { test.store.saves };

    // 움직이지 않고 뗀 누름에도 끝 알림이 온다.
    test.send(finish_pane_resize_intent {});
    CHECK(test.store.saves == start);
    CHECK_FALSE(test.controller.next_tick().has_value());

    // 이미 끝까지 넓힌 판을 더 끌어도 폭이 그대로면 쓸 것이 없다.
    test.send(resize_pane_intent { .sidebar = true, .delta = 10000.0f });
    test.send(finish_pane_resize_intent {});
    CHECK(test.store.saves == start + 1);
    test.now += std::chrono::seconds { 2 };
    test.send(resize_pane_intent { .sidebar = true, .delta = 10.0f });
    CHECK_FALSE(test.controller.next_tick().has_value());
    test.send(finish_pane_resize_intent {});
    CHECK(test.store.saves == start + 1);

    // 쉬는 사이 예약이 이미 쓴 폭은 움직임 없이 놓아도 다시 쓰지 않는다.
    test.now += std::chrono::seconds { 2 };
    test.send(resize_pane_intent { .sidebar = true, .delta = -20.0f });
    test.tick_due();
    CHECK(test.store.saves == start + 2);
    test.send(finish_pane_resize_intent {});
    CHECK(test.store.saves == start + 2);
}

TEST_CASE("the open diff of the selected tab is the webview's wanted target", "[presentation][controller][diff]")
{
    controller_harness test {};
    CHECK_FALSE(test.controller.current_diff_target().has_value());

    test.controller.open_repository(u8"C:\\work\\repo");
    CHECK_FALSE(test.controller.current_diff_target().has_value());

    const diff_file_request request { .path = "src/a.cpp", .view = diff_view::staged };
    REQUIRE(test.send(select_file_intent { request }));
    const std::optional<diff_target> target { test.controller.current_diff_target() };
    REQUIRE(target.has_value());
    CHECK(target->repository_id == test.controller.selected_repository_id());
    CHECK(target->repository_path == u8"C:\\work\\repo");
    CHECK(target->request == request);

    // 다른 탭(시작 탭)으로 가면 그 탭에는 열린 diff가 없다. 돌아오면 다시 있다.
    REQUIRE(test.send(new_tab_intent {}));
    CHECK_FALSE(test.controller.current_diff_target().has_value());

    REQUIRE(test.send(close_diff_intent {}));
    CHECK_FALSE(test.controller.current_diff_target().has_value());
}

TEST_CASE("the diff surface is stored for the header bar", "[presentation][controller][diff]")
{
    controller_harness test {};
    diff_surface_state surface {};
    surface.webview_available = true;
    surface.layout = diff_layout::split;
    surface.content = diff_content::image;
    test.controller.set_diff_surface(surface);
    CHECK(test.controller.state().diff_surface == surface);
}

TEST_CASE("changing repository layout preserves the draft and exact diff request without submitting Git work", "[presentation][controller][layout]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:/work/repo");
    REQUIRE(test.send(select_workspace_intent { true }));
    luil::text_edit_request insert {};
    insert.target = to_luil_target(text_target::commit_subject);
    insert.command = luil::text::text_edit_command::insert;
    insert.text = u8"배치와 무관한 초안";
    REQUIRE(test.send(edit_text_intent { insert }));
    const diff_file_request request { .path = "src/a.cpp", .view = diff_view::staged };
    REQUIRE(test.send(select_file_intent { request }));
    const auto before { test.controller.current_diff_target() };
    const auto pending { test.dispatcher.pending() };
    const auto tab { test.controller.tabs().selected().id };
    for (const bool graph : { false, true, false })
    {
        REQUIRE(test.send(set_repository_layout_intent { graph }));
        CHECK(test.controller.settings().ui.graph_layout == graph);
        REQUIRE(test.controller.current_diff_target().has_value());
        CHECK(test.controller.current_diff_target()->request == before->request);
        CHECK(test.controller.state().tabs.at(tab).commit_subject.text == insert.text);
        CHECK(test.dispatcher.pending() == pending);
        CHECK_FALSE(test.controller.state().drawer_open);
    }
}

TEST_CASE("a run without git keeps the saved open tabs when tabs are touched", "[presentation][controller][tabs]")
{
    using namespace cyella;
    memory_settings_store store {};
    store.initial.open_tabs = { u8"C:\\work\\one", u8"C:\\work\\two" };
    store.initial.active_tab = u8"C:\\work\\two";
    app_settings_service service { store, u8"memory" };
    static_cast<void>(service.load());

    // git을 찾지 못한 실행이다 (workspace가 없다). 저장된 탭은 되살리지 않지만 지우지도 않는다.
    shell_controller::dependencies dependencies {};
    dependencies.settings = &service;
    shell_controller controller { std::move(dependencies) };
    controller.start({});
    REQUIRE(controller.handle(luil::app_message { new_tab_intent {} }));
    REQUIRE(controller.handle(luil::app_message { cycle_tab_intent { true } }));
    CHECK(controller.settings().open_tabs == std::vector<std::u8string> { u8"C:\\work\\one", u8"C:\\work\\two" });
    CHECK(controller.settings().active_tab == u8"C:\\work\\two");
}

TEST_CASE("a bell press right after an outside close uses the injected clock", "[presentation][controller][notifications]")
{
    controller_harness test {};
    REQUIRE(test.send(toggle_notification_center_intent {}));
    REQUIRE(test.send(close_notification_center_intent { .dismissed = true }));
    // 같은 시각의 종 누름은 방금 닫은 그 누름이다.
    REQUIRE(test.send(toggle_notification_center_intent {}));
    CHECK_FALSE(test.controller.state().notifications_open);
    // 주입한 시계로 창을 넘기면 새 누름이다 (실제 시계는 test 동안 거의 흐르지 않는다).
    REQUIRE(test.send(toggle_notification_center_intent {}));
    REQUIRE(test.send(close_notification_center_intent { .dismissed = true }));
    test.advance(std::chrono::seconds { 1 });
    REQUIRE(test.send(toggle_notification_center_intent {}));
    CHECK(test.controller.state().notifications_open);
}

TEST_CASE("Enter in the clone fields does not submit the same clone again while it runs", "[presentation][controller][clone]")
{
    controller_harness test {};
    REQUIRE(test.send(open_clone_dialog_intent {}));
    for (const auto& [target, text] : { std::pair { text_target::clone_url, u8"https://example.com/r.git" }, std::pair { text_target::clone_path, u8"C:\\work\\r" } })
    {
        luil::text_edit_request insert {};
        insert.target = to_luil_target(target);
        insert.command = luil::text::text_edit_command::insert;
        insert.text = text;
        REQUIRE(test.send(edit_text_intent { insert }));
    }
    REQUIRE(test.send(submit_clone_intent {}));
    REQUIRE(test.controller.state().clone.has_value());
    const operation_id first { test.controller.state().clone->operation };
    REQUIRE(first != 0);
    REQUIRE(test.send(submit_clone_intent {}));
    CHECK(count_operations(test.workspace, operation_kind::clone) == 1);
    CHECK(test.controller.state().clone->operation == first);
}

TEST_CASE("the clone folder name drops queries and refuses names that leave the chosen folder", "[presentation][controller][clone]")
{
    controller_harness test {};
    REQUIRE(test.send(open_clone_dialog_intent {}));
    const auto chosen_path = [&test](const std::u8string& url) {
        luil::text_edit_request clear {};
        clear.target = to_luil_target(text_target::clone_url);
        clear.command = luil::text::text_edit_command::clear;
        REQUIRE(test.send(edit_text_intent { clear }));
        luil::text_edit_request insert {};
        insert.target = to_luil_target(text_target::clone_url);
        insert.command = luil::text::text_edit_command::insert;
        insert.text = url;
        REQUIRE(test.send(edit_text_intent { insert }));
        REQUIRE(test.send(folder_chosen_intent { .purpose = folder_purpose::clone_target, .path = u8"C:\\work", .origin = {} }));
        return test.controller.state().clone->path.text;
    };
    CHECK(chosen_path(u8"git@example.com:a/b.git") == u8"C:\\work\\b");
    CHECK(chosen_path(u8"https://example.com/r.git?x=1#top") == u8"C:\\work\\r");
    CHECK(chosen_path(u8"https://example.com/..") == u8"C:\\work");
    CHECK(chosen_path(u8"https://example.com/a/.") == u8"C:\\work");
    CHECK(chosen_path(u8"https://example.com/a|b") == u8"C:\\work");
}

TEST_CASE("closing a tab cancels every queued operation even when trimming the history shifts the order", "[presentation][controller][tabs]")
{
    test::fake_git_runner process {};
    test::fake_workspace_files files {};
    test::manual_dispatcher dispatcher {};
    test::event_queue events {};
    git_operation_runner runner { process, u8"C:/Git/cmd/git.exe", files, {} };
    git_workspace_options options {};
    options.finished_history_limit = 2;
    git_workspace workspace { runner, dispatcher.dispatcher(), events.sink(), nullptr, options };
    shell_controller::dependencies dependencies {};
    dependencies.workspace = &workspace;
    shell_controller controller { std::move(dependencies) };
    controller.start({});
    controller.open_repository(u8"C:\\work\\repo");
    const std::string repository { controller.selected_repository_id() };

    // 앞에는 지워지지 않는 결정 대기(needs_input)가 서고, 끝난 작업이 상한까지 찬다. 대기 작업을 취소해 끝내면 그 사이의 끝난 작업이 지워진다.
    const operation_id waiting { workspace.submit(repository, checkout_branch_request { "topic" }) };
    workspace.apply(operation_event { waiting, repository, operation_started {} });
    input_request needed {};
    needed.kind = input_kind::dirty_worktree;
    needed.target = "topic";
    workspace.apply(operation_event { waiting, repository, operation_finished { operation_state::needs_input, std::nullopt, needed, {} } });
    const operation_id done { workspace.submit(repository, stash_push_request {}) };
    workspace.apply(operation_event { done, repository, operation_started {} });
    workspace.apply(operation_event { done, repository, operation_finished { operation_state::succeeded, std::nullopt, std::nullopt, {} } });
    std::vector<operation_id> queued {};
    for (int index { 0 }; index < 3; ++index)
        queued.push_back(workspace.submit(repository, stash_apply_request { 0 }));

    REQUIRE(controller.handle(luil::app_message { close_tab_intent { controller.tabs().selected().id } }));
    for (const operation_id id : queued)
    {
        const operation_status* const status { workspace.find_operation(id) };
        CHECK((status == nullptr || status->state == operation_state::cancelled));
    }
    CHECK(workspace.repository_ids().empty());
}

TEST_CASE("switching tabs closes a confirmation opened on another repository", "[presentation][controller][tabs][dialogs]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\one");
    REQUIRE(test.send(new_tab_intent {}));
    test.controller.open_repository(u8"C:\\work\\two");
    REQUIRE(test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::local_branch, .name = "topic" }, 0.0f, 0.0f }));
    REQUIRE(test.send(menu_select_intent { u8"delete" }));
    REQUIRE(test.controller.state().dialog.has_value());
    CHECK(test.controller.state().dialog->repository_id == test.controller.selected_repository_id());

    // 다른 탭으로 가면 상자가 닫힌다. 남겨 두면 [확인]이 그 탭의 저장소에 같은 이름의 브랜치 삭제를 낸다.
    const std::size_t before { count_operations(test.workspace, operation_kind::delete_branch) };
    REQUIRE(test.send(cycle_tab_intent { true }));
    CHECK_FALSE(test.controller.state().dialog.has_value());
    REQUIRE(test.send(form_button_intent { u8"submit" }));
    CHECK(count_operations(test.workspace, operation_kind::delete_branch) == before);
}

TEST_CASE("a dialog field keeps Enter and the palette closes with its own shortcut", "[presentation][controller][policy]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    ui::cyella_policy policy {};
    luil::interaction_snapshot snapshot {};
    const auto ctrl = [](const luil::key_code key, const bool shift = false) {
        luil::key_pressed_event event {};
        event.key = key;
        event.control = true;
        event.shift = shift;
        return event;
    };
    const auto command_of = [&](const luil::key_pressed_event& event, const luil::interaction_snapshot& state) {
        const std::shared_ptr<const luil::ui_tree> tree { ui::build_ui_tree(test.controller.make_view(1440.0f, 900.0f, 1.0f)).tree };
        const std::vector<luil::input_action> actions { policy.on_key(tree.get(), event, state) };
        if (actions.size() != 1 || std::holds_alternative<luil::app_message>(actions[0]) == false)
            return std::string {};
        const run_command_intent* const run { std::get<luil::app_message>(actions[0]).get<run_command_intent>() };
        return run == nullptr ? std::string {} : run->id;
    };
    CHECK(command_of(ctrl(luil::key_code::tab), snapshot) == "tab.next");

    // 확인 상자 칸의 Enter는 상자의 것이다.
    REQUIRE(test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::local_branch, .name = "topic" }, 0.0f, 0.0f }));
    REQUIRE(test.send(menu_select_intent { u8"delete" }));
    REQUIRE(test.controller.state().dialog.has_value());
    luil::interaction_snapshot in_field {};
    in_field.focused_input = luil::ui_element_id { ui::kind_form_input_0, u8"confirm-dialog-field-0" };
    luil::key_pressed_event enter {};
    enter.key = luil::key_code::enter;
    const std::shared_ptr<const luil::ui_tree> tree { ui::build_ui_tree(test.controller.make_view(1440.0f, 900.0f, 1.0f)).tree };
    const std::vector<luil::input_action> entered { policy.on_key(tree.get(), enter, in_field) };
    REQUIRE(entered.size() == 1);
    REQUIRE(std::holds_alternative<luil::app_message>(entered[0]));
    CHECK(std::get<luil::app_message>(entered[0]).get<form_enter_intent>() != nullptr);
    REQUIRE(test.send(form_button_intent { u8"cancel" }));

    // 명령 팔레트는 같은 단축키로 닫는다.
    REQUIRE(test.send(run_command_intent { "palette.open" }));
    REQUIRE(test.controller.state().palette.has_value());
    CHECK(command_of(ctrl(luil::key_code::key_p, true), snapshot) == "palette.open");
}

namespace {
    // 저장소를 열고 이력을 채운 뒤 커밋 3의 파일 하나를 판에 연다.
    repository_session& open_commit_file(controller_harness& test, const std::string& path = "a.txt")
    {
        test.controller.open_repository(u8"C:\\work\\repo");
        repository_session* const session { test.workspace.find_session(test.controller.selected_repository_id()) };
        REQUIRE(session != nullptr);
        load_history(*session, branching_history());
        static_cast<void>(test.controller.make_view(1400.0f, 900.0f, 1.0f));
        REQUIRE(test.send(select_commit_intent { graph_item_key(sha_of(3)) }));
        REQUIRE(test.send(select_file_intent { diff_file_request { .path = path, .view = diff_view::commit, .sha = sha_of(3) } }));
        return *session;
    }

    const tab_ui_state& tab_of(const controller_harness& test)
    {
        return test.controller.state().tabs.at(test.controller.tabs().selected().id);
    }
} // namespace

TEST_CASE("the blame view reads once, follows the selected file and closes with the panel", "[presentation][controller][file_view]")
{
    controller_harness test {};
    static_cast<void>(open_commit_file(test));
    CHECK_FALSE(test.controller.current_blame_target().has_value());

    REQUIRE(test.send(set_file_view_mode_intent { file_view_mode::blame }));
    REQUIRE(tab_of(test).file_view.has_value());
    CHECK(tab_of(test).file_view->mode == file_view_mode::blame);
    // 커밋의 파일은 그 커밋의 판을 blame한다.
    CHECK(tab_of(test).file_view->blame == blame_file_request { .path = "a.txt", .revision = sha_of(3) });
    CHECK(count_operations(test.workspace, operation_kind::blame_file) == 1);
    REQUIRE(test.controller.current_blame_target().has_value());
    CHECK(test.controller.current_blame_target()->request.path == "a.txt");
    // 웹뷰는 blame을 보이므로 diff 대상은 없다.
    CHECK_FALSE(test.controller.current_diff_target().has_value());

    // 같은 보기를 다시 눌러도 다시 읽지 않는다.
    REQUIRE(test.send(set_file_view_mode_intent { file_view_mode::blame }));
    CHECK(count_operations(test.workspace, operation_kind::blame_file) == 1);

    // 다른 파일을 고르면 같은 보기로 그 파일을 연다.
    REQUIRE(test.send(select_file_intent { diff_file_request { .path = "b.txt", .view = diff_view::commit, .sha = sha_of(3) } }));
    REQUIRE(tab_of(test).file_view.has_value());
    CHECK(tab_of(test).file_view->mode == file_view_mode::blame);
    CHECK(tab_of(test).file_view->blame.path == "b.txt");
    CHECK(count_operations(test.workspace, operation_kind::blame_file) == 2);

    // 변경 내용으로 돌아가면 보기를 거두고 diff 대상이 돌아온다.
    REQUIRE(test.send(set_file_view_mode_intent { file_view_mode::changes }));
    CHECK_FALSE(tab_of(test).file_view.has_value());
    REQUIRE(test.controller.current_diff_target().has_value());
    CHECK(test.controller.current_diff_target()->request.path == "b.txt");

    // 판을 닫으면 보기도 함께 닫히고 끝나지 않은 blame 읽기는 취소된다.
    REQUIRE(test.send(set_file_view_mode_intent { file_view_mode::blame }));
    const operation_id pending { tab_of(test).file_view->blame_operation };
    REQUIRE(test.send(close_diff_intent {}));
    CHECK_FALSE(tab_of(test).file_view.has_value());
    CHECK_FALSE(test.controller.current_blame_target().has_value());
    REQUIRE(test.workspace.find_operation(pending) != nullptr);
    CHECK((test.workspace.find_operation(pending)->finished() || test.workspace.find_operation(pending)->cancel_requested));
}

TEST_CASE("the blame of a file deleted by the commit reads the revision before it", "[presentation][controller][file_view]")
{
    controller_harness test {};
    repository_session& session { open_commit_file(test, "gone.txt") };
    // 커밋 상세가 그 파일을 지웠다고 말한다.
    commit_detail detail {};
    detail.commit = commit_info { .sha = sha_of(3), .parents = { sha_of(2) }, .subject = "three" };
    detail.files.push_back(file_change { .path = "gone.txt", .status = 'D' });
    bool applied { false };
    for (const operation_id id : test.workspace.operation_order())
        if (test.workspace.find_operation(id)->kind == operation_kind::show_commit && session.apply(id, commit_detail_loaded { detail }))
            applied = true;
    REQUIRE(applied);

    REQUIRE(test.send(set_file_view_mode_intent { file_view_mode::blame }));
    REQUIRE(tab_of(test).file_view.has_value());
    CHECK(tab_of(test).file_view->blame.revision == sha_of(3) + "^");
    // `<sha>^`는 그래프의 한 커밋이 아니라 "그래프에서 보기" 단추가 없다.
    const view_snapshot view { test.controller.make_view(1400.0f, 900.0f, 1.0f) };
    REQUIRE(view.repository.has_value());
    REQUIRE(view.repository->diff.has_value());
    CHECK(view.repository->diff->mode == file_view_mode::blame);
    CHECK(view.repository->diff->reveal_sha.empty());
    CHECK(view.repository->diff->loading);
}

TEST_CASE("the file history picks its newest commit and shows each commit's change at the path it had then", "[presentation][controller][file_view]")
{
    controller_harness test {};
    repository_session& session { open_commit_file(test) };

    REQUIRE(test.send(set_file_view_mode_intent { file_view_mode::history }));
    REQUIRE(tab_of(test).file_view.has_value());
    const operation_id operation { tab_of(test).file_view->history_operation };
    REQUIRE(operation != 0);
    CHECK(tab_of(test).file_view->history == file_history_request { .path = "a.txt", .revision = sha_of(3) });
    // 이력이 오기 전에는 고른 커밋이 없어 웹뷰가 보일 것이 없다.
    CHECK_FALSE(test.controller.current_diff_target().has_value());

    file_history history {};
    history.request = tab_of(test).file_view->history;
    history.entries.push_back(file_history_entry { .commit = { .sha = sha_of(3), .parents = { sha_of(2) }, .author_name = "Morgan", .subject = "three" }, .path = "a.txt", .previous_path = {}, .status = 'M' });
    history.entries.push_back(file_history_entry { .commit = { .sha = sha_of(2), .parents = { sha_of(1) }, .author_name = "Robin", .subject = "two" }, .path = "a.txt", .previous_path = "old.txt", .status = 'R' });
    history.entries.push_back(file_history_entry { .commit = { .sha = sha_of(1), .author_name = "Morgan", .subject = "one" }, .path = "old.txt", .previous_path = {}, .status = 'A' });
    REQUIRE(session.apply(operation, file_history_loaded { history }));
    test.controller.after_events();

    // 가장 새 커밋을 골라 둔다.
    CHECK(tab_of(test).file_view->history_selected == sha_of(3));
    REQUIRE(test.controller.current_diff_target().has_value());
    CHECK(test.controller.current_diff_target()->request.sha == sha_of(3));
    CHECK(test.controller.current_diff_target()->request.path == "a.txt");

    // 이름이 바뀌기 전의 커밋은 그때의 경로로 읽는다.
    REQUIRE(test.send(select_file_history_intent { graph_item_key(sha_of(1)) }));
    REQUIRE(test.controller.current_diff_target().has_value());
    CHECK(test.controller.current_diff_target()->request.path == "old.txt");
    CHECK(test.controller.current_diff_target()->request.view == diff_view::commit);

    const view_snapshot view { test.controller.make_view(1400.0f, 900.0f, 1.0f) };
    REQUIRE(view.repository.has_value());
    REQUIRE(view.repository->diff.has_value());
    const diff_panel_view& panel { *view.repository->diff };
    CHECK(panel.mode == file_view_mode::history);
    REQUIRE(panel.history.has_value());
    REQUIRE(panel.history->rows.size() == 3);
    CHECK(panel.history->selected == sha_of(1));
    CHECK(panel.history->rows[1].status == 'R');
    CHECK(panel.history->rows[1].rename_note == u8"old.txt → a.txt");
    CHECK(panel.history->rows[0].rename_note.empty());
    CHECK(panel.reveal_sha == sha_of(1));
    // 머리의 파일 위치는 판을 연 파일을 따른다.
    CHECK(panel.request.path == "a.txt");
    CHECK(panel.request.sha == sha_of(3));

    // 이력에서 Blame으로 넘어가면 고른 커밋의 줄을 강조해 연다.
    REQUIRE(test.send(set_file_view_mode_intent { file_view_mode::blame }));
    REQUIRE(test.controller.current_blame_target().has_value());
    CHECK(test.controller.current_blame_target()->focus == sha_of(1));

    // 변경 내용으로 돌아오면 판의 diff를 다시 읽는다 (세션의 마지막 diff는 이력에서 고른 커밋의 것이다).
    const std::size_t diffs { count_operations(test.workspace, operation_kind::diff_file) };
    REQUIRE(test.send(set_file_view_mode_intent { file_view_mode::changes }));
    CHECK(count_operations(test.workspace, operation_kind::diff_file) == diffs + 1);
    REQUIRE(test.controller.current_diff_target().has_value());
    CHECK(test.controller.current_diff_target()->request.sha == sha_of(3));
}

TEST_CASE("a failed blame says why in the panel and an untracked file has no blame", "[presentation][controller][file_view]")
{
    controller_harness test {};
    static_cast<void>(open_commit_file(test));

    // 추적하지 않는 파일은 보기를 열지 않고 까닭을 알린다.
    REQUIRE(test.send(select_file_intent { diff_file_request { .path = "new.txt", .view = diff_view::unstaged, .untracked = true } }));
    const std::size_t notices { test.controller.state().notifications.size() };
    REQUIRE(test.send(set_file_view_mode_intent { file_view_mode::blame }));
    CHECK_FALSE(tab_of(test).file_view.has_value());
    CHECK(test.controller.state().notifications.size() == notices + 1);
    CHECK(count_operations(test.workspace, operation_kind::blame_file) == 0);
}

TEST_CASE("show in graph selects a loaded commit and keeps reading history for one that is not loaded yet", "[presentation][controller][file_view]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    repository_session* const session { test.workspace.find_session(test.controller.selected_repository_id()) };
    REQUIRE(session != nullptr);
    // 첫 쪽에는 커밋 5와 4만 있다. 이력은 더 있다.
    const std::uint64_t generation { session->begin_refresh() };
    repository_snapshot snapshot {};
    snapshot.summary.head_sha = sha_of(5);
    REQUIRE(session->apply(snapshot_loaded { generation, snapshot }));
    std::vector<commit_info> commits { branching_history() };
    std::vector<commit_info> first_page { commits.begin(), commits.begin() + 2 };
    std::vector<commit_info> second_page { commits.begin() + 2, commits.end() };
    REQUIRE(session->apply(commits_loaded { .generation = generation, .commits = first_page, .page_complete = true, .history_complete = false }));
    static_cast<void>(test.controller.make_view(1400.0f, 900.0f, 1.0f));

    REQUIRE(test.send(select_commit_intent { graph_item_key(sha_of(5)) }));
    REQUIRE(test.send(select_file_intent { diff_file_request { .path = "a.txt", .view = diff_view::commit, .sha = sha_of(5) } }));
    REQUIRE(test.send(set_file_view_mode_intent { file_view_mode::blame }));

    // 읽은 커밋이면 곧바로 고르고 판을 닫는다.
    REQUIRE(test.send(reveal_commit_intent { .repository_id = {}, .sha = sha_of(4) }));
    CHECK(tab_of(test).selected_sha == sha_of(4));
    CHECK_FALSE(tab_of(test).diff.has_value());
    CHECK_FALSE(tab_of(test).file_view.has_value());
    CHECK(tab_of(test).pending_reveal.empty());

    // 아직 읽지 않은 커밋이면 다음 쪽을 청하고 기다린다.
    REQUIRE(test.send(reveal_commit_intent { .repository_id = {}, .sha = sha_of(1) }));
    CHECK(tab_of(test).pending_reveal == sha_of(1));
    CHECK(tab_of(test).selected_sha == sha_of(4));
    CHECK(session->log_page_loading());

    // 쪽이 오면 그 커밋을 고른다.
    REQUIRE(session->apply(commits_loaded { .generation = session->displayed_generation(), .first_index = 2, .commits = second_page, .page_complete = true, .history_complete = true }));
    test.controller.after_events();
    CHECK(tab_of(test).selected_sha == sha_of(1));
    CHECK(tab_of(test).pending_reveal.empty());

    // 이력 끝까지 없는 커밋은 찾기를 그만두고 알린다.
    const std::size_t notices { test.controller.state().notifications.size() };
    REQUIRE(test.send(reveal_commit_intent { .repository_id = {}, .sha = sha_of(9) }));
    CHECK(tab_of(test).pending_reveal.empty());
    CHECK(tab_of(test).selected_sha == sha_of(1));
    CHECK(test.controller.state().notifications.size() == notices + 1);

    // 다른 저장소의 판이 보낸 요청은 이 탭의 그래프를 건드리지 않는다.
    REQUIRE(test.send(reveal_commit_intent { .repository_id = "another-repository", .sha = sha_of(5) }));
    CHECK(tab_of(test).selected_sha == sha_of(1));
}

TEST_CASE("the file menus open the blame and the history of that file", "[presentation][controller][file_view]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    repository_session* const session { test.workspace.find_session(test.controller.selected_repository_id()) };
    REQUIRE(session != nullptr);
    load_history(*session, branching_history(), true);

    // 작업 영역 파일은 작업 트리를 blame한다 (커밋하지 않은 줄이 보인다).
    const menu_target working { .kind = menu_target_kind::file, .name = "src/a.cpp", .side = change_side::unstaged };
    REQUIRE(test.send(open_context_menu_intent { working, 0.0f, 0.0f }));
    REQUIRE(test.send(menu_select_intent { u8"blame" }));
    REQUIRE(tab_of(test).diff.has_value());
    CHECK(tab_of(test).diff->path == "src/a.cpp");
    CHECK(tab_of(test).diff->view == diff_view::unstaged);
    REQUIRE(tab_of(test).file_view.has_value());
    CHECK(tab_of(test).file_view->mode == file_view_mode::blame);
    CHECK(tab_of(test).file_view->blame == blame_file_request { .path = "src/a.cpp", .revision = {} });

    // 커밋 상세의 파일은 그 커밋부터의 이력이다.
    const menu_target committed { .kind = menu_target_kind::commit_file, .name = "src/b.cpp", .sha = sha_of(2) };
    REQUIRE(test.send(open_context_menu_intent { committed, 0.0f, 0.0f }));
    REQUIRE(test.send(menu_select_intent { u8"file_history" }));
    REQUIRE(tab_of(test).file_view.has_value());
    CHECK(tab_of(test).file_view->mode == file_view_mode::history);
    CHECK(tab_of(test).file_view->history == file_history_request { .path = "src/b.cpp", .revision = sha_of(2) });
}

TEST_CASE("show in graph gives up once when reading more history fails", "[presentation][controller][file_view]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    repository_session* const session { test.workspace.find_session(test.controller.selected_repository_id()) };
    REQUIRE(session != nullptr);
    const std::uint64_t generation { session->begin_refresh() };
    repository_snapshot snapshot {};
    snapshot.summary.head_sha = sha_of(5);
    REQUIRE(session->apply(snapshot_loaded { generation, snapshot }));
    std::vector<commit_info> commits { branching_history() };
    REQUIRE(session->apply(commits_loaded { .generation = generation, .commits = { commits.begin(), commits.begin() + 2 }, .page_complete = true, .history_complete = false }));
    static_cast<void>(test.controller.make_view(1400.0f, 900.0f, 1.0f));

    REQUIRE(test.send(reveal_commit_intent { .repository_id = {}, .sha = sha_of(1) }));
    REQUIRE(tab_of(test).pending_reveal == sha_of(1));
    const operation_id page { tab_of(test).reveal_operation };
    REQUIRE(page != 0);

    // 그 읽기가 끝나지 못하면 찾기를 거두고 한 번 알린다. 메시지마다 같은 읽기를 다시 내지 않는다.
    REQUIRE(test.workspace.cancel(page));
    const std::size_t notices { test.controller.state().notifications.size() };
    test.controller.after_events();
    CHECK(tab_of(test).pending_reveal.empty());
    CHECK(tab_of(test).reveal_operation == 0);
    CHECK(test.controller.state().notifications.size() == notices + 1);
    // 찾기를 거뒀으므로 뒤의 메시지는 이 일로 더 알리지 않는다 (그래프 끝의 자동 이어 읽기는 따로 돈다).
    REQUIRE(test.send(set_file_view_mode_intent { file_view_mode::changes }));
    CHECK(tab_of(test).pending_reveal.empty());
    CHECK(test.controller.state().notifications.size() == notices + 1);

    // 사이드바에서 다른 커밋을 고르면 찾던 커밋이 늦게 와서 선택을 빼앗지 않는다.
    REQUIRE(test.send(reveal_commit_intent { .repository_id = {}, .sha = sha_of(2) }));
    REQUIRE(tab_of(test).pending_reveal == sha_of(2));
    REQUIRE(test.send(select_sidebar_item_intent { .key = u8"l:main", .target_sha = sha_of(5) }));
    CHECK(tab_of(test).pending_reveal.empty());
}

namespace {
    // 검색 칸에 글을 친다 (지금 글 끝에 덧붙인다).
    void type_search(controller_harness& test, const std::u8string& text)
    {
        luil::text_edit_request insert {};
        insert.target = to_luil_target(text_target::graph_search);
        insert.command = luil::text::text_edit_command::insert;
        insert.text = text;
        REQUIRE(test.send(edit_text_intent { insert }));
    }

    // 한글 제목이 섞인 이력이다 (자식이 앞).
    std::vector<commit_info> searchable_history()
    {
        return {
            { .sha = sha_of(5), .parents = { sha_of(4) }, .author_name = "Morgan", .author_email = "morgan@example.com", .subject = "fix: 인사말을 고친다" },
            { .sha = sha_of(4), .parents = { sha_of(3) }, .author_name = "Robin Lee", .author_email = "robin@example.com", .subject = "docs: 설명을 더한다" },
            { .sha = sha_of(3), .parents = { sha_of(2) }, .author_name = "Morgan", .author_email = "morgan@example.com", .subject = "feat: 인사말을 더한다" },
            { .sha = sha_of(2), .parents = { sha_of(1) }, .author_name = "Robin Lee", .author_email = "robin@example.com", .subject = "chore: Cleanup" },
            { .sha = sha_of(1), .author_name = "Morgan", .author_email = "morgan@example.com", .subject = "first" },
        };
    }

    const graph_search_view& search_view_of(const view_snapshot& view)
    {
        REQUIRE(view.repository.has_value());
        REQUIRE(view.repository->graph.search.has_value());
        return *view.repository->graph.search;
    }
} // namespace

TEST_CASE("the graph search opens over the graph, takes focus and closes with Esc after the file panel", "[presentation][controller][search]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    repository_session* const session { test.workspace.find_session(test.controller.selected_repository_id()) };
    REQUIRE(session != nullptr);
    load_history(*session, searchable_history());

    CHECK_FALSE(test.controller.make_view(1400.0f, 900.0f, 1.0f).repository->graph.search.has_value());
    REQUIRE(test.send(run_command_intent { "graph.search" }));
    REQUIRE(tab_of(test).search.has_value());
    // 열면 칸이 초점을 받는다. 다시 열면(Ctrl+F를 또 누르면) 새 요청으로 초점을 돌린다.
    REQUIRE(test.controller.state().popup_focus.has_value());
    CHECK(test.controller.state().popup_focus->target == popup_focus_target::graph_search);
    const std::uint64_t first_focus { test.controller.state().popup_focus->id };
    REQUIRE(test.send(open_graph_search_intent {}));
    CHECK(test.controller.state().popup_focus->id > first_focus);

    const view_snapshot view { test.controller.make_view(1400.0f, 900.0f, 1.0f) };
    CHECK(search_view_of(view).status.empty());
    CHECK_FALSE(search_view_of(view).can_step);
    const ui::built_tree built { ui::build_ui_tree(view) };
    CHECK(built.tree->find({ ui::kind_graph_search_input, u8"graph-search" }) != nullptr);
    REQUIRE(built.focus.has_value());
    CHECK(built.focus->target == luil::ui_element_id { ui::kind_graph_search_input, u8"graph-search" });

    // 파일 판이 그래프를 덮고 있으면 Esc는 판부터 닫고, 그다음에 검색을 닫는다.
    REQUIRE(test.send(select_commit_intent { graph_item_key(sha_of(3)) }));
    REQUIRE(test.send(select_file_intent { diff_file_request { .path = "a.txt", .view = diff_view::commit, .sha = sha_of(3) } }));
    REQUIRE(test.send(close_overlays_intent {}));
    CHECK_FALSE(tab_of(test).diff.has_value());
    CHECK(tab_of(test).search.has_value());
    REQUIRE(test.send(close_overlays_intent {}));
    CHECK_FALSE(tab_of(test).search.has_value());

    // 판이 열린 채 검색을 열면 그래프가 보이게 판을 닫는다.
    REQUIRE(test.send(select_file_intent { diff_file_request { .path = "a.txt", .view = diff_view::commit, .sha = sha_of(3) } }));
    REQUIRE(test.send(open_graph_search_intent {}));
    CHECK_FALSE(tab_of(test).diff.has_value());
    CHECK(tab_of(test).search.has_value());
    REQUIRE(test.send(close_graph_search_intent {}));
    CHECK_FALSE(tab_of(test).search.has_value());
}

TEST_CASE("loaded commits are matched at once by subject, author, sha prefix and initial consonants", "[presentation][controller][search]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    repository_session* const session { test.workspace.find_session(test.controller.selected_repository_id()) };
    REQUIRE(session != nullptr);
    load_history(*session, searchable_history());
    REQUIRE(test.send(open_graph_search_intent {}));

    const auto hits_for = [&test](const std::u8string& query) {
        luil::text_edit_request clear {};
        clear.target = to_luil_target(text_target::graph_search);
        clear.command = luil::text::text_edit_command::clear;
        REQUIRE(test.send(edit_text_intent { clear }));
        type_search(test, query);
        const view_snapshot view { test.controller.make_view(1400.0f, 900.0f, 1.0f) };
        const graph_search_view& search { search_view_of(view) };
        REQUIRE(search.hits != nullptr);
        return std::vector<bool> { search.hits->contains(sha_of(5)), search.hits->contains(sha_of(4)), search.hits->contains(sha_of(3)), search.hits->contains(sha_of(2)), search.hits->contains(sha_of(1)) };
    };
    // 제목의 한글 부분 문자열
    CHECK(hits_for(u8"인사말") == std::vector<bool> { true, false, true, false, false });
    // 초성: ㅇㅅㅁ → 인사말
    CHECK(hits_for(u8"ㅇㅅㅁ") == std::vector<bool> { true, false, true, false, false });
    // 영문은 대소문자를 가리지 않는다
    CHECK(hits_for(u8"cleanup") == std::vector<bool> { false, false, false, true, false });
    // 작성자 이름과 메일
    CHECK(hits_for(u8"robin") == std::vector<bool> { false, true, false, true, false });
    CHECK(hits_for(u8"morgan@") == std::vector<bool> { true, false, true, false, true });
    // SHA 앞부분 (넷 이상의 16진수)
    const std::string prefix { sha_of(4).substr(0, 8) };
    CHECK(hits_for(std::u8string { prefix.begin(), prefix.end() }) == std::vector<bool> { false, true, false, false, false });
    // 건너뛰며 맞는 것은 받지 않는다
    CHECK(hits_for(u8"인말") == std::vector<bool> { false, false, false, false, false });

    const view_snapshot none { test.controller.make_view(1400.0f, 900.0f, 1.0f) };
    CHECK_FALSE(search_view_of(none).can_step);
}

TEST_CASE("git is asked once typing pauses and its hits join the loaded ones, unloaded hits last", "[presentation][controller][search]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    repository_session* const session { test.workspace.find_session(test.controller.selected_repository_id()) };
    REQUIRE(session != nullptr);
    load_history(*session, searchable_history());
    REQUIRE(test.send(open_graph_search_intent {}));

    // 한 글자는 git에 묻지 않는다. 읽은 커밋은 그래도 훑는다.
    type_search(test, u8"인");
    REQUIRE(test.controller.next_tick().has_value());
    test.advance(std::chrono::milliseconds { 300 });
    test.controller.tick(test.steady());
    CHECK(count_operations(test.workspace, operation_kind::search_commits) == 0);

    // 치는 동안에는 내지 않고, 멈춘 뒤 한 번 낸다.
    type_search(test, u8"사");
    type_search(test, u8"말");
    CHECK(count_operations(test.workspace, operation_kind::search_commits) == 0);
    test.controller.tick(test.steady());
    CHECK(count_operations(test.workspace, operation_kind::search_commits) == 0);
    test.advance(std::chrono::milliseconds { 300 });
    test.controller.tick(test.steady());
    REQUIRE(count_operations(test.workspace, operation_kind::search_commits) == 1);
    const operation_id operation { tab_of(test).search->operation };
    REQUIRE(operation != 0);
    CHECK(tab_of(test).search->submitted == "인사말");
    // 같은 질의로는 다시 내지 않는다 (caret만 옮겼다).
    test.advance(std::chrono::milliseconds { 300 });
    test.controller.tick(test.steady());
    CHECK(count_operations(test.workspace, operation_kind::search_commits) == 1);

    // git이 본문에서 찾은 읽은 커밋(4)과 아직 읽지 않은 커밋(9)을 돌려준다.
    commit_search found {};
    found.request = search_commits_request { .query = "인사말" };
    found.commits.push_back(commit_info { .sha = sha_of(5), .subject = "fix: 인사말을 고친다" });
    found.commits.push_back(commit_info { .sha = sha_of(4), .subject = "docs: 설명을 더한다" });
    found.commits.push_back(commit_info { .sha = sha_of(9), .subject = "old: 인사말" });
    REQUIRE(session->apply(operation, commit_search_loaded { found }));
    test.controller.after_events();

    const view_snapshot view { test.controller.make_view(1400.0f, 900.0f, 1.0f) };
    const graph_search_view& search { search_view_of(view) };
    REQUIRE(search.hits != nullptr);
    CHECK(search.hits->size() == 4);
    CHECK(search.hits->contains(sha_of(9)));
    CHECK(search.can_step);
    CHECK(search.status == u8"4개");

    // 앞으로: 읽은 커밋이 그래프 순서로(5, 4, 3), 그다음 읽지 않은 커밋(9)이다.
    REQUIRE(test.send(graph_search_step_intent { .forward = true }));
    CHECK(tab_of(test).selected_sha == sha_of(5));
    CHECK(tab_of(test).search->current == sha_of(5));
    CHECK(search_view_of(test.controller.make_view(1400.0f, 900.0f, 1.0f)).status == u8"1 / 4");
    REQUIRE(test.send(graph_search_step_intent { .forward = true }));
    CHECK(tab_of(test).selected_sha == sha_of(4));
    REQUIRE(test.send(graph_search_step_intent { .forward = true }));
    CHECK(tab_of(test).selected_sha == sha_of(3));
    // 읽지 않은 커밋은 그래프에서 찾기로 넘어간다. 이 이력은 끝까지 읽혀 있어 없다고 알리고 선택은 그대로다.
    REQUIRE(test.send(graph_search_step_intent { .forward = true }));
    CHECK(tab_of(test).search->current == sha_of(9));
    CHECK(tab_of(test).selected_sha == sha_of(3));
    // 끝에서 처음으로 돌고, 뒤로는 거꾸로 돈다.
    REQUIRE(test.send(graph_search_step_intent { .forward = true }));
    CHECK(tab_of(test).selected_sha == sha_of(5));
    REQUIRE(test.send(graph_search_step_intent { .forward = false }));
    CHECK(tab_of(test).search->current == sha_of(9));
    REQUIRE(test.send(graph_search_step_intent { .forward = false }));
    CHECK(tab_of(test).selected_sha == sha_of(3));

    // 질의를 고치면 앞 질의의 git 결과는 쓰지 않고, 새 질의를 낼 때 앞 검색을 거둔다.
    type_search(test, u8"을");
    const view_snapshot edited { test.controller.make_view(1400.0f, 900.0f, 1.0f) };
    CHECK_FALSE(search_view_of(edited).hits->contains(sha_of(9)));
    CHECK(search_view_of(edited).hits->contains(sha_of(5)));
    CHECK(tab_of(test).search->current.empty());

    // 닫으면 강조가 사라진다.
    REQUIRE(test.send(close_graph_search_intent {}));
    CHECK_FALSE(test.controller.make_view(1400.0f, 900.0f, 1.0f).repository->graph.search.has_value());
}

TEST_CASE("stepping starts from the selected commit when it is a hit", "[presentation][controller][search]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    repository_session* const session { test.workspace.find_session(test.controller.selected_repository_id()) };
    REQUIRE(session != nullptr);
    load_history(*session, searchable_history());
    static_cast<void>(test.controller.make_view(1400.0f, 900.0f, 1.0f));
    REQUIRE(test.send(select_commit_intent { graph_item_key(sha_of(3)) }));
    REQUIRE(test.send(open_graph_search_intent {}));
    type_search(test, u8"인사말");

    // 고른 커밋이 결과에 있으면 첫 걸음은 그 커밋에 서고, 다음 걸음부터 옮긴다.
    REQUIRE(test.send(graph_search_step_intent { .forward = true }));
    CHECK(tab_of(test).search->current == sha_of(3));
    REQUIRE(test.send(graph_search_step_intent { .forward = true }));
    CHECK(tab_of(test).search->current == sha_of(5));
    CHECK(tab_of(test).selected_sha == sha_of(5));

    // 다음·이전 명령은 검색이 열려 있을 때만 쓸 수 있다.
    command_availability open {};
    open.repository = true;
    open.graph_search = true;
    CHECK(command_available("graph.search.next", open));
    open.graph_search = false;
    CHECK_FALSE(command_available("graph.search.previous", open));
    CHECK(command_available("graph.search", open));
}
