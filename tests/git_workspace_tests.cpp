// 저장소 세션과 operation 표(git_workspace)를 가짜 runner와 수동 dispatcher로 돌린다. job을 언제
// 실행할지 test가 정하므로 대기·취소·늦게 온 결과를 결정적으로 만든다.
#include "application/git_workspace.h"
#include "infrastructure/git_operation_runner.h"

#include "fake_git_runner.h"
#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {
    using namespace cyella;
    using cyella::test::scripted_response;
    using cyella::test::text_list;

    struct workspace_harness
    {
        explicit workspace_harness(const std::size_t log_page_size = 10000, const std::size_t finished_history_limit = 256)
            : runner { process, u8"C:/Git/cmd/git.exe", files, make_options(log_page_size) }
            , workspace { runner, make_dispatcher(), events.sink(), nullptr, make_workspace_options(finished_history_limit) }
        {
            process.when({ "rev-parse", "--absolute-git-dir" }, scripted_response { 0, "C:/work/repo/.git\nC:/work/repo\n" });
            repository = workspace.open_repository(u8"C:/work/repo");
        }

        static git_operation_runner_options make_options(const std::size_t log_page_size)
        {
            git_operation_runner_options options {};
            options.log_page_size = log_page_size;
            options.log_batch_size = 2;
            options.log_flush_interval = std::chrono::hours { 1 };
            return options;
        }

        static git_workspace_options make_workspace_options(const std::size_t finished_history_limit)
        {
            git_workspace_options options {};
            options.refresh_on_open = false;
            options.finished_history_limit = finished_history_limit;
            return options;
        }

        // `accepting`을 끄면 종료 중인 scheduler처럼 접수를 거절하고 거절한 수를 센다.
        operation_dispatcher make_dispatcher()
        {
            return [this, accept = dispatcher.dispatcher()](operation_request request) {
                if (accepting == false)
                {
                    ++refused;
                    return false;
                }
                return accept(std::move(request));
            };
        }

        // 대기 중인 job을 모두 실행하고 사건을 상태에 반영한다. 반영이 새 job(자동 refresh)을 낼 수 있다.
        void settle()
        {
            for (int round { 0 }; round < 16; ++round)
            {
                while (dispatcher.run())
                {
                }
                const std::vector<operation_event> taken { events.take() };
                if (taken.empty() && dispatcher.pending() == 0)
                    return;
                for (const operation_event& event : taken)
                    workspace.apply(event);
            }
        }

        void apply_events()
        {
            for (const operation_event& event : events.take())
                workspace.apply(event);
        }

        void script_history(const int count, const bool dirty)
        {
            const std::string head { test::sha_of(count) };
            process.when({ "status" }, scripted_response { 0, test::status_header(head, "main") + (dirty ? test::status_modified("a.txt") : std::string {}) });
            std::string log {};
            for (int index { count }; index >= 1; --index)
                log.append(test::log_record(test::sha_of(index), index == 1 ? text_list {} : text_list { test::sha_of(index - 1) }));
            process.when({ "log", "--topo-order" }, scripted_response { 0, log });
        }

        [[nodiscard]] const operation_status& status(const operation_id id) const
        {
            const operation_status* const found { workspace.find_operation(id) };
            REQUIRE(found != nullptr);
            return *found;
        }

        [[nodiscard]] repository_session& session()
        {
            repository_session* const found { workspace.find_session(repository) };
            REQUIRE(found != nullptr);
            return *found;
        }

        test::fake_git_runner process {};
        test::fake_workspace_files files {};
        test::manual_dispatcher dispatcher {};
        test::event_queue events {};
        bool accepting { true };
        std::size_t refused { 0 };
        git_operation_runner runner;
        git_workspace workspace;
        std::string repository {};
    };
} // namespace

TEST_CASE("a write operation goes queued, running, succeeded and then refreshes", "[workspace][state]")
{
    workspace_harness test {};
    test.script_history(3, false);

    const operation_id id { test.workspace.submit(test.repository, stage_files_request { { "a.txt" } }) };
    REQUIRE(id != 0);
    CHECK(test.status(id).state == operation_state::queued);
    // 쓰기는 저장소 key의 직렬 lane에 들어간다.
    REQUIRE(test.dispatcher.pending() == 1);
    CHECK(test.dispatcher.peek(0).interactive == false);
    CHECK(test.dispatcher.peek(0).repository_key == test.repository);

    REQUIRE(test.dispatcher.run());
    std::vector<operation_event> events { test.events.take() };
    REQUIRE(events.size() >= 2);
    test.workspace.apply(events.front());
    CHECK(test.status(id).state == operation_state::running);
    for (std::size_t index { 1 }; index < events.size(); ++index)
        test.workspace.apply(events[index]);

    const operation_status& finished { test.status(id) };
    CHECK(finished.state == operation_state::succeeded);
    REQUIRE(finished.commands.size() == 1);
    CHECK(finished.commands[0].command_line == u8"git --literal-pathspecs add -- a.txt");
    CHECK(finished.commands[0].exit_code == 0);
    CHECK_FALSE(finished.cancellable);

    // 쓰기가 끝나면 읽기 lane에 자동 refresh가 선다.
    REQUIRE(test.dispatcher.pending() == 1);
    CHECK(test.dispatcher.peek(0).interactive);
    const operation_id refresh { test.workspace.operation_order().back() };
    CHECK(test.status(refresh).kind == operation_kind::refresh);
    CHECK(test.status(refresh).automatic);
    test.settle();
    CHECK(test.session().loaded());
    CHECK(test.session().graph_rows().size() == 3);
}

TEST_CASE("a second write on the same repository waits behind the first", "[workspace][state]")
{
    workspace_harness test {};
    const operation_id first { test.workspace.submit(test.repository, fetch_request {}) };
    const operation_id second { test.workspace.submit(test.repository, push_execute_request {}) };
    CHECK_FALSE(test.status(first).queued_behind.has_value());
    CHECK(test.status(second).queued_behind == first);
    // 읽기는 쓰기 뒤에 줄 서지 않는다.
    const operation_id read { test.workspace.submit(test.repository, show_commit_request { test::sha_of(1) }) };
    CHECK_FALSE(test.status(read).queued_behind.has_value());
}

