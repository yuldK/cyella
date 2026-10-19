// 화면 snapshot builder다. 세션은 사건을 직접 넣어 만들고(git 없이), operation 상태는 값으로 짓는다.
#include "view_snapshot_support.h"

#include <catch2/catch_test_macros.hpp>

using namespace cyella;
using namespace cyella::test::snapshot;

TEST_CASE("start tab snapshot lists recent repositories with open and missing states", "[presentation][snapshot]")
{
    snapshot_harness harness {};
    harness.settings.recent_repositories = {
        { .path = u8"C:\\work\\pinned", .display_name = u8"pinned", .opened_at = u8"2026-10-19T09:00:00Z", .pinned = true },
        { .path = u8"C:\\work\\cyella", .display_name = u8"cyella", .opened_at = u8"2026-10-20T09:00:00Z" },
        { .path = u8"C:\\work\\missing", .display_name = u8"missing", .opened_at = u8"2026-10-18T09:00:00Z" },
    };
    const view_snapshot view { harness.build() };
    REQUIRE(view.start.has_value());
    CHECK_FALSE(view.repository.has_value());
    REQUIRE(view.start->list.items.size() == 3);
    CHECK(view.start->list.items[0].pinned);
    CHECK(view.start->list.items[2].exists == false);
    CHECK(view.start->open_keys.empty());
    CHECK(view.tabs.size() == 1);
    CHECK(view.tabs.front().start);

    harness.ui.start_search = text(u8"cyel");
    const view_snapshot searched { harness.build() };
    REQUIRE(searched.start->list.items.size() == 1);
    CHECK(searched.start->list.items[0].display_name == u8"cyella");
    CHECK(searched.start->search.displayed_text() == u8"cyel");
}

TEST_CASE("repository snapshot exposes toolbar state, graph model and labels", "[presentation][snapshot]")
{
    snapshot_harness harness {};
    harness.open(u8"C:\\work\\repo", "repo-1");
    load(*harness.sessions["repo-1"], linear_history(5), 2);

    const view_snapshot view { harness.build() };
    REQUIRE(view.repository.has_value());
    const repository_view& repository { *view.repository };
    CHECK(repository.loaded);
    CHECK(view.mode == layout_mode::wide);
    CHECK(view.toolbar.branch_text == u8"main");
    CHECK(view.toolbar.ahead == 1);
    CHECK(view.toolbar.fetch_enabled);
    CHECK(view.toolbar.pull_enabled);
    CHECK(view.toolbar.push_enabled);
    CHECK_FALSE(view.toolbar.push_needs_dialog);
    CHECK(view.toolbar.stash_enabled);
    CHECK_FALSE(view.toolbar.pop_enabled);
    CHECK(view.tabs.front().marker == tab_marker::changes);

    const graph_view& graph { repository.graph };
    REQUIRE(graph.items != nullptr);
    // WIP 행 + 커밋 다섯이다.
    CHECK(graph.items->size() == 6);
    CHECK(graph.has_wip);
    CHECK(graph.items->front().key == u8"WIP");
    CHECK(graph.head_row_index == 1);
    CHECK(graph.wip_count == 2);
    REQUIRE(graph.labels != nullptr);
    CHECK(graph.labels->at(sha_of(4)).front().text == u8"main");
    CHECK(graph.labels->at(sha_of(0)).size() == 3);
    CHECK(repository.detail == detail_mode::none);
}

TEST_CASE("selecting WIP builds the working area and gates the commit button", "[presentation][snapshot]")
{
    snapshot_harness harness {};
    const repository_tab_id tab { harness.open(u8"C:\\work\\repo", "repo-1") };
    load(*harness.sessions["repo-1"], linear_history(3), 2);
    harness.ui.tabs[tab].selected_sha = std::string { graph_wip_sha };

    view_snapshot view { harness.build() };
    REQUIRE(view.repository->working.has_value());
    const working_area_view& empty { *view.repository->working };
    CHECK(view.repository->detail == detail_mode::working);
    REQUIRE(empty.unstaged.size() == 1);
    CHECK(empty.unstaged[0].name == u8"a.cpp");
    CHECK(empty.unstaged[0].folder == u8"/src");
    CHECK(empty.unstaged[0].added == u8"+3");
    REQUIRE(empty.staged.size() == 1);
    CHECK_FALSE(empty.can_commit);
    CHECK(empty.blocked_reason == u8"커밋 제목을 입력하세요.");
    CHECK(empty.commit_label == u8"main에 커밋 (staged 1개)");

    harness.ui.tabs[tab].commit_subject = text(u8"feat: 그래프를 그린다");
    view = harness.build();
    CHECK(view.repository->working->can_commit);
    CHECK(view.repository->working->subject_length == 14);

    harness.ui.tabs[tab].pending_commit = 7;
    harness.operations.push_back({ .id = 7, .kind = operation_kind::commit, .state = operation_state::running, .repository_id = "repo-1" });
    view = harness.build();
    CHECK(view.repository->working->committing);
    CHECK_FALSE(view.repository->working->can_commit);
    CHECK(view.status.active.has_value());
    CHECK(view.tabs.front().marker == tab_marker::running);
}

