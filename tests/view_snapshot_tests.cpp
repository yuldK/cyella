// 화면 snapshot builder다. 세션은 사건을 직접 넣어 만들고(git 없이), operation 상태는 값으로 짓는다.
#include "view_snapshot_support.h"

#include "presentation/snapshot_dialogs.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>

using namespace cyella;
using namespace cyella::test::snapshot;

namespace {
    // 웹뷰가 없는 대체 보기로 문맥 줄 `line_count`개짜리 diff 하나를 연 화면의 diff 판이다.
    diff_panel_view fallback_diff_of(const std::int32_t line_count)
    {
        snapshot_harness harness {};
        const repository_tab_id tab { harness.open(u8"C:\\work\\repo", "repo-1") };
        repository_session& session { *harness.sessions["repo-1"] };
        load(session, linear_history(2), 0);
        const diff_file_request request { .path = "big.txt", .view = diff_view::unstaged };
        harness.ui.tabs[tab].diff = request;

        session.expect_file_diff(5);
        file_diff diff {};
        diff.request = request;
        diff_file file {};
        file.old_path = file.new_path = request.path;
        diff_hunk hunk {};
        hunk.header = "@@ -1 +1 @@";
        for (std::int32_t number { 1 }; number <= line_count; ++number)
            hunk.lines.push_back({ diff_line_kind::context, number, number, "line" });
        file.hunks.push_back(std::move(hunk));
        diff.files.push_back(std::move(file));
        REQUIRE(session.apply(5, file_diff_loaded { std::move(diff) }));
        return *harness.build().repository->diff;
    }
} // namespace

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

TEST_CASE("remote toolbar explains unavailable synchronization actions", "[presentation][snapshot]")
{
    snapshot_harness harness {};
    harness.open(u8"C:\\work\\repo", "repo-1");
    repository_session& session { *harness.sessions["repo-1"] };
    repository_snapshot snapshot { make_snapshot(sha_of(1), 0) };
    snapshot.summary.has_upstream = false;
    snapshot.summary.remotes.clear();
    const std::uint64_t generation { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { generation, snapshot }));
    const toolbar_view no_remote { harness.build().toolbar };
    CHECK_FALSE(no_remote.fetch_enabled);
    CHECK_FALSE(no_remote.pull_enabled);
    // 원격이 없어도 push 단추는 켜진다. 누르면 원격을 더하고 게시하는 상자가 열린다.
    CHECK(no_remote.push_enabled);
    CHECK(no_remote.push_hint.find(u8"원격") != std::u8string::npos);

    snapshot.summary.remotes.push_back({ .name = "origin", .fetch_url = "https://example.com/repo.git" });
    REQUIRE(session.apply(snapshot_loaded { generation, snapshot }));
    CHECK(harness.build().toolbar.push_enabled);
    CHECK(harness.build().toolbar.push_needs_dialog);

    snapshot.summary.is_detached = true;
    REQUIRE(session.apply(snapshot_loaded { generation, snapshot }));
    const toolbar_view detached { harness.build().toolbar };
    CHECK_FALSE(detached.push_enabled);
    CHECK(detached.push_hint.find(u8"HEAD") != std::u8string::npos);
    // 분리된 HEAD의 표시는 카탈로그 문구에 짧은 SHA를 채운 것이다.
    const std::string head { sha_of(1) };
    CHECK(detached.branch_text == u8"(detached) " + std::u8string { head.begin(), head.begin() + 7 });
}