TEST_CASE("refresh results from an older generation are dropped", "[workspace][generation]")
{
    workspace_harness test {};
    test.script_history(4, true);

    const operation_id older { test.workspace.submit(test.repository, refresh_request {}) };
    const operation_id newer { test.workspace.submit(test.repository, refresh_request {}) };
    CHECK(test.session().requested_generation() == 2);

    // 앞선 refresh가 늦게 끝났다. snapshot과 log 묶음이 모두 버려진다.
    REQUIRE(test.dispatcher.run(0));
    test.apply_events();
    CHECK(test.status(older).state == operation_state::succeeded);
    CHECK_FALSE(test.session().loaded());
    CHECK(test.session().graph_rows().empty());

    REQUIRE(test.dispatcher.run(0));
    test.apply_events();
    CHECK(test.status(newer).state == operation_state::succeeded);
    CHECK(test.session().displayed_generation() == 2);
    // 더러운 작업 트리라 WIP 행이 첫 쪽 앞에 붙는다.
    CHECK(test.session().has_wip_row());
    CHECK(test.session().graph_rows().size() == 5);
    CHECK(test.session().graph_commits().front().sha == graph_wip_sha);
    CHECK(test.session().loaded_commit_count() == 4);
    // 2개씩 두 묶음과, 쪽이 끝났음을 알리는 마지막 묶음이다.
    CHECK(test.session().commit_batch_count() == 3);
    CHECK(test.session().history_complete());
}

TEST_CASE("log batches of a replaced layout are dropped mid-stream", "[workspace][generation]")
{
    workspace_harness test {};
    test.script_history(4, false);
    const operation_id first { test.workspace.submit(test.repository, refresh_request {}) };
    REQUIRE(test.dispatcher.run(0));
    std::vector<operation_event> events { test.events.take() };

    // snapshot까지만 반영한 뒤 새 refresh를 낸다. 남은 묶음은 옛 generation이다.
    std::size_t index { 0 };
    for (; index < events.size(); ++index)
    {
        test.workspace.apply(events[index]);
        if (std::holds_alternative<snapshot_loaded>(events[index].payload))
            break;
    }
    REQUIRE(test.session().loaded());
    static_cast<void>(test.workspace.submit(test.repository, refresh_request {}));
    for (++index; index < events.size(); ++index)
        test.workspace.apply(events[index]);
    CHECK(test.session().graph_rows().empty());
    CHECK(test.status(first).state == operation_state::succeeded);

    test.settle();
    CHECK(test.session().graph_rows().size() == 4);
}

TEST_CASE("refresh keeps a coherent graph until the replacement page is ready", "[workspace][generation]")
{
    workspace_harness test {};
    test.script_history(4, false);
    REQUIRE(test.workspace.submit(test.repository, refresh_request {}) != 0);
    test.settle();
    repository_session& session { test.session() };
    const auto displayed { session.displayed_generation() };
    const auto old_head { session.snapshot().summary.head_sha };
    repository_snapshot fresh { session.snapshot() };
    fresh.summary.current_branch = "feature/hidden";
    fresh.summary.head_sha = test::sha_of(9);
    fresh.summary.changed_file_count = 1;
    const auto generation { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { generation, fresh }));
    CHECK(session.displayed_generation() == displayed);
    CHECK(session.snapshot().summary.head_sha == old_head);
    CHECK(session.graph_rows().size() == 4);
    CHECK_FALSE(session.has_wip_row());

    commits_loaded batch {};
    batch.generation = generation;
    commit_info commit {};
    commit.sha = fresh.summary.head_sha;
    batch.commits.push_back(commit);
    REQUIRE(session.apply(batch));
    CHECK(session.graph_rows().size() == 4);
    CHECK(session.snapshot().summary.current_branch == "main");
    CHECK(session.displayed_generation() == displayed);

    SECTION("the complete page replaces snapshot and graph together")
    {
        batch.first_index = 1;
        batch.commits.clear();
        batch.page_complete = true;
        batch.history_complete = true;
        REQUIRE(session.apply(batch));
        CHECK(session.displayed_generation() == generation);
        CHECK(session.snapshot().summary.current_branch == "feature/hidden");
        REQUIRE(session.graph_rows().size() == 2);
        CHECK(session.graph_commits().front().sha == graph_wip_sha);
        CHECK(session.graph_commits().back().sha == fresh.summary.head_sha);
        CHECK(session.has_wip_row());
        CHECK(session.commit_batch_count() == 2);
        CHECK_FALSE(session.log_page_loading());
    }
    SECTION("a failed refresh retains the displayed graph")
    {
        session.abandon_log_page(generation);
        CHECK(session.graph_rows().size() == 4);
        CHECK(session.snapshot().summary.head_sha == old_head);
        CHECK(session.displayed_generation() == displayed);
        CHECK_FALSE(session.log_page_loading());
        batch.first_index = 1;
        batch.commits.clear();
        batch.page_complete = true;
        CHECK_FALSE(session.apply(batch));
    }
    SECTION("a newer refresh discards the pending page")
    {
        const auto newer { session.begin_refresh() };
        batch.first_index = 1;
        batch.commits.clear();
        batch.page_complete = true;
        CHECK_FALSE(session.apply(batch));
        fresh.summary.changed_file_count = 0;
        fresh.summary.current_branch = "empty";
        fresh.summary.head_sha.clear();
        fresh.refs.clear();
        REQUIRE(session.apply(snapshot_loaded { newer, fresh }));
        CHECK(session.graph_rows().size() == 4);
        batch.generation = newer;
        batch.first_index = 0;
        batch.history_complete = true;
        REQUIRE(session.apply(batch));
        CHECK(session.graph_rows().empty());
        CHECK(session.snapshot().summary.current_branch == "empty");
        CHECK(session.displayed_generation() == newer);
    }
}

TEST_CASE("refresh rereads the previously loaded range", "[workspace][log]")
{
    workspace_harness test { 3 };
    test.script_history(3, false);
    REQUIRE(test.workspace.submit(test.repository, refresh_request {}) != 0);
    test.settle();
    REQUIRE(test.workspace.submit(test.repository, load_more_log_request {}) != 0);
    test.settle();
    REQUIRE(test.session().loaded_commit_count() == 6);
    REQUIRE(test.workspace.submit(test.repository, refresh_request {}) != 0);
    test.settle();
    const auto& calls { test.process.calls() };
    const auto log { std::find_if(calls.rbegin(), calls.rend(), [](const test::recorded_call& call) { return call.arguments.front() == "log"; }) };
    REQUIRE(log != calls.rend());
    CHECK(std::find(log->arguments.begin(), log->arguments.end(), "--max-count=6") != log->arguments.end());
}

TEST_CASE("load_more_log continues the same generation from the loaded count", "[workspace][log]")
{
    // 쪽 크기가 3이라 첫 쪽이 꽉 차면 이력이 더 있다고 본다.
    workspace_harness test { 3 };
    test.script_history(3, false);
    // 첫 쪽이 오기 전에는 이어 읽을 것이 없다.
    CHECK(test.workspace.submit(test.repository, load_more_log_request {}) == 0);
    static_cast<void>(test.workspace.submit(test.repository, refresh_request {}));
    test.settle();
    REQUIRE(test.session().loaded_commit_count() == 3);
    REQUIRE_FALSE(test.session().history_complete());

    const operation_id more { test.workspace.submit(test.repository, load_more_log_request {}) };
    REQUIRE(more != 0);
    CHECK(test.session().log_page_loading());
    // 읽는 중에 또 내면 같은 쪽을 두 번 읽고 둘째 묶음이 버려져 읽는 중 표시가 풀리지 않는다.
    CHECK(test.workspace.submit(test.repository, load_more_log_request {}) == 0);
    test.settle();
    CHECK(test.status(more).state == operation_state::succeeded);
    CHECK_FALSE(test.session().log_page_loading());
    CHECK(test.session().loaded_commit_count() == 6);
    const std::vector<test::recorded_call> calls { test.process.calls() };
    CHECK(std::find(calls.back().arguments.begin(), calls.back().arguments.end(), "--skip=3") != calls.back().arguments.end());
}