TEST_CASE("a commit row selection shows a loading detail until show_commit returns", "[presentation][snapshot]")
{
    snapshot_harness harness {};
    const repository_tab_id tab { harness.open(u8"C:\\work\\repo", "repo-1") };
    load(*harness.sessions["repo-1"], linear_history(3), 0);
    harness.ui.tabs[tab].selected_sha = sha_of(1);

    const view_snapshot view { harness.build() };
    REQUIRE(view.repository->commit.has_value());
    CHECK(view.repository->detail == detail_mode::commit);
    CHECK(view.repository->commit->loading);
    CHECK(view.repository->commit->subject == u8"commit 2");
    CHECK(view.repository->commit->short_sha == u8"0000000");
    CHECK(view.repository->commit->date == u8"1시간 전");
    CHECK(view.repository->graph.has_wip == false);
}

TEST_CASE("failed and needs_input operations become banners until dismissed", "[presentation][snapshot]")
{
    snapshot_harness harness {};
    const repository_tab_id tab { harness.open(u8"C:\\work\\repo", "repo-1") };
    load(*harness.sessions["repo-1"], linear_history(2), 0);

    operation_status failed { .id = 3, .kind = operation_kind::push_execute, .state = operation_state::failed, .repository_id = "repo-1" };
    failed.commands = { { .command_line = u8"git push --progress", .exit_code = 1 } };
    failed.failure = operation_failure { .kind = failure_kind::rejected_non_ff, .summary = u8"원격이 거절했습니다.", .detail = u8"! [rejected]\nhint: pull first" };
    harness.operations.push_back(failed);
    view_snapshot view { harness.build() };
    REQUIRE(view.banner.has_value());
    CHECK(view.banner->kind == banner_kind::failure);
    CHECK(view.banner->title == u8"Push 실패");
    REQUIRE(view.banner->details.size() == 3);
    CHECK(view.banner->details[0].starts_with(u8"$ git push --progress"));
    CHECK(view.status.last_failed);

    harness.ui.tabs[tab].dismissed_banner = 3;
    CHECK_FALSE(harness.build().banner.has_value());

    operation_status checkout { .id = 4, .kind = operation_kind::checkout_branch, .state = operation_state::needs_input, .repository_id = "repo-1" };
    checkout.needs_input = input_request { .kind = input_kind::dirty_worktree, .files = { "a.txt" }, .target = "feature" };
    harness.operations.push_back(checkout);
    view = harness.build();
    // 입력이 필요한 작업은 배너가 아니라 체크아웃 정책 대화 상자가 맡는다.
    CHECK_FALSE(view.banner.has_value());
    REQUIRE(view.form.has_value());
    CHECK(view.form->id == u8"dirty-dialog");

    // 안내는 operation 배너보다 앞선다.
    harness.ui.notice = notice_state { u8"안내입니다.", false };
    view = harness.build();
    REQUIRE(view.banner.has_value());
    CHECK(view.banner->kind == banner_kind::notice);
}

TEST_CASE("reads and automatic refreshes stay out of the status bar and tab markers", "[presentation][snapshot]")
{
    snapshot_harness harness {};
    harness.open(u8"C:\\work\\repo", "repo-1");
    load(*harness.sessions["repo-1"], linear_history(2), 0);
    harness.operations.push_back({ .id = 1, .kind = operation_kind::refresh, .state = operation_state::running, .repository_id = "repo-1", .automatic = true });
    harness.operations.push_back({ .id = 2, .kind = operation_kind::show_commit, .state = operation_state::succeeded, .repository_id = "repo-1", .result_summary = u8"읽었습니다" });
    const view_snapshot view { harness.build() };
    CHECK_FALSE(view.status.active.has_value());
    CHECK(view.status.last_result.empty());
    CHECK(view.tabs.front().marker == tab_marker::none);
}