TEST_CASE("git verbs keep their English names in the operation list", "[presentation][snapshot]")
{
    // 한국어 화면에서도 git 동사는 영어 표기다. 카탈로그 키로 옮겨도 보이는 글은 그대로여야 한다.
    CHECK(operation_display_name(operation_kind::stage_files) == u8"Stage");
    CHECK(operation_display_name(operation_kind::unstage_files) == u8"Unstage");
    CHECK(operation_display_name(operation_kind::fetch) == u8"Fetch");
    CHECK(operation_display_name(operation_kind::pull) == u8"Pull");
    CHECK(operation_display_name(operation_kind::push_execute) == u8"Push");
    CHECK(operation_display_name(operation_kind::stash_push) == u8"Stash");
    CHECK(operation_display_name(operation_kind::stash_pop) == u8"Pop");
    CHECK(operation_display_name(operation_kind::clone) == u8"Clone");
    CHECK(operation_display_name(operation_kind::init) == u8"Init");
    CHECK(operation_display_name(operation_kind::lfs_pull) == u8"LFS pull");
    CHECK(operation_display_name(operation_kind::lfs_fetch) == u8"LFS fetch");
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

TEST_CASE("a failed operation becomes an error notification with its commands and git output", "[presentation][snapshot]")
{
    operation_status failed { .id = 3, .kind = operation_kind::push_execute, .state = operation_state::failed, .repository_id = "repo-1" };
    failed.commands = { { .command_line = u8"git push --progress", .exit_code = 1 } };
    failed.failure = operation_failure { .kind = failure_kind::rejected_non_ff, .summary = u8"원격이 거절했습니다.", .detail = u8"! [rejected]\nhint: pull first" };
    const std::optional<notification> value { failure_notification(failed) };
    REQUIRE(value.has_value());
    CHECK(value->severity == notification_severity::error);
    CHECK(value->title == u8"Push 실패");
    CHECK(value->text == u8"원격이 거절했습니다.");
    CHECK(value->operation == 3);
    CHECK(value->repository_id == "repo-1");
    REQUIRE(value->details.size() == 3);
    CHECK(value->details[0].starts_with(u8"$ git push --progress"));
    CHECK(value->details[0].find(u8"→ exit 1") != std::u8string::npos);
    REQUIRE(value->actions.size() == 1);
    CHECK(value->actions[0].key == u8"pull_then_push");

    operation_status succeeded { .id = 4, .kind = operation_kind::fetch, .state = operation_state::succeeded, .repository_id = "repo-1" };
    CHECK_FALSE(failure_notification(succeeded).has_value());
}

TEST_CASE("blame and file history reads are not user operations", "[presentation][snapshot]")
{
    // 파일을 바꿔 볼 때마다 앞 읽기가 취소된다. 사용자 작업으로 세면 취소된 읽기가 상태 줄에 뜨고 앞선 작업의 실패 토스트를 내린다.
    for (const operation_kind kind : { operation_kind::blame_file, operation_kind::file_history })
    {
        const operation_status cancelled { .id = 5, .kind = kind, .state = operation_state::cancelled, .repository_id = "repo-1", .cancel_requested = true };
        CHECK_FALSE(is_user_operation_status(cancelled));
        CHECK_FALSE(failure_notification(cancelled).has_value());
    }
    CHECK(operation_display_name(operation_kind::blame_file) == u8"blame 읽기");
    CHECK(operation_display_name(operation_kind::file_history) == u8"파일 이력 읽기");
}

TEST_CASE("commit searches are not user operations", "[presentation][snapshot]")
{
    // 질의를 고쳐 칠 때마다 앞 검색이 취소된다. 사용자 작업으로 세면 글자마다 취소된 검색이 상태 줄에 뜬다.
    // 읽는 중과 실패는 검색 상자가 보인다. 실패도 토스트로 띄우지 않는다.
    const operation_status cancelled { .id = 5, .kind = operation_kind::search_commits, .state = operation_state::cancelled, .repository_id = "repo-1", .cancel_requested = true };
    CHECK_FALSE(is_user_operation_status(cancelled));
    CHECK_FALSE(failure_notification(cancelled).has_value());
    operation_status failed { .id = 6, .kind = operation_kind::search_commits, .state = operation_state::failed, .repository_id = "repo-1" };
    failed.failure = operation_failure { .kind = failure_kind::other, .summary = u8"git failed" };
    CHECK_FALSE(is_user_operation_status(failed));
    CHECK(operation_display_name(operation_kind::search_commits) == u8"커밋 검색");
}

TEST_CASE("notifications become toasts newest first until the center is opened", "[presentation][snapshot]")
{
    snapshot_harness harness {};
    const repository_tab_id tab { harness.open(u8"C:\\work\\repo", "repo-1") };
    load(*harness.sessions["repo-1"], linear_history(2), 0);
    for (std::uint64_t id { 1 }; id <= 5; ++id)
        harness.ui.notifications.push_back(notification { .id = id, .text = u8"안내입니다." });
    harness.ui.notifications[1].toast = false;
    harness.ui.notifications[4].read = true;
    view_snapshot view { harness.build() };
    // 최신이 앞이고, 내려간 토스트는 건너뛰며, 세 장까지다.
    REQUIRE(view.toasts.size() == toast_visible_limit);
    CHECK(view.toasts[0].id == 5);
    CHECK(view.toasts[1].id == 4);
    CHECK(view.toasts[2].id == 3);
    CHECK_FALSE(view.notification_center.has_value());
    CHECK(view.status.notifications == 5);
    CHECK(view.status.unread_notifications == 4);

    // 센터가 열려 있으면 토스트는 없고 모든 알림이 최신부터 보인다.
    harness.ui.notifications_open = true;
    view = harness.build();
    CHECK(view.toasts.empty());
    REQUIRE(view.notification_center.has_value());
    REQUIRE(view.notification_center->entries.size() == 5);
    CHECK(view.notification_center->entries.front().id == 5);
    CHECK(view.notification_center->entries.back().id == 1);
    CHECK(view.status.notifications_open);

    // 거르기는 그 심각도만 남기고, 머리의 개수는 거르기 전의 것이다. 모두 같은 날이면 날 머리글을 세우지 않는다.
    harness.ui.notifications[1].severity = notification_severity::warning;
    harness.ui.notifications[3].severity = notification_severity::error;
    harness.ui.notifications_filter = notification_filter::error;
    view = harness.build();
    REQUIRE(view.notification_center.has_value());
    REQUIRE(view.notification_center->entries.size() == 1);
    CHECK(view.notification_center->entries.front().id == 4);
    CHECK(view.notification_center->filter == notification_filter::error);
    CHECK(view.notification_center->total == 5);
    CHECK(view.notification_center->warnings == 1);
    CHECK(view.notification_center->errors == 1);
    CHECK_FALSE(view.notification_center->show_days);
    harness.ui.notifications_filter = notification_filter::all;

    // 알림이 두 날에 걸치면 날마다 머리글을 세운다.
    const auto now { std::chrono::system_clock::time_point { std::chrono::seconds { 1700000000 + 3600 } } };
    for (notification& value : harness.ui.notifications)
        value.at = now;
    harness.ui.notifications.front().at = now - std::chrono::days { 1 };
    view = harness.build();
    REQUIRE(view.notification_center.has_value());
    CHECK(view.notification_center->show_days);
    CHECK(view.notification_center->entries.front().day == tr(u8"snapshot.day.today"));
    CHECK(view.notification_center->entries.back().day == tr(u8"snapshot.day.yesterday"));
    const std::int64_t seconds { 1700000000 + 3600 };
    CHECK(day_text(seconds, seconds - 5 * 86400).find(u8"11") != std::u8string::npos);
    CHECK(day_text(seconds, seconds - 400 * 86400).find(u8"2022") != std::u8string::npos);

    // 입력이 필요한 작업은 알림이 아니라 체크아웃 정책 대화 상자가 맡는다.
    harness.ui.notifications.clear();
    harness.ui.notifications_open = false;
    operation_status checkout { .id = 4, .kind = operation_kind::checkout_branch, .state = operation_state::needs_input, .repository_id = "repo-1" };
    checkout.needs_input = input_request { .kind = input_kind::dirty_worktree, .files = { "a.txt" }, .target = "feature" };
    harness.operations.push_back(checkout);
    view = harness.build();
    CHECK(view.toasts.empty());
    REQUIRE(view.form.has_value());
    CHECK(view.form->id == u8"dirty-dialog");
    static_cast<void>(tab);
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
    CHECK(all.counts == std::array<std::uint32_t, sidebar_section_count> { 2, 1, 1, 1, 0, 0, 0 });
    std::vector<std::u8string> keys {};
    for (const sidebar_row_view& row : all.rows)
        keys.push_back(row.key);
    // submodule·worktree 구역은 항목이 없어도 머리가 선다 (LFS 구역은 저장소가 LFS를 쓸 때만 선다).
    std::vector<std::u8string> expected { u8"h:LOCAL", u8"l:main", u8"l:feature/graph", u8"h:REMOTE", u8"r:origin", u8"rb:origin/main", u8"h:TAGS", u8"t:v1.0", u8"h:STASHES", u8"s:0" };
    expected.push_back(u8"h:SUBMODULES");
    expected.push_back(u8"h:WORKTREES");
    CHECK(keys == expected);
    CHECK(all.rows[1].current);
    CHECK(all.rows[1].ahead == 1);
    CHECK(all.rows[2].no_upstream);
    // 툴팁은 첫 줄이 이름이고 나머지가 한 줄씩이다 (카드로 그린다).
    CHECK(all.rows[1].tooltip == u8"main\nupstream: origin/main\n보낼 커밋 1개");
    CHECK(all.rows[2].tooltip == u8"feature/graph\nupstream 없음");
    CHECK(all.rows[4].group);
    CHECK(all.rows[4].detail == u8"https://example.com/repo.git");
    CHECK(all.rows[4].tooltip == u8"origin\nhttps://example.com/repo.git");
    CHECK(all.rows[7].tooltip == u8"v1.0\n원격에 없는 태그");
    CHECK(all.rows[9].tooltip == u8"stash@{0}\nWIP on main");
    CHECK(all.rows[5].label == u8"main");
    CHECK(all.rows[5].depth == 1);
    CHECK(all.rows[7].local_only);

    const sidebar_rows filtered { build_sidebar_rows(snapshot, u8"FEAT", 0) };
    CHECK(filtered.counts[0] == 1);
    CHECK(filtered.counts[1] == 0);
    CHECK(filtered.rows.size() == 7);

    // 이름에 '/'가 든 ref는 '/'를 친 질의로도 걸러진다 (질의만 접으면 0건이 된다).
    const sidebar_rows slashed { build_sidebar_rows(snapshot, u8"feature/", 0) };
    CHECK(slashed.counts[0] == 1);
    CHECK(slashed.counts[1] == 0);
    const sidebar_rows remote_slashed { build_sidebar_rows(snapshot, u8"origin/ma", 0) };
    CHECK(remote_slashed.counts[0] == 0);
    CHECK(remote_slashed.counts[1] == 1);

    const std::uint8_t collapsed { static_cast<std::uint8_t>((1u << static_cast<unsigned>(sidebar_section::local)) | (1u << static_cast<unsigned>(sidebar_section::tags))) };
    const sidebar_rows folded { build_sidebar_rows(snapshot, {}, collapsed) };
    CHECK(folded.rows[0].collapsed);
    CHECK(folded.rows[1].key == u8"h:REMOTE");
    CHECK(folded.counts[0] == 2);
}

TEST_CASE("sidebar rows carry the graph filter mark and say so in the tooltip", "[presentation][snapshot][graph_filter]")
{
    const repository_snapshot snapshot { make_snapshot(sha_of(0), 0) };
    const auto row_of = [](const sidebar_rows& rows, const std::u8string& key) {
        const auto found { std::ranges::find(rows.rows, key, &sidebar_row_view::key) };
        REQUIRE(found != rows.rows.end());
        return *found;
    };

    // 필터를 주지 않거나 비어 있으면 표시가 없고 툴팁도 그대로다.
    const graph_ref_filter none {};
    for (const sidebar_rows& rows : { build_sidebar_rows(snapshot, {}, 0), build_sidebar_rows(snapshot, {}, 0, nullptr, &none) })
    {
        for (const sidebar_row_view& row : rows.rows)
            CHECK(row.graph_mark == graph_ref_mark::none);
        CHECK(row_of(rows, u8"l:feature/graph").tooltip == u8"feature/graph\nupstream 없음");
    }

    graph_ref_filter hidden {};
    hidden.hidden = { "refs/heads/feature/graph", "refs/tags/v1.0" };
    const sidebar_rows hiding { build_sidebar_rows(snapshot, {}, 0, nullptr, &hidden) };
    CHECK(row_of(hiding, u8"l:feature/graph").graph_mark == graph_ref_mark::hidden);
    CHECK(row_of(hiding, u8"l:feature/graph").tooltip == u8"feature/graph\nupstream 없음\n그래프에서 숨김");
    CHECK(row_of(hiding, u8"t:v1.0").graph_mark == graph_ref_mark::hidden);
    CHECK(row_of(hiding, u8"t:v1.0").tooltip == u8"v1.0\n원격에 없는 태그\n그래프에서 숨김");
    CHECK(row_of(hiding, u8"l:main").graph_mark == graph_ref_mark::none);
    CHECK(row_of(hiding, u8"rb:origin/main").graph_mark == graph_ref_mark::none);
    // 머리 줄과 원격 묶음에는 표시가 없다.
    CHECK(row_of(hiding, u8"h:LOCAL").graph_mark == graph_ref_mark::none);
    CHECK(row_of(hiding, u8"r:origin").graph_mark == graph_ref_mark::none);

    // "이것만 보기"가 있으면 그 ref만 표시하고, 숨긴 것은 그동안 듣지 않으므로 표시하지 않는다.
    graph_ref_filter only { hidden };
    only.only = { "refs/remotes/origin/main" };
    const sidebar_rows showing { build_sidebar_rows(snapshot, {}, 0, nullptr, &only) };
    CHECK(row_of(showing, u8"rb:origin/main").graph_mark == graph_ref_mark::only);
    CHECK(row_of(showing, u8"rb:origin/main").tooltip == u8"origin/main\n그래프에 이것만 보는 중");
    CHECK(row_of(showing, u8"l:feature/graph").graph_mark == graph_ref_mark::none);
    CHECK(row_of(showing, u8"t:v1.0").graph_mark == graph_ref_mark::none);
    // 짧은 이름이 같은 로컬 브랜치 main은 다른 ref다.
    CHECK(row_of(showing, u8"l:main").graph_mark == graph_ref_mark::none);
}

TEST_CASE("the snapshot carries the filter band only while the session has a graph filter", "[presentation][snapshot][graph_filter]")
{
    snapshot_harness harness {};
    static_cast<void>(harness.open(u8"C:\\work\\repo", "repo-1"));
    repository_session& session { *harness.sessions["repo-1"] };
    load(session, linear_history(3), 0);
    CHECK_FALSE(harness.build().repository->graph.filter.has_value());

    graph_ref_filter filter {};
    filter.hidden = { "refs/heads/feature/graph" };
    REQUIRE(session.set_graph_filter(filter));
    {
        const view_snapshot view { harness.build() };
        REQUIRE(view.repository->graph.filter.has_value());
        CHECK(view.repository->graph.filter->status == u8"브랜치 1개를 숨긴 중");
        const auto row { std::ranges::find(view.repository->sidebar, std::u8string { u8"l:feature/graph" }, &sidebar_row_view::key) };
        REQUIRE(row != view.repository->sidebar.end());
        CHECK(row->graph_mark == graph_ref_mark::hidden);
    }

    filter.hidden.push_back("refs/tags/v1.0");
    filter.hidden.push_back("refs/remotes/origin/main");
    REQUIRE(session.set_graph_filter(filter));
    CHECK(harness.build().repository->graph.filter->status == u8"브랜치 3개를 숨긴 중");

    // "이것만 보기"가 있으면 그 수를 말한다. 숨긴 것은 그동안 세지 않는다.
    filter.only = { "refs/heads/main", "refs/tags/v1.0" };
    REQUIRE(session.set_graph_filter(filter));
    CHECK(harness.build().repository->graph.filter->status == u8"브랜치 2개만 보는 중");

    // 현재 브랜치는 숨긴 목록에 남지 않는다. 남는 것이 없으면 띠도 없다.
    graph_ref_filter current {};
    current.hidden = { "refs/heads/main" };
    REQUIRE(session.set_graph_filter(current));
    CHECK(session.graph_filter().empty());
    CHECK_FALSE(harness.build().repository->graph.filter.has_value());
    // 그대로면 바뀐 것이 없다.
    CHECK_FALSE(session.set_graph_filter({}));
}

TEST_CASE("a token in a remote URL does not show in the sidebar row or its tooltip", "[presentation][snapshot]")
{
    repository_snapshot snapshot { make_snapshot(sha_of(0), 0) };
    snapshot.summary.remotes = { { .name = "origin", .fetch_url = "https://user:tok@h/r.git" } };
    const sidebar_rows rows { build_sidebar_rows(snapshot, {}, 0) };
    const auto group { std::ranges::find(rows.rows, std::u8string { u8"r:origin" }, &sidebar_row_view::key) };
    REQUIRE(group != rows.rows.end());
    CHECK(group->detail.find(u8"tok") == std::u8string::npos);
    CHECK(group->detail.find(u8"user:***@") != std::u8string::npos);
    CHECK(group->tooltip.find(u8"tok") == std::u8string::npos);
    CHECK(group->tooltip.find(u8"user:***@") != std::u8string::npos);
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

TEST_CASE("the text fallback tells when it cut a long diff", "[presentation][snapshot]")
{
    // 상한(2만 줄) 안이면 자르지 않는다. hunk 머리 한 줄과 본문 19999줄이 정확히 상한이다.
    const diff_panel_view exact { fallback_diff_of(19999) };
    CHECK_FALSE(exact.use_webview);
    CHECK_FALSE(exact.text_truncated);
    REQUIRE(exact.lines.size() == 20000);
    CHECK_FALSE(exact.lines.back().hunk_header);

    // 넘치면 상한까지 담고 끝에 안내 행 하나를 세워 diff가 거기서 끝난 것으로 읽히지 않게 한다.
    const diff_panel_view cut { fallback_diff_of(30000) };
    CHECK(cut.text_truncated);
    REQUIRE(cut.lines.size() == 20001);
    CHECK_FALSE(cut.lines[19999].hunk_header);
    CHECK(cut.lines.back().hunk_header);
}

TEST_CASE("forged commit times far outside the calendar still format without throwing", "[presentation][snapshot]")
{
    constexpr std::int64_t now { 1700000000 };
    constexpr std::int64_t forged { 9999999999999 };
    constexpr std::int64_t lowest { std::numeric_limits<std::int64_t>::min() };
    constexpr std::int64_t highest { std::numeric_limits<std::int64_t>::max() };
    CHECK_NOTHROW(absolute_time_text(forged));
    CHECK_NOTHROW(absolute_time_text(lowest));
    CHECK_NOTHROW(absolute_time_text(highest));
    CHECK_NOTHROW(day_text(now, forged));
    CHECK_NOTHROW(day_text(now, lowest));
    CHECK_NOTHROW(day_text(highest, now));
    // 뺄셈이 넘치는 끝값에서도 미래의 시각은 방금이다.
    CHECK(relative_time_text(now, forged) == u8"방금");
    CHECK(relative_time_text(0, highest) == u8"방금");
    CHECK_NOTHROW(relative_time_text(highest, lowest));
    // 범위 안의 값은 그대로다.
    CHECK(absolute_time_text(now).find(u8"2023") != std::u8string::npos);
    CHECK(day_text(now, forged) == tr(u8"snapshot.day.today"));
}

TEST_CASE("a nested repository path with a trailing slash keeps its name and original path", "[presentation][snapshot]")
{
    // git은 작업 폴더 안의 다른 저장소를 `vendor/lib/`처럼 끝에 '/'를 붙여 낸다.
    const file_row_view nested { make_file_row({ .path = "vendor/lib/", .status = '?' }, u8"u:") };
    CHECK(nested.name == u8"lib");
    CHECK(nested.folder == u8"/vendor");
    // stage·diff 요청이 쓰는 경로와 행 키는 원래 값이다.
    CHECK(nested.path == "vendor/lib/");
    CHECK(nested.key == u8"u:vendor/lib/");
    const file_row_view top { make_file_row({ .path = "lib/", .status = '?' }, u8"u:") };
    CHECK(top.name == u8"lib");
    CHECK(top.folder.empty());
}

TEST_CASE("an opened conflict diff selects its row and steps through the conflict files", "[presentation][snapshot]")
{
    snapshot_harness harness {};
    const repository_tab_id tab { harness.open(u8"C:\\work\\repo", "repo-1") };
    repository_session& session { *harness.sessions["repo-1"] };
    repository_snapshot snapshot { make_snapshot(sha_of(0), 2) };
    snapshot.conflicts = {
        { .path = "c1.cpp", .status = 'U', .side = change_side::unstaged, .conflict_code = "UU" },
        { .path = "c2.cpp", .status = 'U', .side = change_side::unstaged, .conflict_code = "UU" },
    };
    const std::uint64_t generation { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { generation, snapshot }));
    harness.ui.tabs[tab].selected_sha = std::string { graph_wip_sha };
    // 충돌 파일 행은 unstaged 보기로 diff를 연다.
    harness.ui.tabs[tab].diff = diff_file_request { .path = "c1.cpp", .view = diff_view::unstaged };

    const view_snapshot first { harness.build() };
    REQUIRE(first.repository->working.has_value());
    REQUIRE(first.repository->working->conflicts.size() == 2);
    CHECK(first.repository->working->conflicts[0].selected);
    CHECK_FALSE(first.repository->working->conflicts[1].selected);
    REQUIRE(first.repository->diff.has_value());
    CHECK(first.repository->diff->file_position == 1);
    CHECK(first.repository->diff->file_count == 2);
    CHECK_FALSE(first.repository->diff->previous_file.has_value());
    REQUIRE(first.repository->diff->next_file.has_value());
    CHECK(first.repository->diff->next_file->path == "c2.cpp");
    CHECK(first.repository->diff->next_file->view == diff_view::unstaged);

    harness.ui.tabs[tab].diff = diff_file_request { .path = "c2.cpp", .view = diff_view::unstaged };
    const view_snapshot second { harness.build() };
    CHECK_FALSE(second.repository->working->conflicts[0].selected);
    CHECK(second.repository->working->conflicts[1].selected);
    REQUIRE(second.repository->diff->previous_file.has_value());
    CHECK(second.repository->diff->previous_file->path == "c1.cpp");
    CHECK_FALSE(second.repository->diff->next_file.has_value());
    // 충돌 파일이 아닌 unstaged 파일의 diff는 충돌 행을 고르지 않는다.
    harness.ui.tabs[tab].diff = diff_file_request { .path = "src/a.cpp", .view = diff_view::unstaged };
    const view_snapshot plain { harness.build() };
    CHECK_FALSE(plain.repository->working->conflicts[0].selected);
    CHECK_FALSE(plain.repository->working->conflicts[1].selected);
    CHECK(plain.repository->working->unstaged[0].selected);
}

TEST_CASE("Stash stays off before the first commit and says why", "[presentation][snapshot]")
{
    snapshot_harness harness {};
    harness.open(u8"C:\\work\\repo", "repo-1");
    repository_session& session { *harness.sessions["repo-1"] };
    // 첫 커밋 전에는 현재 브랜치 이름은 있어도 HEAD 커밋이 없다.
    repository_snapshot snapshot { make_snapshot({}, 2) };
    const std::uint64_t generation { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { generation, snapshot }));
    const toolbar_view unborn { harness.build().toolbar };
    CHECK(unborn.branch_text == u8"main");
    CHECK_FALSE(unborn.stash_enabled);
    CHECK_FALSE(unborn.stash_hint.empty());

    // 이미 그린 행(WIP)이 있으면 새 snapshot은 커밋 목록이 올 때까지 보류되므로 refresh 한 바퀴를 다 돌린다.
    load(session, linear_history(2), 2);
    const toolbar_view born { harness.build().toolbar };
    CHECK(born.stash_enabled);
    CHECK(born.stash_hint.empty());
}