TEST_CASE("cancelling a queued operation ends it without running git", "[workspace][cancel]")
{
    workspace_harness test {};
    const operation_id id { test.workspace.submit(test.repository, fetch_request {}) };
    REQUIRE(test.workspace.cancel(id));
    CHECK(test.status(id).state == operation_state::cancelled);
    CHECK(test.status(id).cancel_requested);

    REQUIRE(test.dispatcher.run());
    test.apply_events();
    CHECK(test.status(id).state == operation_state::cancelled);
    CHECK(test.process.calls().empty());
    // 실행되지 않은 쓰기는 저장소를 바꾸지 않았으므로 refresh도 내지 않는다.
    CHECK(test.dispatcher.pending() == 0);
    CHECK_FALSE(test.workspace.cancel(id));
}

TEST_CASE("a conflict waits for a decision and can only be left through abort", "[workspace][conflict]")
{
    workspace_harness test {};
    test.process.when({ "rebase", "--", "main", "feature" }, scripted_response { 1, "CONFLICT (content): Merge conflict in a.txt\n", { "error: could not apply 1234567... feat" } });
    test.process.when({ "status" }, scripted_response { 0, test::status_header(test::sha_of(1), "(detached)") + test::status_conflict("a.txt") });
    test.files.put(u8"C:/work/repo/.git/rebase-merge/msgnum", "1\n");

    const operation_id rebase { test.workspace.submit(test.repository, rebase_request { "feature", "main" }) };
    REQUIRE(test.dispatcher.run());
    test.apply_events();

    const operation_status& waiting { test.status(rebase) };
    REQUIRE(waiting.state == operation_state::needs_input);
    REQUIRE(waiting.needs_input.has_value());
    CHECK(waiting.needs_input->kind == input_kind::conflict);
    CHECK(waiting.finished());
    CHECK_FALSE(waiting.cancellable);
    CHECK_FALSE(test.workspace.cancel(rebase));
    CHECK(test.session().awaiting_input() == rebase);

    const operation_id abort { test.workspace.submit(test.repository, sequence_control_request { operation_kind::rebase_abort }) };
    CHECK(test.status(abort).kind == operation_kind::rebase_abort);
    CHECK_FALSE(test.session().awaiting_input().has_value());
}

TEST_CASE("a newer commit selection drops the older detail", "[workspace][generation]")
{
    workspace_harness test {};
    const text_list info { "log", "-1", "-z", "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%ct%x1f%s%x1f%b" };
    test.process.when_once(info, scripted_response { 0, test::log_record(test::sha_of(1), {}, "first", true) });
    test.process.when_once(info, scripted_response { 0, test::log_record(test::sha_of(2), { test::sha_of(1) }, "second", true) });
    const operation_id older { test.workspace.submit(test.repository, show_commit_request { test::sha_of(1) }) };
    const operation_id newer { test.workspace.submit(test.repository, show_commit_request { test::sha_of(2) }) };

    REQUIRE(test.dispatcher.run(0));
    test.apply_events();
    CHECK(test.status(older).state == operation_state::succeeded);
    CHECK_FALSE(test.session().selected_commit().has_value());

    REQUIRE(test.dispatcher.run(0));
    test.apply_events();
    CHECK(test.status(newer).state == operation_state::succeeded);
    REQUIRE(test.session().selected_commit().has_value());
    CHECK(test.session().selected_commit()->commit.subject == "second");
}

TEST_CASE("events of unknown operations are ignored", "[workspace][generation]")
{
    workspace_harness test {};
    const std::uint64_t before { test.workspace.revision() };
    test.workspace.apply(operation_event { 999, test.repository, operation_finished {} });
    CHECK(test.workspace.revision() == before);
}

TEST_CASE("fetch refreshes the remote tag state after it succeeds", "[workspace][tags]")
{
    workspace_harness test {};
    test.process.when({ "status" }, scripted_response { 0, test::status_header(test::sha_of(1), "main") });
    test.process.when({ "remote", "-v" }, scripted_response { 0, "origin\tC:/remote.git (fetch)\norigin\tC:/remote.git (push)\n" });
    test.process.when({ "for-each-ref" }, scripted_response { 0, "refs/tags/v1.0\x1f" + test::sha_of(1) + "\x1f\x1f\x1f\x1f\x1f\x1f\n" });
    test.process.when({ "ls-remote", "--tags" }, scripted_response { 0, test::sha_of(1) + "\trefs/tags/v1.0\n" });

    static_cast<void>(test.workspace.submit(test.repository, fetch_request { "origin" }));
    test.settle();

    const text_list names { test.process.command_names() };
    CHECK(std::find(names.begin(), names.end(), "ls-remote") != names.end());
    const std::vector<ref>& refs { test.session().snapshot().refs };
    const auto tag { std::find_if(refs.begin(), refs.end(), [](const ref& entry) { return entry.name == "v1.0"; }) };
    REQUIRE(tag != refs.end());
    CHECK(tag->remotes == text_list { "origin" });
}

TEST_CASE("setup operations are keyed by the target path", "[workspace][setup]")
{
    workspace_harness test {};
    const operation_id clone { test.workspace.submit_setup(clone_request { "C:/remote.git", u8"C:/work/new" }) };
    REQUIRE(clone != 0);
    CHECK(test.status(clone).repository_id.empty());
    CHECK(test.status(clone).target_path == u8"C:/work/new");
    CHECK(test.dispatcher.peek(0).repository_key == "setup:c:/work/new");
    CHECK(test.workspace.submit(test.repository, clone_request {}) == 0);
    CHECK(test.workspace.submit_setup(fetch_request {}) == 0);

    test.settle();
    CHECK(test.status(clone).state == operation_state::succeeded);
    REQUIRE(test.process.calls().size() == 1);
    CHECK(test.process.calls()[0].working_directory == "C:/work");
}

TEST_CASE("the same path opens the same repository", "[workspace]")
{
    workspace_harness test {};
    CHECK(test.workspace.open_repository(u8"c:\\WORK\\repo\\") == test.repository);
    CHECK(test.workspace.repository_ids().size() == 1);
}