TEST_CASE("tab markers follow the priority needs_input > running > queued > changes", "[presentation][snapshot]")
{
    operation_status running { .id = 1, .kind = operation_kind::fetch, .state = operation_state::running };
    operation_status queued { .id = 2, .kind = operation_kind::push_execute, .state = operation_state::queued };
    const std::vector<const operation_status*> both { &running, &queued };
    const std::vector<const operation_status*> waiting { &queued };
    CHECK(compute_tab_marker(both, 3, true) == tab_marker::needs_input);
    CHECK(compute_tab_marker(both, 3, false) == tab_marker::running);
    CHECK(compute_tab_marker(waiting, 3, false) == tab_marker::queued);
    CHECK(compute_tab_marker({}, 3, false) == tab_marker::changes);
    CHECK(compute_tab_marker({}, 0, false) == tab_marker::none);
}

TEST_CASE("sidebar rows group remotes, filter by name and collapse sections", "[presentation][snapshot]")
{
    repository_snapshot snapshot { make_snapshot(sha_of(0), 0) };
    snapshot.refs.push_back({ .kind = ref_kind::remote, .name = "origin/HEAD", .target_sha = sha_of(0) });
    snapshot.stashes = { { .index = 0, .name = "stash@{0}", .sha = "s", .message = "WIP on main" } };

    const sidebar_rows all { build_sidebar_rows(snapshot, {}, 0) };
    CHECK(all.counts == std::array<std::uint32_t, sidebar_section_count> { 2, 1, 1, 1 });
    std::vector<std::u8string> keys {};
    for (const sidebar_row_view& row : all.rows)
        keys.push_back(row.key);
    CHECK(keys == std::vector<std::u8string> { u8"h:LOCAL", u8"l:main", u8"l:feature/graph", u8"h:REMOTE", u8"r:origin", u8"rb:origin/main", u8"h:TAGS", u8"t:v1.0", u8"h:STASHES", u8"s:0" });
    CHECK(all.rows[1].current);
    CHECK(all.rows[1].ahead == 1);
    CHECK(all.rows[2].no_upstream);
    CHECK(all.rows[4].group);
    CHECK(all.rows[4].detail == u8"https://example.com/repo.git");
    CHECK(all.rows[5].label == u8"main");
    CHECK(all.rows[5].depth == 1);
    CHECK(all.rows[7].local_only);

    const sidebar_rows filtered { build_sidebar_rows(snapshot, u8"FEAT", 0) };
    CHECK(filtered.counts[0] == 1);
    CHECK(filtered.counts[1] == 0);
    CHECK(filtered.rows.size() == 5);

    const std::uint8_t collapsed { static_cast<std::uint8_t>((1u << static_cast<unsigned>(sidebar_section::local)) | (1u << static_cast<unsigned>(sidebar_section::tags))) };
    const sidebar_rows folded { build_sidebar_rows(snapshot, {}, collapsed) };
    CHECK(folded.rows[0].collapsed);
    CHECK(folded.rows[1].key == u8"h:REMOTE");
    CHECK(folded.counts[0] == 2);
}

TEST_CASE("relative time and file rows read naturally", "[presentation][snapshot]")
{
    CHECK(relative_time_text(100, 70) == u8"방금");
    CHECK(relative_time_text(4000, 1000) == u8"50분 전");
    CHECK(relative_time_text(100000, 10000) == u8"1일 전");
    CHECK(relative_time_text(100000000, 10000) == u8"3년 전");

    const file_row_view renamed { make_file_row({ .path = "src/new.cpp", .original_path = "src/old.cpp", .status = 'R', .side = change_side::staged }, u8"s:") };
    CHECK(renamed.key == u8"s:src/new.cpp");
    CHECK(renamed.name == u8"new.cpp");
    CHECK(renamed.folder == u8"← src/old.cpp");
    const file_row_view untracked { make_file_row({ .path = "notes.txt", .status = '?' }, u8"u:") };
    CHECK(untracked.untracked);
    CHECK(untracked.folder.empty());
}

TEST_CASE("narrow width compacts graph columns and keeps the drawer closed without a selection", "[presentation][snapshot]")
{
    snapshot_harness harness {};
    harness.open(u8"C:\\work\\repo", "repo-1");
    load(*harness.sessions["repo-1"], linear_history(2), 0);
    harness.ui.drawer_open = true;
    const view_snapshot narrow { harness.build(700.0f) };
    CHECK(narrow.mode == layout_mode::narrow);
    CHECK(narrow.repository->graph.compact_columns);
    CHECK_FALSE(narrow.toolbar.show_labels);
    CHECK(narrow.toolbar.show_detail_toggle);
    const view_snapshot wide { harness.build(1440.0f) };
    CHECK_FALSE(wide.drawer_open);
}
