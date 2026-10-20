// 저장소 세션과 operation 표(git_workspace)를 가짜 runner와 수동 dispatcher로 돌린다. job을 언제
// 실행할지 test가 정하므로 대기·취소·늦게 온 결과를 결정적으로 만든다.
#include "application/git_workspace.h"
#include "infrastructure/git_operation_runner.h"

#include "fake_git_runner.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {
    using namespace cyella;
    using cyella::test::scripted_response;
    using cyella::test::text_list;

    struct workspace_harness
    {
        explicit workspace_harness(const std::size_t log_page_size = 10000)
            : runner { process, u8"C:/Git/cmd/git.exe", files, make_options(log_page_size) }
            , workspace { runner, dispatcher.dispatcher(), events.sink(), nullptr, make_workspace_options() }
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

        static git_workspace_options make_workspace_options()
        {
            git_workspace_options options {};
            options.refresh_on_open = false;
            return options;
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