namespace {
    // 첫 refresh(HEAD `head`, 변경 `first_files`)를 반영해 읽힌 세션을 만든다. 이어지는 status 응답은 `when_once`로 쌓는다.
    void load_with_changes(workspace_harness& test, const std::string& head, const std::vector<std::string>& first_files)
    {
        std::string output { test::status_header(head, "main", "origin/main") };
        for (const std::string& path : first_files)
            output += test::status_modified(path);
        test.process.when_once({ "status" }, scripted_response { 0, output });
        test.process.when({ "log", "--topo-order" }, scripted_response { 0, test::log_record(head, {}) });
        REQUIRE(test.workspace.submit(test.repository, refresh_request {}) != 0);
        test.settle();
        REQUIRE(test.session().loaded());
    }

    std::size_t count_of(const workspace_harness& test, const operation_kind kind)
    {
        std::size_t count { 0 };
        for (const operation_id id : test.workspace.operation_order())
            if (test.status(id).kind == kind)
                ++count;
        return count;
    }
} // namespace

TEST_CASE("refresh_status updates the working tree without reading the log or rebuilding the graph", "[workspace][status]")
{
    workspace_harness test {};
    const std::string head { test::sha_of(1) };
    load_with_changes(test, head, { "a.txt" });
    const std::uint64_t generation { test.session().displayed_generation() };
    const std::size_t rows { test.session().graph_rows().size() };
    const text_list names_before { test.process.command_names() };
    const std::size_t log_calls_before { static_cast<std::size_t>(std::count(names_before.begin(), names_before.end(), "log")) };

    test.process.when_once({ "status" }, scripted_response { 0, test::status_header(head, "main", "origin/main") + test::status_modified("a.txt") + test::status_modified("b.txt") });
    const operation_id id { test.workspace.submit(test.repository, refresh_status_request {}, true) };
    REQUIRE(id != 0);
    CHECK(test.dispatcher.peek(0).interactive);
    test.settle();

    CHECK(test.status(id).state == operation_state::succeeded);
    CHECK(test.session().snapshot().summary.changed_file_count == 2);
    CHECK(test.session().snapshot().unstaged.size() == 2);
    // 그래프·generation은 그대로이고 log도 읽지 않았다.
    CHECK(test.session().displayed_generation() == generation);
    CHECK(test.session().requested_generation() == generation);
    CHECK(test.session().graph_rows().size() == rows);
    const text_list names { test.process.command_names() };
    CHECK(static_cast<std::size_t>(std::count(names.begin(), names.end(), "log")) == log_calls_before);
    CHECK(count_of(test, operation_kind::refresh) == 1);
    CHECK(static_cast<std::size_t>(std::count(names.begin(), names.end(), "for-each-ref")) == 1);
}

TEST_CASE("refresh_status asks for a full refresh when the WIP row would appear or vanish", "[workspace][status]")
{
    workspace_harness test {};
    const std::string head { test::sha_of(1) };
    load_with_changes(test, head, {});
    REQUIRE_FALSE(test.session().has_wip_row());

    // 깨끗하던 작업 트리에 파일이 생겼다. WIP 행을 끼워야 하니 전체 refresh가 이어진다.
    test.process.when_once({ "status" }, scripted_response { 0, test::status_header(head, "main", "origin/main") + test::status_modified("new.txt") });
    test.process.when_once({ "status" }, scripted_response { 0, test::status_header(head, "main", "origin/main") + test::status_modified("new.txt") });
    REQUIRE(test.workspace.submit(test.repository, refresh_status_request {}, true) != 0);
    test.settle();

    CHECK(count_of(test, operation_kind::refresh_status) == 1);
    CHECK(count_of(test, operation_kind::refresh) == 2);
    CHECK(test.session().has_wip_row());
    CHECK(test.session().snapshot().summary.changed_file_count == 1);

    // 변경이 모두 사라졌을 때도 같다.
    test.process.when_once({ "status" }, scripted_response { 0, test::status_header(head, "main", "origin/main") });
    test.process.when_once({ "status" }, scripted_response { 0, test::status_header(head, "main", "origin/main") });
    REQUIRE(test.workspace.submit(test.repository, refresh_status_request {}, true) != 0);
    test.settle();
    CHECK(count_of(test, operation_kind::refresh) == 3);
    CHECK_FALSE(test.session().has_wip_row());
}

TEST_CASE("refresh_status asks for a full refresh when HEAD moved", "[workspace][status]")
{
    workspace_harness test {};
    load_with_changes(test, test::sha_of(1), { "a.txt" });

    test.process.when_once({ "status" }, scripted_response { 0, test::status_header(test::sha_of(2), "main", "origin/main") + test::status_modified("a.txt") });
    test.process.when_once({ "status" }, scripted_response { 0, test::status_header(test::sha_of(2), "main", "origin/main") + test::status_modified("a.txt") });
    REQUIRE(test.workspace.submit(test.repository, refresh_status_request {}, true) != 0);
    test.settle();

    CHECK(count_of(test, operation_kind::refresh) == 2);
    CHECK(test.session().snapshot().summary.head_sha == test::sha_of(2));
}

TEST_CASE("refresh_status is not started before the first snapshot or while a full refresh runs", "[workspace][status]")
{
    workspace_harness test {};
    test.script_history(2, false);
    // 아직 읽지 않았다.
    CHECK(test.workspace.submit(test.repository, refresh_status_request {}, true) == 0);

    REQUIRE(test.workspace.submit(test.repository, refresh_request {}) != 0);
    test.settle();
    REQUIRE(test.session().loaded());
    CHECK(test.workspace.submit(test.repository, refresh_status_request {}, true) != 0);
    test.settle();

    // 전체 refresh를 내 둔 채 status를 내려 하면 그 refresh가 모든 것을 읽으므로 내지 않는다.
    REQUIRE(test.workspace.submit(test.repository, refresh_request {}) != 0);
    CHECK(test.workspace.submit(test.repository, refresh_status_request {}, true) == 0);
}

TEST_CASE("a status result that arrives after a newer full refresh is discarded", "[workspace][status]")
{
    workspace_harness test {};
    const std::string head { test::sha_of(1) };
    load_with_changes(test, head, { "a.txt" });

    test.process.when_once({ "status" }, scripted_response { 0, test::status_header(head, "main", "origin/main") + test::status_modified("a.txt") + test::status_modified("stale.txt") });
    const operation_id status_id { test.workspace.submit(test.repository, refresh_status_request {}, true) };
    REQUIRE(status_id != 0);
    // status job을 실행해 결과를 쥐고 있다가, 그 사이 전체 refresh가 나간 뒤에 반영한다.
    REQUIRE(test.dispatcher.run());
    const std::vector<operation_event> late { test.events.take() };
    test.process.when_once({ "status" }, scripted_response { 0, test::status_header(head, "main", "origin/main") + test::status_modified("a.txt") });
    REQUIRE(test.workspace.submit(test.repository, refresh_request {}) != 0);
    for (const operation_event& event : late)
        test.workspace.apply(event);
    test.settle();

    CHECK(test.session().snapshot().summary.changed_file_count == 1);
    CHECK(test.session().snapshot().unstaged.size() == 1);
}

TEST_CASE("activity_of reports unfinished writes and refreshes of that repository only", "[workspace][status]")
{
    workspace_harness test {};
    test.script_history(2, false);
    CHECK_FALSE(test.workspace.activity_of(test.repository).write_pending);
    CHECK_FALSE(test.workspace.activity_of(test.repository).refresh_pending);

    REQUIRE(test.workspace.submit(test.repository, refresh_request {}) != 0);
    CHECK(test.workspace.activity_of(test.repository).refresh_pending);
    CHECK_FALSE(test.workspace.activity_of(test.repository).write_pending);
    test.settle();
    CHECK_FALSE(test.workspace.activity_of(test.repository).refresh_pending);

    REQUIRE(test.workspace.submit(test.repository, stage_files_request { { "a.txt" } }) != 0);
    CHECK(test.workspace.activity_of(test.repository).write_pending);
    CHECK_FALSE(test.workspace.activity_of("some-other-repository").write_pending);
    // 읽기(상세·diff)는 쓰기도 새로 고침도 아니다.
    test.settle();
    REQUIRE(test.workspace.submit(test.repository, show_commit_request { test::sha_of(2) }) != 0);
    CHECK_FALSE(test.workspace.activity_of(test.repository).write_pending);
    CHECK_FALSE(test.workspace.activity_of(test.repository).refresh_pending);
}

TEST_CASE("refresh_status after a failed or cancelled full refresh asks for a full refresh again", "[workspace][status]")
{
    workspace_harness test {};
    const std::string head { test::sha_of(1) };
    load_with_changes(test, head, { "a.txt" });

    SECTION("the refresh failed")
    {
        test.process.when_once({ "status" }, scripted_response { 128, "", { "fatal: unable to read the index" } });
        const operation_id failed { test.workspace.submit(test.repository, refresh_request {}) };
        REQUIRE(failed != 0);
        test.settle();
        REQUIRE(test.status(failed).state == operation_state::failed);
    }
    SECTION("the refresh was cancelled")
    {
        const operation_id cancelled { test.workspace.submit(test.repository, refresh_request {}) };
        REQUIRE(cancelled != 0);
        REQUIRE(test.workspace.cancel(cancelled));
        test.settle();
        REQUIRE(test.status(cancelled).state == operation_state::cancelled);
    }
    // 화면은 첫 refresh의 것이고 낸 generation만 앞서 있다. 그대로 두면 status 새로 고침이 계속 거절된다.
    REQUIRE(test.session().requested_generation() != test.session().displayed_generation());

    test.process.when_once({ "status" }, scripted_response { 0, test::status_header(head, "main", "origin/main") + test::status_modified("a.txt") + test::status_modified("b.txt") });
    const operation_id retried { test.workspace.submit(test.repository, refresh_status_request {}, true) };
    REQUIRE(retried != 0);
    CHECK(test.status(retried).kind == operation_kind::refresh);
    CHECK(test.status(retried).automatic);
    // 올린 refresh가 도는 동안에는 그 refresh가 모든 것을 읽는다.
    CHECK(test.workspace.submit(test.repository, refresh_status_request {}, true) == 0);
    test.settle();
    CHECK(test.status(retried).state == operation_state::succeeded);
    CHECK(test.session().requested_generation() == test.session().displayed_generation());
    CHECK(test.session().snapshot().summary.changed_file_count == 2);
}

TEST_CASE("refresh_status raises a full refresh only once until a refresh succeeds", "[workspace][status]")
{
    workspace_harness test {};
    const std::string head { test::sha_of(1) };
    load_with_changes(test, head, { "a.txt" });

    // 깨진 index처럼 계속 실패하는 저장소다. watcher 사건마다 전체 refresh를 되풀이하지 않는다.
    const scripted_response broken { 128, "", { "fatal: unable to read the index" } };
    test.process.when_once({ "status" }, broken);
    test.process.when_once({ "status" }, broken);
    const operation_id failed { test.workspace.submit(test.repository, refresh_request {}) };
    REQUIRE(failed != 0);
    test.settle();
    REQUIRE(test.status(failed).state == operation_state::failed);

    const operation_id raised { test.workspace.submit(test.repository, refresh_status_request {}, true) };
    REQUIRE(raised != 0);
    CHECK(test.status(raised).kind == operation_kind::refresh);
    test.settle();
    REQUIRE(test.status(raised).state == operation_state::failed);
    CHECK(test.workspace.submit(test.repository, refresh_status_request {}, true) == 0);

    // 사용자가 낸 전체 refresh가 성공하면 다음 실패 뒤에 다시 한 번 올릴 수 있다.
    test.process.when_once({ "status" }, scripted_response { 0, test::status_header(head, "main", "origin/main") + test::status_modified("a.txt") });
    const operation_id manual { test.workspace.submit(test.repository, refresh_request {}) };
    REQUIRE(manual != 0);
    test.settle();
    REQUIRE(test.status(manual).state == operation_state::succeeded);
    test.process.when_once({ "status" }, broken);
    const operation_id failed_again { test.workspace.submit(test.repository, refresh_request {}) };
    REQUIRE(failed_again != 0);
    test.settle();
    REQUIRE(test.status(failed_again).state == operation_state::failed);
    const operation_id raised_again { test.workspace.submit(test.repository, refresh_status_request {}, true) };
    REQUIRE(raised_again != 0);
    CHECK(test.status(raised_again).kind == operation_kind::refresh);
}

TEST_CASE("closing a repository ends its unfinished operations as cancelled", "[workspace][close]")
{
    workspace_harness test {};
    const operation_id running { test.workspace.submit(test.repository, fetch_request {}) };
    REQUIRE(running != 0);
    test.workspace.apply(operation_event { running, test.repository, operation_started {} });
    const operation_id queued { test.workspace.submit(test.repository, push_execute_request {}) };
    REQUIRE(queued != 0);

    REQUIRE(test.workspace.close_repository(test.repository));
    CHECK(test.status(running).state == operation_state::cancelled);
    CHECK(test.status(running).cancel_requested);
    CHECK(test.status(queued).state == operation_state::cancelled);
    CHECK_FALSE(test.workspace.activity_of(test.repository).write_pending);

    // worker가 늦게 보낸 끝 사건은 버린다. 결과를 받을 세션이 없으니 refresh도 내지 않는다.
    const std::size_t recorded { test.workspace.operation_order().size() };
    test.workspace.apply(operation_event { running, test.repository, operation_finished { operation_state::succeeded, std::nullopt, std::nullopt, {} } });
    CHECK(test.status(running).state == operation_state::cancelled);
    CHECK(test.workspace.operation_order().size() == recorded);
    // 남은 job은 취소된 token을 보고 git 없이 빠진다.
    test.settle();
    CHECK(test.process.calls().empty());
}

TEST_CASE("a reopened repository does not receive events of operations from before it was closed", "[workspace][close]")
{
    workspace_harness test {};
    test.script_history(3, false);
    const operation_id old_refresh { test.workspace.submit(test.repository, refresh_request {}) };
    REQUIRE(old_refresh != 0);
    // 옛 refresh는 git을 다 돌렸지만 사건은 아직 relay에 있다.
    REQUIRE(test.dispatcher.run());
    const std::vector<operation_event> late { test.events.take() };
    REQUIRE(test.workspace.close_repository(test.repository));
    CHECK(test.status(old_refresh).state == operation_state::cancelled);

    // 같은 경로로 다시 연 세션은 id가 같고 generation도 1부터 다시 센다.
    REQUIRE(test.workspace.open_repository(u8"C:/work/repo") == test.repository);
    const operation_id fresh { test.workspace.submit(test.repository, refresh_request {}) };
    REQUIRE(fresh != 0);
    REQUIRE(test.session().requested_generation() == 1);
    for (const operation_event& event : late)
        test.workspace.apply(event);
    CHECK_FALSE(test.session().loaded());
    CHECK(test.session().log_page_loading());

    test.settle();
    CHECK(test.status(fresh).state == operation_state::succeeded);
    CHECK(test.session().graph_rows().size() == 3);
}

TEST_CASE("cancelling while walking the operation order keeps the order and trims later", "[workspace][cancel]")
{
    // 끝난 기록을 하나도 남기지 않는 상한이다. 충돌 대기만 지울 수 없어 맨 앞에 남는다.
    workspace_harness test { 10000, 0 };
    test.process.when({ "rebase", "--", "main", "feature" }, scripted_response { 1, "CONFLICT (content): Merge conflict in a.txt\n", { "error: could not apply 1234567... feat" } });
    test.process.when({ "status" }, scripted_response { 0, test::status_header(test::sha_of(1), "(detached)") + test::status_conflict("a.txt") });
    test.files.put(u8"C:/work/repo/.git/rebase-merge/msgnum", "1\n");
    const operation_id rebase { test.workspace.submit(test.repository, rebase_request { "feature", "main" }) };
    REQUIRE(test.dispatcher.run());
    test.apply_events();
    REQUIRE(test.status(rebase).state == operation_state::needs_input);

    const operation_id stage { test.workspace.submit(test.repository, stage_files_request { { "a.txt" } }) };
    const operation_id unstage { test.workspace.submit(test.repository, unstage_files_request { { "a.txt" } }) };
    const std::vector<operation_id> before { test.workspace.operation_order().begin(), test.workspace.operation_order().end() };
    REQUIRE(before.front() == rebase);
    REQUIRE(before.size() >= 3);

    // 탭 닫기처럼 작업 로그를 돌며 끝나지 않은 작업을 모두 취소한다. 돌던 반복자가 무효가 되면 안 된다.
    for (const operation_id id : test.workspace.operation_order())
    {
        if (test.status(id).finished() == false)
            CHECK(test.workspace.cancel(id));
    }
    const std::vector<operation_id> after_cancel { test.workspace.operation_order().begin(), test.workspace.operation_order().end() };
    CHECK(after_cancel == before);
    CHECK(test.status(stage).state == operation_state::cancelled);
    CHECK(test.status(unstage).state == operation_state::cancelled);

    // 미뤄 둔 정리는 다음 operation이 끝날 때 한다. 충돌 대기는 그대로 남는다.
    REQUIRE(test.workspace.submit(test.repository, show_commit_request { test::sha_of(1) }) != 0);
    test.settle();
    const std::vector<operation_id> after_trim { test.workspace.operation_order().begin(), test.workspace.operation_order().end() };
    CHECK(after_trim == std::vector<operation_id> { rebase });
    CHECK(test.status(rebase).state == operation_state::needs_input);
}

TEST_CASE("a write whose follow-up refresh is refused still asks for the remote tags", "[workspace][tags]")
{
    workspace_harness test { 10000, 0 };
    const operation_id fetch { test.workspace.submit(test.repository, fetch_request { "origin" }) };
    REQUIRE(fetch != 0);
    REQUIRE(test.dispatcher.run());
    // 앱이 닫히는 중이라 scheduler가 후속 작업을 받지 않는다. 거절된 refresh를 끝내며 한 정리가 fetch 기록을 지운다.
    test.accepting = false;
    test.apply_events();

    CHECK(test.workspace.find_operation(fetch) == nullptr);
    // 지워진 기록을 다시 읽지 않고 refresh와 원격 태그 읽기를 차례로 냈다.
    CHECK(test.refused == 2);
    CHECK(test.workspace.operation_order().empty());
}

TEST_CASE("cancelling a write the worker already picked up waits for it and refreshes afterwards", "[workspace][cancel]")
{
    workspace_harness test {};
    test.script_history(2, false);
    const operation_id stage { test.workspace.submit(test.repository, stage_files_request { { "a.txt" } }) };
    REQUIRE(stage != 0);
    // worker가 git을 돌렸지만 `operation_started`는 아직 relay에 있다.
    REQUIRE(test.dispatcher.run());
    REQUIRE(test.status(stage).state == operation_state::queued);

    REQUIRE(test.workspace.cancel(stage));
    CHECK_FALSE(test.status(stage).finished());
    CHECK(test.status(stage).cancel_requested);

    test.apply_events();
    CHECK(test.status(stage).finished());
    // git이 저장소를 바꿨을 수 있으므로 끝난 뒤 refresh가 이어진다.
    REQUIRE(test.dispatcher.pending() == 1);
    CHECK(test.status(test.workspace.operation_order().back()).kind == operation_kind::refresh);
}

TEST_CASE("pull refreshes the remote tag state after it succeeds", "[workspace][tags]")
{
    workspace_harness test {};
    test.process.when({ "status" }, scripted_response { 0, test::status_header(test::sha_of(1), "main", "origin/main") });
    test.process.when({ "remote", "-v" }, scripted_response { 0, "fork\tC:/fork.git (fetch)\nfork\tC:/fork.git (push)\norigin\tC:/remote.git (fetch)\norigin\tC:/remote.git (push)\n" });
    test.process.when({ "for-each-ref" }, scripted_response { 0, "refs/tags/v1.0\x1f" + test::sha_of(1) + "\x1f\x1f\x1f\x1f\x1f\x1f\n" });
    test.process.when({ "ls-remote", "--tags" }, scripted_response { 0, test::sha_of(1) + "\trefs/tags/v1.0\n" });
    REQUIRE(test.workspace.submit(test.repository, refresh_request {}) != 0);
    test.settle();
    REQUIRE(test.session().loaded());

    static_cast<void>(test.workspace.submit(test.repository, pull_request {}));
    test.settle();

    // pull이 받는 곳은 upstream 원격 하나다. 다른 원격(fork)에는 ls-remote를 보내지 않는다.
    text_list queried {};
    for (const test::recorded_call& call : test.process.calls())
    {
        if (std::ranges::find(call.arguments, "ls-remote") != call.arguments.end())
            queried.push_back(call.arguments.back());
    }
    CHECK(queried == text_list { "origin" });
    const std::vector<ref>& refs { test.session().snapshot().refs };
    const auto tag { std::find_if(refs.begin(), refs.end(), [](const ref& entry) { return entry.name == "v1.0"; }) };
    REQUIRE(tag != refs.end());
    CHECK(tag->remotes == text_list { "origin" });
}

TEST_CASE("an older ls-remote that finishes last does not overwrite the newer remote tags", "[workspace][tags]")
{
    workspace_harness test {};
    test.process.when({ "status" }, scripted_response { 0, test::status_header(test::sha_of(1), "main") });
    test.process.when({ "remote", "-v" }, scripted_response { 0, "origin\tC:/remote.git (fetch)\norigin\tC:/remote.git (push)\n" });
    test.process.when({ "for-each-ref" }, scripted_response { 0, "refs/tags/v1.0\x1f" + test::sha_of(1) + "\x1f\x1f\x1f\x1f\x1f\x1f\n" });
    REQUIRE(test.workspace.submit(test.repository, refresh_request {}) != 0);
    test.settle();
    REQUIRE(test.session().loaded());

    // 응답은 실행 순서로 쓰인다. 나중에 낸 것이 먼저 돌아 태그를 보고, 먼저 낸 것이 늦게 돌아 push 전의 빈 목록을 본다.
    test.process.when_once({ "ls-remote", "--tags" }, scripted_response { 0, test::sha_of(1) + "\trefs/tags/v1.0\n" });
    test.process.when_once({ "ls-remote", "--tags" }, scripted_response { 0, "" });
    const operation_id older { test.workspace.submit(test.repository, refresh_remote_tags_request { { "origin" } }) };
    const operation_id newer { test.workspace.submit(test.repository, refresh_remote_tags_request { { "origin" } }) };
    REQUIRE(test.dispatcher.pending() == 2);
    REQUIRE(test.dispatcher.run(1));
    REQUIRE(test.dispatcher.run(0));
    test.apply_events();
    CHECK(test.status(older).state == operation_state::succeeded);
    CHECK(test.status(newer).state == operation_state::succeeded);

    const std::vector<ref>& refs { test.session().snapshot().refs };
    const auto tag { std::find_if(refs.begin(), refs.end(), [](const ref& entry) { return entry.name == "v1.0"; }) };
    REQUIRE(tag != refs.end());
    CHECK(tag->remotes == text_list { "origin" });
}

TEST_CASE("blame_file and file_history read on the read lane and keep their result in the session", "[workspace][blame]")
{
    workspace_harness test {};
    test.process.when({ "blame", "--porcelain" }, scripted_response { 0, test::read_fixture("blame_renamed.bin") });
    test.process.when({ "--literal-pathspecs", "log", "--follow" }, scripted_response { 0, test::read_fixture("file_log_renamed.bin") });

    // lane을 빠뜨리면 기본값인 쓰기 lane으로 가서 저장소의 쓰기를 막고 끝난 뒤 refresh까지 낸다.
    CHECK(operation_kind_of(blame_file_request {}) == operation_kind::blame_file);
    CHECK(operation_kind_of(file_history_request {}) == operation_kind::file_history);
    CHECK(operation_lane_of(operation_kind::blame_file) == operation_lane::read);
    CHECK(operation_lane_of(operation_kind::file_history) == operation_lane::read);
    CHECK(operation_kind_name(operation_kind::blame_file) == "blame_file");
    CHECK(operation_kind_name(operation_kind::file_history) == "file_history");

    const blame_file_request blame_request { .path = "b[12].txt", .revision = "HEAD" };
    const operation_id blame { test.workspace.submit(test.repository, blame_request) };
    REQUIRE(blame != 0);
    REQUIRE(test.dispatcher.pending() == 1);
    CHECK(test.dispatcher.peek(0).interactive);
    CHECK(test.dispatcher.peek(0).repository_key.empty());
    CHECK_FALSE(test.session().selected_blame().has_value());
    test.settle();

    CHECK(test.status(blame).state == operation_state::succeeded);
    REQUIRE(test.session().selected_blame().has_value());
    CHECK(test.session().selected_blame()->request == blame_request);
    CHECK(test.session().selected_blame()->lines.size() == 8);
    CHECK(test.session().selected_blame()->origins.size() == 4);

    const file_history_request history_request { .path = "b[12].txt", .revision = {}, .max_count = 3 };
    const operation_id history { test.workspace.submit(test.repository, history_request) };
    REQUIRE(history != 0);
    REQUIRE(test.dispatcher.pending() == 1);
    CHECK(test.dispatcher.peek(0).interactive);
    test.settle();

    CHECK(test.status(history).state == operation_state::succeeded);
    REQUIRE(test.session().selected_history().has_value());
    CHECK(test.session().selected_history()->request == history_request);
    CHECK(test.session().selected_history()->entries.size() == 3);
    CHECK(test.session().selected_history()->truncated);
    // 파일 이력을 읽어도 blame 결과는 그대로다.
    CHECK(test.session().selected_blame().has_value());

    // 읽기는 저장소를 바꾸지 않는다. 뒤따르는 refresh가 없다.
    CHECK(test.process.command_names() == text_list { "blame", "log" });
    CHECK(test.workspace.operation_order().size() == 2);
}

TEST_CASE("a new blame_file clears the shown result and cancels the queued one before it", "[workspace][blame][cancel]")
{
    workspace_harness test {};
    test.process.when({ "blame", "--porcelain" }, scripted_response { 0, test::read_fixture("blame_renamed.bin") });
    const operation_id shown { test.workspace.submit(test.repository, blame_file_request { .path = "a.txt" }) };
    test.settle();
    REQUIRE(test.session().selected_blame().has_value());
    const std::uint64_t revision { test.session().revision() };

    // 낡은 내용이 새 요청의 것처럼 보이지 않게, 요청하는 순간 앞 결과를 비운다.
    const operation_id queued { test.workspace.submit(test.repository, blame_file_request { .path = "b.txt" }) };
    CHECK_FALSE(test.session().selected_blame().has_value());
    CHECK(test.session().revision() > revision);
    // 이미 끝난 앞 operation은 건드리지 않는다.
    CHECK(test.status(shown).state == operation_state::succeeded);
    CHECK_FALSE(test.status(shown).cancel_requested);

    // worker가 아직 집어 들지 않은 앞 요청은 바로 취소로 끝난다. git은 뜨지 않는다.
    const operation_id latest { test.workspace.submit(test.repository, blame_file_request { .path = "c.txt" }) };
    CHECK(test.status(queued).state == operation_state::cancelled);
    CHECK(test.status(queued).cancel_requested);
    CHECK_FALSE(test.status(queued).failure.has_value());
    CHECK_FALSE(test.status(latest).finished());
    test.settle();

    CHECK(test.status(latest).state == operation_state::succeeded);
    REQUIRE(test.session().selected_blame().has_value());
    CHECK(test.session().selected_blame()->request.path == "c.txt");
    text_list blamed {};
    for (const test::recorded_call& call : test.process.calls())
        blamed.push_back(call.arguments.back());
    CHECK(blamed == text_list { "a.txt", "c.txt" });
}

TEST_CASE("a new blame_file stops the git process of the one still running", "[workspace][blame][cancel]")
{
    workspace_harness test {};
    operation_id newer { 0 };
    // 앞 요청의 git이 도는 동안 사용자가 다른 파일을 고른다. 가짜 runner는 응답 직전에 취소를 보고 git이 죽은 것처럼 끝낸다.
    scripted_response interrupted { 0, test::read_fixture("blame_renamed.bin") };
    interrupted.before_reply = [&test, &newer] { newer = test.workspace.submit(test.repository, blame_file_request { .path = "new.txt" }); };
    test.process.when_once({ "blame", "--porcelain" }, interrupted);
    test.process.when({ "blame", "--porcelain" }, scripted_response { 0, test::read_fixture("blame_two_paths.bin") });

    const operation_id older { test.workspace.submit(test.repository, blame_file_request { .path = "old.txt" }) };
    REQUIRE(test.dispatcher.run());
    REQUIRE(newer != 0);
    // worker가 이미 집어 든 작업은 끝 사건을 기다린다.
    CHECK(test.status(older).cancel_requested);
    test.settle();

    // 취소는 실패가 아니다. 실패 알림을 만들 것이 없다.
    CHECK(test.status(older).state == operation_state::cancelled);
    CHECK_FALSE(test.status(older).failure.has_value());
    CHECK(test.status(newer).state == operation_state::succeeded);
    REQUIRE(test.session().selected_blame().has_value());
    CHECK(test.session().selected_blame()->request.path == "new.txt");
    CHECK(test.session().selected_blame()->lines.size() == 10);
}

TEST_CASE("the result of a superseded blame_file or file_history that still finishes is discarded", "[workspace][blame]")
{
    workspace_harness test {};
    test.process.when_once({ "blame", "--porcelain" }, scripted_response { 0, test::read_fixture("blame_renamed.bin") });
    test.process.when({ "blame", "--porcelain" }, scripted_response { 0, test::read_fixture("blame_two_paths.bin") });
    test.process.when({ "--literal-pathspecs", "log", "--follow" }, scripted_response { 0, test::read_fixture("file_log_renamed.bin") });

    // 앞 요청의 git이 이미 끝났고 사건만 relay에 남았다. 그 사이에 새 요청이 나간다.
    const operation_id older { test.workspace.submit(test.repository, blame_file_request { .path = "old.txt" }) };
    REQUIRE(test.dispatcher.run());
    const operation_id newer { test.workspace.submit(test.repository, blame_file_request { .path = "new.txt" }) };
    test.apply_events();
    CHECK(test.status(older).finished());
    // 늦게 온 앞 요청의 결과가 새 요청의 것처럼 보이면 안 된다.
    CHECK_FALSE(test.session().selected_blame().has_value());
    test.settle();
    REQUIRE(test.session().selected_blame().has_value());
    CHECK(test.session().selected_blame()->request.path == "new.txt");
    CHECK(test.status(newer).state == operation_state::succeeded);

    const operation_id older_history { test.workspace.submit(test.repository, file_history_request { .path = "old.txt" }) };
    REQUIRE(test.dispatcher.run());
    static_cast<void>(test.workspace.submit(test.repository, file_history_request { .path = "new.txt" }));
    test.apply_events();
    CHECK(test.status(older_history).finished());
    CHECK_FALSE(test.session().selected_history().has_value());
    test.settle();
    REQUIRE(test.session().selected_history().has_value());
    CHECK(test.session().selected_history()->request.path == "new.txt");
}

TEST_CASE("blame_file and file_history cancel only their own kind in their own repository", "[workspace][blame][cancel]")
{
    workspace_harness test {};
    const std::string other { test.workspace.open_repository(u8"C:/work/other") };

    const operation_id blame { test.workspace.submit(test.repository, blame_file_request { .path = "a.txt" }) };
    const operation_id diff { test.workspace.submit(test.repository, diff_file_request { .path = "a.txt" }) };
    const operation_id other_blame { test.workspace.submit(other, blame_file_request { .path = "a.txt" }) };
    const operation_id other_history { test.workspace.submit(other, file_history_request { .path = "a.txt" }) };

    const operation_id history { test.workspace.submit(test.repository, file_history_request { .path = "a.txt" }) };
    CHECK_FALSE(test.status(blame).finished());
    CHECK_FALSE(test.status(diff).finished());
    CHECK_FALSE(test.status(other_blame).finished());
    CHECK_FALSE(test.status(other_history).finished());

    const operation_id next_history { test.workspace.submit(test.repository, file_history_request { .path = "b.txt" }) };
    CHECK(test.status(history).state == operation_state::cancelled);
    CHECK_FALSE(test.status(blame).finished());
    CHECK_FALSE(test.status(other_history).finished());
    CHECK_FALSE(test.status(next_history).finished());

    const operation_id next_blame { test.workspace.submit(other, blame_file_request { .path = "b.txt" }) };
    CHECK(test.status(other_blame).state == operation_state::cancelled);
    CHECK_FALSE(test.status(blame).finished());
    CHECK_FALSE(test.status(diff).finished());
    CHECK_FALSE(test.status(next_blame).finished());
}

TEST_CASE("the session accepts only the blame and file history it expects", "[session][blame]")
{
    repository_session session { "repo", u8"C:/work/repo" };
    file_blame_loaded blame {};
    blame.blame.request.path = "a.txt";
    file_history_loaded history {};
    history.history.request.path = "a.txt";

    session.expect_file_blame(7);
    CHECK_FALSE(session.apply(6, blame));
    CHECK_FALSE(session.selected_blame().has_value());
    const std::uint64_t before { session.revision() };
    CHECK(session.apply(7, blame));
    CHECK(session.revision() > before);
    REQUIRE(session.selected_blame().has_value());
    CHECK(session.selected_blame()->request.path == "a.txt");

    session.expect_file_history(9);
    CHECK_FALSE(session.apply(8, history));
    CHECK_FALSE(session.selected_history().has_value());
    CHECK(session.apply(9, history));
    REQUIRE(session.selected_history().has_value());
    // blame과 파일 이력은 서로의 기대 값을 건드리지 않는다.
    CHECK(session.selected_blame().has_value());

    // 새로 기대하면 앞 결과를 비우고, 앞 요청의 늦은 결과는 버린다.
    session.expect_file_blame(11);
    CHECK_FALSE(session.selected_blame().has_value());
    CHECK_FALSE(session.apply(7, blame));
    CHECK_FALSE(session.selected_blame().has_value());
    session.expect_file_history(12);
    CHECK_FALSE(session.selected_history().has_value());
    CHECK_FALSE(session.apply(9, history));
    CHECK_FALSE(session.selected_history().has_value());
}
