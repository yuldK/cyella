// 실행기(git_operation_runner)를 가짜 runner로 돌려 명령 순서, 사건, 끝맺음을 본다. git과
// 디스크가 필요 없다.
#include "domain/text_catalog.h"
#include "infrastructure/git_operation_runner.h"

#include "fake_git_runner.h"
#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
    using namespace cyella;
    using cyella::test::fake_git_runner;
    using cyella::test::fake_workspace_files;
    using cyella::test::scripted_response;
    using cyella::test::text_list;

    constexpr std::u8string_view repository_path { u8"C:/work/repo" };
    constexpr std::string_view git_dir { "C:/work/repo/.git" };

    struct harness
    {
        explicit harness(git_operation_runner_options options = {}, const bool answer_git_dir = true)
            : runner { process, u8"C:/Git/cmd/git.exe", files, options }
        {
            // 대부분의 작업이 먼저 git 디렉터리를 묻는다.
            if (answer_git_dir)
                process.when({ "rev-parse", "--absolute-git-dir" }, scripted_response { 0, std::string { git_dir } + "\nC:/work/repo\n" });
        }

        std::vector<operation_event> execute(git_operation operation, const std::uint64_t generation = 1)
        {
            git_operation_job job {};
            job.id = 7;
            job.repository_id = "repo";
            job.repository_path = std::u8string { repository_path };
            job.operation = std::move(operation);
            job.generation = generation;
            job.graph_filter = graph_filter;
            job.current_branch_hint = current_branch;
            job.log_include_head = include_head;
            std::vector<operation_event> events {};
            runner.execute(job, [&events](operation_event event) { events.push_back(std::move(event)); });
            return events;
        }

        fake_git_runner process {};
        fake_workspace_files files {};
        git_operation_runner runner;
        // job에 실어 보낼 값이다. workspace가 접수할 때 세션에서 채우는 것들이다.
        graph_ref_filter graph_filter {};
        std::string current_branch {};
        bool include_head { true };
    };

    template<typename payload_type>
    std::vector<payload_type> payloads(const std::vector<operation_event>& events)
    {
        std::vector<payload_type> found {};
        for (const operation_event& event : events)
            if (const auto* const payload { std::get_if<payload_type>(&event.payload) })
                found.push_back(*payload);
        return found;
    }

    operation_finished finished_of(const std::vector<operation_event>& events)
    {
        REQUIRE(events.empty() == false);
        const auto* const finished { std::get_if<operation_finished>(&events.back().payload) };
        REQUIRE(finished != nullptr);
        return *finished;
    }

    std::string join(const text_list& arguments)
    {
        std::string text {};
        for (const std::string& argument : arguments)
        {
            if (text.empty() == false)
                text.push_back(' ');
            text.append(argument);
        }
        return text;
    }
} // namespace

TEST_CASE("refresh reads status before the log and streams commits in batches", "[operation][refresh]")
{
    git_operation_runner_options options {};
    options.log_batch_size = 2;
    options.log_flush_interval = std::chrono::hours { 1 };
    harness test { options };

    const std::string head { test::sha_of(5) };
    test.process.when({ "status" }, scripted_response { 0, test::status_header(head, "main", "origin/main") + test::status_modified("a.txt") });
    test.process.when({ "remote", "-v" }, scripted_response { 0, "origin\tC:/remote.git (fetch)\norigin\tC:/remote.git (push)\n" });
    test.process.when({ "merge-base", "--is-ancestor" }, scripted_response { 1 });
    std::string log {};
    for (int index { 5 }; index >= 1; --index)
        log.append(test::log_record(test::sha_of(index), index == 1 ? text_list {} : text_list { test::sha_of(index - 1) }));
    test.process.when({ "log", "--topo-order" }, scripted_response { 0, log });

    const std::vector<operation_event> events { test.execute(refresh_request {}, 3) };

    // HEAD를 알아야 WIP 행을 끼우므로 status·refs가 log보다 먼저다.
    CHECK(test.process.command_names() == text_list { "rev-parse", "status", "diff", "diff", "for-each-ref", "stash", "remote", "merge-base", "worktree", "config", "log" });
    for (const test::recorded_call& call : test.process.calls())
        CHECK(call.no_optional_locks);

    std::size_t snapshot_index { events.size() };
    std::size_t first_batch_index { events.size() };
    for (std::size_t index { 0 }; index < events.size(); ++index)
    {
        if (std::holds_alternative<snapshot_loaded>(events[index].payload) && snapshot_index == events.size())
            snapshot_index = index;
        if (std::holds_alternative<commits_loaded>(events[index].payload) && first_batch_index == events.size())
            first_batch_index = index;
    }
    CHECK(snapshot_index < first_batch_index);

    const std::vector<snapshot_loaded> snapshots { payloads<snapshot_loaded>(events) };
    REQUIRE(snapshots.size() == 1);
    CHECK(snapshots[0].generation == 3);
    CHECK(snapshots[0].snapshot.summary.head_sha == head);
    CHECK(snapshots[0].snapshot.summary.changed_file_count == 1);
    CHECK(snapshots[0].snapshot.summary.git_dir == git_dir);
    CHECK(snapshots[0].snapshot.summary.display_name == "repo");
    CHECK(snapshots[0].snapshot.summary.remotes.size() == 1);
    CHECK_FALSE(snapshots[0].snapshot.summary.head_pushed);

    const std::vector<commits_loaded> batches { payloads<commits_loaded>(events) };
    REQUIRE(batches.size() == 3);
    std::size_t next { 0 };
    for (const commits_loaded& batch : batches)
    {
        CHECK(batch.generation == 3);
        CHECK(batch.first_index == next);
        next += batch.commits.size();
    }
    CHECK(next == 5);
    CHECK(batches.back().page_complete);
    CHECK(batches.back().history_complete);
    CHECK(finished_of(events).state == operation_state::succeeded);
    CHECK(payloads<operation_command_finished>(events).size() == test.process.calls().size());
}

namespace {
    // `for-each-ref` 출력의 한 줄이다 (필드 여덟, 0x1F로 가른다). `current`면 HEAD가 가리키는 브랜치다.
    std::string ref_line(const std::string& full_name, const std::string& sha, const bool current = false)
    {
        return full_name + "\x1f" + sha + "\x1f\x1f" + (current ? "*" : " ") + "\x1f\x1f\x1f\x1f\n";
    }

    // 그래프 log(`log --topo-order ...`)의 형식 뒤, `--max-count` 앞까지가 그래프의 ref 집합이다.
    text_list graph_refs_of(const test::recorded_call& call)
    {
        const auto last { std::find_if(call.arguments.begin(), call.arguments.end(), [](const std::string& argument) { return argument.starts_with("--max-count="); }) };
        REQUIRE(call.arguments.size() > 4);
        REQUIRE(last != call.arguments.end());
        return text_list { call.arguments.begin() + 4, last };
    }

    std::vector<test::recorded_call> calls_named(const fake_git_runner& process, const std::string& name)
    {
        std::vector<test::recorded_call> found {};
        for (const test::recorded_call& call : process.calls())
        {
            if (call.arguments.empty() == false && call.arguments.front() == name)
                found.push_back(call);
        }
        return found;
    }

    // main(현재 브랜치)과 feature, 원격 추적 브랜치, 태그가 있는 저장소의 refresh를 꾸민다.
    void script_filtered_refresh(harness& test)
    {
        const std::string head { test::sha_of(3) };
        test.process.when({ "status" }, scripted_response { 0, test::status_header(head, "main") + test::status_modified("a.txt") });
        std::string refs { ref_line("refs/heads/main", head, true) + ref_line("refs/heads/feature", test::sha_of(2)) };
        refs.append(ref_line("refs/remotes/origin/feature", test::sha_of(2))).append(ref_line("refs/tags/v1.0", test::sha_of(1)));
        test.process.when({ "for-each-ref" }, scripted_response { 0, refs });
        test.process.when({ "log", "--topo-order" }, scripted_response { 0, test::log_record(test::sha_of(2), { test::sha_of(1) }) + test::log_record(test::sha_of(1), {}) });
    }
} // namespace

TEST_CASE("refresh reads the log from only the refs of the filter that still exist", "[operation][refresh][graph_filter]")
{
    harness test {};
    script_filtered_refresh(test);
    // 접수한 뒤 지워진 브랜치가 필터에 남아 있다. 그대로 주면 `log refs/heads/gone`이 실패해 그래프 전체가 읽히지 않는다.
    test.graph_filter.only = { "refs/heads/gone", "refs/heads/feature", "refs/tags/v1.0" };
    // 접수 때의 현재 브랜치는 낡았을 수 있다. 실행기는 방금 읽은 status를 쓴다.
    test.current_branch = "stale";
    // HEAD(3번 커밋)는 feature와 v1.0에서 닿지 않는다: 닿지 않으면 git이 그 SHA를 낸다.
    test.process.when({ "rev-list", "--max-count=1" }, scripted_response { 0, test::sha_of(3) + "\n" });

    const std::vector<operation_event> events { test.execute(refresh_request {}) };

    CHECK(finished_of(events).state == operation_state::succeeded);
    const std::vector<test::recorded_call> logs { calls_named(test.process, "log") };
    REQUIRE(logs.size() == 1);
    CHECK(graph_refs_of(logs[0]) == text_list { "refs/heads/feature", "refs/tags/v1.0" });
    // HEAD가 그 ref들에서 닿는지 같은 집합으로 묻는다. snapshot이 log보다 먼저 가야 하므로 log 앞이다.
    const std::vector<test::recorded_call> reachability { calls_named(test.process, "rev-list") };
    REQUIRE(reachability.size() == 1);
    CHECK(reachability[0].arguments == text_list { "rev-list", "--max-count=1", test::sha_of(3), "--not", "refs/heads/feature", "refs/tags/v1.0", "--" });
    const text_list names { test.process.command_names() };
    CHECK(std::find(names.begin(), names.end(), "rev-list") < std::find(names.begin(), names.end(), "log"));

    const std::vector<snapshot_loaded> snapshots { payloads<snapshot_loaded>(events) };
    REQUIRE(snapshots.size() == 1);
    CHECK_FALSE(snapshots[0].snapshot.head_in_graph);
    CHECK(snapshots[0].snapshot.summary.changed_file_count == 1);
    CHECK(payloads<commits_loaded>(events).back().history_complete);
}

TEST_CASE("refresh keeps HEAD in the graph when the shown refs reach it or git cannot tell", "[operation][refresh][graph_filter]")
{
    SECTION("the shown ref reaches HEAD")
    {
        harness test {};
        script_filtered_refresh(test);
        test.graph_filter.only = { "refs/heads/main" };
        // 닿으면 출력이 빈다 (가짜 runner의 기본 응답이다).
        const std::vector<operation_event> events { test.execute(refresh_request {}) };

        CHECK(finished_of(events).state == operation_state::succeeded);
        CHECK(calls_named(test.process, "rev-list").size() == 1);
        REQUIRE(payloads<snapshot_loaded>(events).size() == 1);
        CHECK(payloads<snapshot_loaded>(events)[0].snapshot.head_in_graph);
    }
    SECTION("git fails to answer")
    {
        harness test {};
        script_filtered_refresh(test);
        test.graph_filter.only = { "refs/heads/feature" };
        test.process.when({ "rev-list", "--max-count=1" }, scripted_response { 128, "", { "fatal: bad object" } });
        const std::vector<operation_event> events { test.execute(refresh_request {}) };

        // 모르면 있다고 본다. refresh는 이 물음 때문에 실패하지 않는다.
        CHECK(finished_of(events).state == operation_state::succeeded);
        REQUIRE(payloads<snapshot_loaded>(events).size() == 1);
        CHECK(payloads<snapshot_loaded>(events)[0].snapshot.head_in_graph);
        CHECK(calls_named(test.process, "log").size() == 1);
    }
    SECTION("no ref is shown alone")
    {
        harness test {};
        script_filtered_refresh(test);
        test.graph_filter.hidden = { "refs/heads/feature" };
        const std::vector<operation_event> events { test.execute(refresh_request {}) };

        // 숨기기는 HEAD를 시작점에 남긴다. 물을 것이 없다.
        CHECK(calls_named(test.process, "rev-list").empty());
        REQUIRE(payloads<snapshot_loaded>(events).size() == 1);
        CHECK(payloads<snapshot_loaded>(events)[0].snapshot.head_in_graph);
    }
}

TEST_CASE("refresh falls back to every ref when all the shown refs are gone", "[operation][refresh][graph_filter]")
{
    harness test {};
    script_filtered_refresh(test);
    test.graph_filter.only = { "refs/heads/gone", "refs/tags/v0.9" };

    const std::vector<operation_event> events { test.execute(refresh_request {}) };

    CHECK(finished_of(events).state == operation_state::succeeded);
    const std::vector<test::recorded_call> logs { calls_named(test.process, "log") };
    REQUIRE(logs.size() == 1);
    CHECK(graph_refs_of(logs[0]) == text_list { "--branches", "--remotes", "--tags", "HEAD" });
    CHECK(calls_named(test.process, "rev-list").empty());
}

TEST_CASE("refresh excludes the hidden refs that exist but never the branch that status reports as current", "[operation][refresh][graph_filter]")
{
    harness test {};
    script_filtered_refresh(test);
    // main은 방금 읽은 status의 현재 브랜치다. 접수 때는 다른 브랜치에 있었다(숨긴 브랜치로 옮겨 간 직후의 refresh).
    test.current_branch = "feature";
    test.graph_filter.hidden = { "refs/heads/main", "refs/heads/gone", "refs/heads/feature", "refs/remotes/origin/feature", "refs/tags/v1.0", "refs/tags/gone" };

    const std::vector<operation_event> events { test.execute(refresh_request {}) };

    CHECK(finished_of(events).state == operation_state::succeeded);
    const std::vector<test::recorded_call> logs { calls_named(test.process, "log") };
    REQUIRE(logs.size() == 1);
    CHECK(graph_refs_of(logs[0]) == text_list { "--exclude=feature", "--branches", "--exclude=origin/feature", "--exclude=*/HEAD", "--remotes", "--exclude=v1.0", "--tags", "HEAD" });
}

TEST_CASE("load_more_log and search_commits read the refs of the filter the job carries", "[operation][log][commit_search][graph_filter]")
{
    harness test {};
    test.current_branch = "main";
    test.graph_filter.hidden = { "refs/heads/main", "refs/heads/feature", "refs/tags/v1.0" };
    test.process.when({ "log", "--topo-order" }, scripted_response { 0, test::log_record(test::sha_of(1), {}) });

    // 이어 읽기는 ref를 읽지 않는다. 세션이 snapshot 때 걸러 둔 필터를 그대로 쓰고, 현재 브랜치만 한 번 더 지킨다.
    CHECK(finished_of(test.execute(load_more_log_request {})).state == operation_state::succeeded);
    std::vector<test::recorded_call> logs { calls_named(test.process, "log") };
    REQUIRE(logs.size() == 1);
    CHECK(graph_refs_of(logs[0]) == text_list { "--exclude=feature", "--branches", "--remotes", "--exclude=v1.0", "--tags", "HEAD" });

    // 검색의 두 log와 SHA가 닿는지 묻는 명령도 같은 집합이다. 숨긴 브랜치에만 있는 커밋은 찾아도 갈 수 없다.
    // 가짜 SHA는 `aaaa…9`다. 숨긴 브랜치에만 있어 닿지 않는다고 답하게 한다 (닿지 않으면 git이 그 SHA를 낸다).
    test.process.when({ "rev-parse", "--verify" }, scripted_response { 0, test::sha_of(9) + "\n" });
    test.process.when({ "rev-list", "--max-count=1" }, scripted_response { 0, test::sha_of(9) + "\n" });
    const std::vector<operation_event> searched { test.execute(search_commits_request { .query = "aaaa" }) };
    CHECK(finished_of(searched).state == operation_state::succeeded);
    REQUIRE(payloads<commit_search_loaded>(searched).size() == 1);
    // 두 log가 찾은 1번 커밋뿐이다. 닿지 않는 9번은 넣지 않는다.
    REQUIRE(payloads<commit_search_loaded>(searched)[0].search.commits.size() == 1);
    CHECK(payloads<commit_search_loaded>(searched)[0].search.commits[0].sha == test::sha_of(1));
    logs = calls_named(test.process, "log");
    REQUIRE(logs.size() == 3);
    const text_list refs { "--exclude=feature", "--branches", "--remotes", "--exclude=v1.0", "--tags", "HEAD" };
    for (const std::size_t index : { std::size_t { 1 }, std::size_t { 2 } })
    {
        const text_list& arguments { logs[index].arguments };
        REQUIRE(arguments.size() > refs.size() + 1);
        CHECK(text_list { arguments.end() - 1 - static_cast<std::ptrdiff_t>(refs.size()), arguments.end() - 1 } == refs);
    }
    const std::vector<test::recorded_call> reachability { calls_named(test.process, "rev-list") };
    REQUIRE(reachability.size() == 1);
    text_list expected { "rev-list", "--max-count=1", test::sha_of(9), "--not" };
    expected.insert(expected.end(), refs.begin(), refs.end());
    expected.push_back("--");
    CHECK(reachability[0].arguments == expected);

    // "이것만 보기"면 그 ref들뿐이다.
    harness only {};
    only.graph_filter.only = { "refs/heads/feature" };
    only.process.when({ "log", "--topo-order" }, scripted_response { 0, test::log_record(test::sha_of(1), {}) });
    CHECK(finished_of(only.execute(load_more_log_request {})).state == operation_state::succeeded);
    logs = calls_named(only.process, "log");
    REQUIRE(logs.size() == 1);
    CHECK(graph_refs_of(logs[0]) == text_list { "refs/heads/feature" });
}

TEST_CASE("refresh of an unborn repository skips the log", "[operation][refresh]")
{
    harness test {};
    std::string status { "# branch.oid (initial)" };
    status.push_back('\0');
    status.append("# branch.head main");
    status.push_back('\0');
    test.process.when({ "status" }, scripted_response { 0, status });

    const std::vector<operation_event> events { test.execute(refresh_request {}) };

    const text_list names { test.process.command_names() };
    CHECK(std::find(names.begin(), names.end(), "log") == names.end());
    const std::vector<commits_loaded> batches { payloads<commits_loaded>(events) };
    REQUIRE(batches.size() == 1);
    CHECK(batches[0].history_complete);
    CHECK(finished_of(events).state == operation_state::succeeded);
}

TEST_CASE("a cancelled log stream does not turn its cut-off tail into a commit", "[operation][log]")
{
    git_operation_runner_options options {};
    options.log_batch_size = 1;
    options.log_flush_interval = std::chrono::hours { 1 };
    // 완결된 레코드 둘 다음에 제목 한가운데서 끊긴 셋째 레코드다(NUL이 없다). 필드 수와 시간은 그대로 읽힌다.
    std::string log { test::log_record(test::sha_of(3), { test::sha_of(2) }) + test::log_record(test::sha_of(2), { test::sha_of(1) }) };
    std::string tail { test::log_record(test::sha_of(1), {}, "a subject cut in the mid") };
    tail.pop_back();
    log.append(tail);
    const auto commit_count = [](const std::vector<operation_event>& events) {
        std::size_t count { 0 };
        for (const commits_loaded& batch : payloads<commits_loaded>(events))
            count += batch.commits.size();
        return count;
    };

    SECTION("cancelled")
    {
        harness test { options };
        test.process.when({ "log", "--topo-order" }, scripted_response { 0, log, {}, {}, process_completion::cancelled });
        const std::vector<operation_event> events { test.execute(load_more_log_request {}) };
        CHECK(commit_count(events) == 2);
        for (const commits_loaded& batch : payloads<commits_loaded>(events))
            for (const commit_info& commit : batch.commits)
                CHECK(commit.sha != test::sha_of(1));
        CHECK(finished_of(events).state == operation_state::cancelled);
    }

    SECTION("a successful stream still reads its tail as before")
    {
        harness test { options };
        test.process.when({ "log", "--topo-order" }, scripted_response { 0, log });
        const std::vector<operation_event> events { test.execute(load_more_log_request {}) };
        CHECK(commit_count(events) == 3);
        CHECK(finished_of(events).state == operation_state::succeeded);
    }
}

TEST_CASE("a dropped log record still counts toward a full page", "[operation][log]")
{
    git_operation_runner_options options {};
    options.log_page_size = 3;
    const std::string valid { test::log_record(test::sha_of(3), { test::sha_of(2) }) + test::log_record(test::sha_of(2), { test::sha_of(1) }) };

    SECTION("a malformed record keeps the history open")
    {
        // 필드가 모자란 NUL 종료 레코드다. git은 쪽을 다 채워 돌려줬으므로 뒤에 이력이 더 있을 수 있다.
        harness test { options };
        std::string log { valid };
        log.append("deadbeef\x1Fx");
        log.push_back('\0');
        test.process.when({ "log", "--topo-order" }, scripted_response { 0, log });
        const std::vector<commits_loaded> batches { payloads<commits_loaded>(test.execute(load_more_log_request {})) };
        REQUIRE_FALSE(batches.empty());
        CHECK(batches.back().page_complete);
        CHECK_FALSE(batches.back().history_complete);
    }

    SECTION("a short page without dropped records ends the history")
    {
        harness test { options };
        test.process.when({ "log", "--topo-order" }, scripted_response { 0, valid });
        const std::vector<commits_loaded> batches { payloads<commits_loaded>(test.execute(load_more_log_request {})) };
        REQUIRE_FALSE(batches.empty());
        CHECK(batches.back().page_complete);
        CHECK(batches.back().history_complete);
    }
}

TEST_CASE("refresh reports rebase progress from the git directory", "[operation][refresh][conflict]")
{
    harness test {};
    test.files.put(u8"C:/work/repo/.git/rebase-merge/msgnum", "2\n");
    test.files.put(u8"C:/work/repo/.git/rebase-merge/end", "3\n");
    test.files.put(u8"C:/work/repo/.git/rebase-merge/onto", "85bed87523af002c3c36633ebeb6295417ca6006\n");
    test.files.put(u8"C:/work/repo/.git/rebase-merge/head-name", "refs/heads/feature\n");
    test.files.put(u8"C:/work/repo/.git/rebase-merge/message", "feat two\n");
    test.process.when({ "status" }, scripted_response { 0, test::status_header(test::sha_of(1), "(detached)") + test::status_conflict("a.txt") });

    const std::vector<snapshot_loaded> snapshots { payloads<snapshot_loaded>(test.execute(refresh_request {})) };

    REQUIRE(snapshots.size() == 1);
    const repository_summary& summary { snapshots[0].snapshot.summary };
    CHECK(summary.operation_state == repository_operation_state::rebase);
    CHECK(summary.in_progress.kind == repository_operation_state::rebase);
    CHECK(summary.in_progress.step == 2);
    CHECK(summary.in_progress.total == 3);
    CHECK(summary.in_progress.current_summary == "feat two");
    CHECK(summary.in_progress.head_name == "refs/heads/feature");
    CHECK(snapshots[0].snapshot.conflicts.size() == 1);
}

TEST_CASE("a blocked checkout finishes as needs_input with the overwritten files", "[operation][checkout]")
{
    harness test {};
    scripted_response blocked { 1 };
    blocked.standard_error_lines.push_back("error: Your local changes to the following files would be overwritten by checkout:");
    blocked.standard_error_lines.push_back("\ta.txt");
    blocked.standard_error_lines.push_back("\tb.txt");
    blocked.standard_error_lines.push_back("Please commit your changes or stash them before you switch branches.");
    blocked.standard_error_lines.push_back("Aborting");
    test.process.when({ "switch" }, blocked);

    const std::vector<operation_event> events { test.execute(checkout_branch_request { "feature", checkout_policy::ask }) };

    const operation_finished& finished { finished_of(events) };
    REQUIRE(finished.state == operation_state::needs_input);
    REQUIRE(finished.needs_input.has_value());
    CHECK(finished.needs_input->kind == input_kind::dirty_worktree);
    CHECK(finished.needs_input->files == text_list { "a.txt", "b.txt" });
    CHECK(finished.needs_input->target == "feature");
    // worker는 기다리지 않고 한 번 시도한 뒤 끝난다.
    CHECK(test.process.command_names() == text_list { "switch" });
}

TEST_CASE("checkout policies run the matching commands", "[operation][checkout]")
{
    SECTION("stash and reapply runs stash push, switch and stash pop as one operation")
    {
        harness test {};
        const std::vector<operation_event> events { test.execute(checkout_branch_request { "main", checkout_policy::stash_and_reapply }) };
        CHECK(finished_of(events).state == operation_state::succeeded);
        const std::vector<test::recorded_call> calls { test.process.calls() };
        REQUIRE(calls.size() == 3);
        CHECK(join(calls[0].arguments).starts_with("stash push --include-untracked -m"));
        CHECK(calls[1].arguments == text_list { "switch", "--", "main" });
        CHECK(calls[2].arguments == text_list { "stash", "pop", "stash@{0}" });
        // 작업 로그에는 세 명령이 모두 남는다.
        CHECK(payloads<operation_command_finished>(events).size() == 3);
    }

    SECTION("nothing to stash does not pop someone else's stash")
    {
        harness test {};
        test.process.when({ "stash", "push" }, scripted_response { 0, "No local changes to save\n" });
        CHECK(finished_of(test.execute(checkout_branch_request { "main", checkout_policy::stash_and_reapply })).state == operation_state::succeeded);
        CHECK(test.process.command_names() == text_list { "stash", "switch" });
    }

    SECTION("a conflicting pop keeps the stash and reports a conflict")
    {
        harness test {};
        test.process.when({ "stash", "pop" }, scripted_response { 1, "Auto-merging a.txt\nCONFLICT (content): Merge conflict in a.txt\n", { "The stash entry is kept in case you need it again." } });
        test.process.when({ "status" }, scripted_response { 0, test::status_header(test::sha_of(1), "main") + test::status_conflict("a.txt") });
        const operation_finished& finished { finished_of(test.execute(checkout_branch_request { "main", checkout_policy::stash_and_reapply })) };
        REQUIRE(finished.state == operation_state::failed);
        REQUIRE(finished.failure.has_value());
        CHECK(finished.failure->kind == failure_kind::conflict);
        CHECK(finished.failure->summary.find(u8"stash@{0}") != std::u8string::npos);
        const text_list names { test.process.command_names() };
        CHECK(std::count(names.begin(), names.end(), "stash") == 2);
    }

    SECTION("discard and carry pass the switch mode")
    {
        harness test {};
        static_cast<void>(test.execute(checkout_branch_request { "main", checkout_policy::discard }));
        static_cast<void>(test.execute(checkout_detached_request { "abc1234", checkout_policy::carry }));
        const std::vector<test::recorded_call> calls { test.process.calls() };
        REQUIRE(calls.size() == 2);
        CHECK(calls[0].arguments == text_list { "switch", "--discard-changes", "--", "main" });
        CHECK(calls[1].arguments == text_list { "switch", "--detach", "--merge", "--", "abc1234" });
    }

    SECTION("a remote branch checkout creates a tracking branch only when the local one is missing")
    {
        harness test {};
        test.process.when_once({ "show-ref" }, scripted_response { 1 });
        static_cast<void>(test.execute(checkout_remote_branch_request { "origin/release/1.0" }));
        static_cast<void>(test.execute(checkout_remote_branch_request { "origin/release/1.0" }));
        const std::vector<test::recorded_call> calls { test.process.calls() };
        REQUIRE(calls.size() == 4);
        CHECK(calls[1].arguments == text_list { "switch", "-c", "release/1.0", "--track", "--", "origin/release/1.0" });
        CHECK(calls[3].arguments == text_list { "switch", "--", "release/1.0" });
    }
}

TEST_CASE("a rebase conflict finishes as needs_input and is detected from the repository state", "[operation][conflict]")
{
    harness test {};
    // rebase는 CONFLICT를 stdout에, `could not apply`를 stderr에 내고 1로 끝난다.
    test.process.when(
        { "rebase", "--", "main", "feature" }, scripted_response { 1, "Auto-merging a.txt\nCONFLICT (content): Merge conflict in a.txt\n", { "error: could not apply b39aec2... feat two" } });
    test.process.when({ "status" }, scripted_response { 0, test::status_header(test::sha_of(1), "(detached)") + test::status_conflict("a.txt") });
    test.files.put(u8"C:/work/repo/.git/rebase-merge/msgnum", "2\n");

    const operation_finished& finished { finished_of(test.execute(rebase_request { "feature", "main" })) };

    REQUIRE(finished.state == operation_state::needs_input);
    REQUIRE(finished.needs_input.has_value());
    CHECK(finished.needs_input->kind == input_kind::conflict);
    CHECK(finished.needs_input->sequence == repository_operation_state::rebase);
    CHECK(finished.needs_input->files == text_list { "a.txt" });
}

TEST_CASE("continue that still has unresolved files stays in needs_input", "[operation][conflict]")
{
    harness test {};
    // 영어 문구가 분류기 목록에 없어도 표지 파일과 status로 판정한다.
    test.process.when(
        { "-c", "core.editor=true", "rebase", "--continue" }, scripted_response { 1, {}, { "error: you must edit all merge conflicts and then", "mark them as resolved using git add" } });
    test.process.when({ "status" }, scripted_response { 0, test::status_header(test::sha_of(1), "(detached)") + test::status_conflict("a.txt") });
    test.files.put(u8"C:/work/repo/.git/rebase-merge/msgnum", "2\n");

    const operation_finished& finished { finished_of(test.execute(sequence_control_request { operation_kind::rebase_continue })) };
    CHECK(finished.state == operation_state::needs_input);
}

TEST_CASE("resolve_file takes one side and stages it", "[operation][conflict]")
{
    harness test {};
    CHECK(finished_of(test.execute(resolve_file_request { "a.txt", conflict_side::theirs })).state == operation_state::succeeded);
    const std::vector<test::recorded_call> calls { test.process.calls() };
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].arguments == text_list { "--literal-pathspecs", "checkout", "--theirs", "--", "a.txt" });
    CHECK(calls[1].arguments == text_list { "--literal-pathspecs", "add", "--", "a.txt" });
}

TEST_CASE("merge into another branch checks it out first", "[operation][merge]")
{
    harness test {};
    test.process.when({ "symbolic-ref" }, scripted_response { 0, "feature\n" });

    CHECK(finished_of(test.execute(merge_request { "main", "feature" })).state == operation_state::succeeded);
    const std::vector<test::recorded_call> calls { test.process.calls() };
    REQUIRE(calls.size() == 3);
    CHECK(calls[1].arguments == text_list { "switch", "--", "main" });
    CHECK(calls[2].arguments == text_list { "merge", "--no-edit", "--", "feature" });
}

TEST_CASE("fast_forward uses merge --ff-only on the current branch and fetch . elsewhere", "[operation][fast_forward]")
{
    SECTION("current branch")
    {
        harness test {};
        test.process.when({ "symbolic-ref" }, scripted_response { 0, "main\n" });
        static_cast<void>(test.execute(fast_forward_request { "main", "feature" }));
        CHECK(test.process.calls().back().arguments == text_list { "merge", "--no-edit", "--ff-only", "--", "feature" });
    }

    SECTION("another branch is refused when it is not a fast-forward")
    {
        harness test {};
        test.process.when({ "symbolic-ref" }, scripted_response { 0, "main\n" });
        test.process.when({ "rev-parse", "--symbolic-full-name" }, scripted_response { 0, "refs/remotes/origin/feature\n" });
        test.process.when({ "fetch" }, scripted_response { 1, {}, { "From .", " ! [rejected]        origin/feature -> feature  (non-fast-forward)" } });

        const operation_finished& finished { finished_of(test.execute(fast_forward_request { "feature", "origin/feature" })) };
        CHECK(test.process.calls().back().arguments == text_list { "fetch", "--", ".", "refs/remotes/origin/feature:refs/heads/feature" });
        REQUIRE(finished.failure.has_value());
        CHECK(finished.failure->kind == failure_kind::not_possible_to_fast_forward);
    }
}

TEST_CASE("git failures map to the failure kinds the banner understands", "[operation][failure]")
{
    const auto failure_for = [](const scripted_response& response) {
        harness test {};
        test.process.when({ "push" }, response);
        const operation_finished finished { finished_of(test.execute(push_execute_request { "origin" })) };
        REQUIRE(finished.failure.has_value());
        return *finished.failure;
    };

    CHECK(failure_for(scripted_response { 1, {}, { "To C:/remote.git", " ! [rejected]        main -> main (fetch first)" } }).kind == failure_kind::rejected_fetch_first);
    CHECK(failure_for(scripted_response { 1, {}, { " ! [rejected]        main -> main (non-fast-forward)" } }).kind == failure_kind::rejected_non_ff);
    CHECK(failure_for(scripted_response { 1, {}, { " ! [rejected]        main -> main (stale info)" } }).kind == failure_kind::stale_info);
    CHECK(failure_for(scripted_response { 128, {}, { "ssh: Could not resolve hostname example.invalid: No such host is known." } }).kind == failure_kind::offline);
    CHECK(failure_for(scripted_response { 128, {}, { "git@github.com: Permission denied (publickey)." } }).kind == failure_kind::auth_failed);
    CHECK(failure_for(scripted_response { 128, {}, { "Host key verification failed." } }).kind == failure_kind::host_key_refused);
    CHECK(failure_for(scripted_response { 128, {}, { "fatal: Unable to create 'C:/work/repo/.git/index.lock': File exists." } }).kind == failure_kind::index_lock);
    CHECK(failure_for(scripted_response { 128, {}, { "fatal: repository 'https://example.com/r.git/' not found", "remote: Repository not found." } }).kind == failure_kind::not_found);

    scripted_response changed_key { 128 };
    changed_key.standard_error_lines.push_back("@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @");
    changed_key.standard_error_lines.push_back("Offending ED25519 key in C:/Users/me/.ssh/known_hosts:4");
    changed_key.standard_error_lines.push_back("Host key verification failed.");
    const operation_failure changed { failure_for(changed_key) };
    CHECK(changed.kind == failure_kind::host_key_changed);
    CHECK(changed.summary.find(u8"known_hosts:4") != std::u8string::npos);
    // 실패 원문은 [자세히]가 보인다.
    CHECK(changed.detail.find(u8"Offending") != std::u8string::npos);
}

TEST_CASE("progress lines become throttled progress events", "[operation][progress]")
{
    harness test {};
    test.process.when(
        { "fetch" }, scripted_response { 0, {}, { "From C:/remote.git" }, { "Receiving objects:  10% (1/10)", "Receiving objects:  50% (5/10)", "Resolving deltas: 100% (3/3), done." } });

    const std::vector<operation_event> events { test.execute(fetch_request { "origin" }) };
    const std::vector<operation_progressed> progress { payloads<operation_progressed>(events) };
    // 단계가 바뀌거나 끝난 줄은 간격과 관계없이 보낸다.
    REQUIRE(progress.size() >= 2);
    CHECK(progress.front().progress.phase_text == u8"Receiving objects");
    CHECK(progress.back().progress.phase_text == u8"Resolving deltas");
    REQUIRE(progress.back().progress.fraction.has_value());
    CHECK(*progress.back().progress.fraction == 1.0);
    CHECK(test.process.calls().front().arguments == text_list { "fetch", "--prune", "--progress", "--", "origin" });
}

TEST_CASE("a cancelled command finishes the operation as cancelled", "[operation][cancel]")
{
    harness test {};
    test.process.when({ "fetch" }, scripted_response { 0, {}, {}, {}, process_completion::cancelled });
    CHECK(finished_of(test.execute(fetch_request {})).state == operation_state::cancelled);
}

TEST_CASE("a stash request stops when its number now points at another stash", "[operation][stash]")
{
    const std::string chosen { test::sha_of(7) };
    SECTION("the number moved to another stash")
    {
        // 화면에서 고른 뒤 다른 곳에서 stash를 더해 `stash@{0}`이 다른 커밋이 됐다. 사용자가 고르지 않은 stash를 지우지 않는다.
        harness test {};
        test.process.when({ "rev-parse", "--verify" }, scripted_response { 0, test::sha_of(8) + "\n" });
        const operation_finished finished { finished_of(test.execute(stash_drop_request { .index = 0, .expected_sha = chosen })) };
        REQUIRE(finished.failure.has_value());
        CHECK(finished.failure->kind == failure_kind::invalid_request);
        const text_list names { test.process.command_names() };
        CHECK(std::find(names.begin(), names.end(), "stash") == names.end());
    }

    SECTION("the same stash is still there")
    {
        harness test {};
        test.process.when({ "rev-parse", "--verify" }, scripted_response { 0, chosen + "\n" });
        CHECK(finished_of(test.execute(stash_apply_request { .index = 0, .expected_sha = chosen })).state == operation_state::succeeded);
        const text_list names { test.process.command_names() };
        CHECK(std::find(names.begin(), names.end(), "stash") != names.end());
    }
}

TEST_CASE("invalid names are refused before git runs", "[operation][failure]")
{
    harness test {};
    const operation_finished& finished { finished_of(test.execute(create_branch_request { "-bad" })) };
    REQUIRE(finished.failure.has_value());
    CHECK(finished.failure->kind == failure_kind::invalid_request);
    CHECK(test.process.calls().empty());
}

TEST_CASE("commit sends the message on stdin and can stage everything first", "[operation][commit]")
{
    harness test {};
    CHECK(finished_of(test.execute(commit_request { "subject\n\nbody", false, true })).state == operation_state::succeeded);
    const std::vector<test::recorded_call> calls { test.process.calls() };
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].arguments == text_list { "add", "--all" });
    CHECK(calls[1].arguments == text_list { "commit", "-F", "-" });
    CHECK(calls[1].standard_input == "subject\n\nbody\n");

    static_cast<void>(test.execute(commit_request { "", true, false }));
    CHECK(test.process.calls().back().arguments == text_list { "commit", "--amend", "--no-edit" });
}

TEST_CASE("tags are explicit about annotation and patches go through git apply", "[operation][tag]")
{
    harness test {};
    static_cast<void>(test.execute(create_tag_request { "v1.0", "abc1234", true, "release" }));
    static_cast<void>(test.execute(create_tag_request { "v1.1", {}, false, "ignored" }));
    static_cast<void>(test.execute(apply_patch_request { "diff --git a/a b/a\n", true, true, true }));
    const std::vector<test::recorded_call> calls { test.process.calls() };
    REQUIRE(calls.size() == 3);
    CHECK(calls[0].arguments == text_list { "tag", "-a", "-F", "-", "--", "v1.0", "abc1234" });
    CHECK(calls[0].standard_input == "release\n");
    CHECK(calls[1].arguments == text_list { "tag", "--", "v1.1" });
    CHECK(calls[1].standard_input.empty());
    CHECK(calls[2].arguments == text_list { "apply", "--cached", "-R", "--recount", "--whitespace=nowarn", "-" });
    CHECK(calls[2].standard_input == "diff --git a/a b/a\n");
}

TEST_CASE("ignore_path appends a line without joining the last one", "[operation][worktree]")
{
    harness test {};
    test.files.put(u8"C:/work/repo/.gitignore", "build/");
    CHECK(finished_of(test.execute(ignore_path_request { "*.log" })).state == operation_state::succeeded);
    CHECK(test.files.content(u8"C:/work/repo/.gitignore") == std::optional<std::string> { "build/\n*.log\n" });
}

TEST_CASE("remove_stale_index_lock refuses while git runs on the repository", "[operation][index_lock]")
{
    harness test {};
    test.files.put(u8"C:/work/repo/.git/index.lock", "");

    // 다른 job의 status가 도는 동안이다.
    std::mutex mutex {};
    std::condition_variable condition {};
    bool entered { false };
    bool release { false };
    scripted_response blocking { 0, test::status_header(test::sha_of(1), "main") };
    blocking.before_reply = [&]() {
        std::unique_lock<std::mutex> lock { mutex };
        entered = true;
        condition.notify_all();
        condition.wait_for(lock, std::chrono::seconds { 10 }, [&]() { return release; });
    };
    test.process.when_once({ "status" }, blocking);
    std::thread other { [&test]() { static_cast<void>(test.execute(refresh_request {})); } };
    {
        std::unique_lock<std::mutex> lock { mutex };
        REQUIRE(condition.wait_for(lock, std::chrono::seconds { 10 }, [&]() { return entered; }));
    }
    CHECK(test.runner.active_process_count(u8"c:\\work\\repo\\") == 1);

    const operation_finished refused { finished_of(test.execute(remove_stale_index_lock_request {})) };
    {
        const std::lock_guard<std::mutex> lock { mutex };
        release = true;
    }
    condition.notify_all();
    other.join();

    REQUIRE(refused.failure.has_value());
    CHECK(refused.failure->kind == failure_kind::refused);
    CHECK(test.files.content(u8"C:/work/repo/.git/index.lock").has_value());

    // 아무것도 돌지 않으면 지운다.
    CHECK(test.runner.active_process_count(repository_path) == 0);
    CHECK(finished_of(test.execute(remove_stale_index_lock_request {})).state == operation_state::succeeded);
    CHECK_FALSE(test.files.content(u8"C:/work/repo/.git/index.lock").has_value());
}

TEST_CASE("read queries answer ancestry and commit counts", "[operation][query]")
{
    harness test {};
    test.process.when({ "merge-base" }, scripted_response { 1 });
    test.process.when({ "rev-list", "--count" }, scripted_response { 0, "3\n" });

    const std::vector<ancestry_answered> ancestry { payloads<ancestry_answered>(test.execute(is_ancestor_request { "main", "feature" })) };
    REQUIRE(ancestry.size() == 1);
    CHECK_FALSE(ancestry[0].is_ancestor);

    const std::vector<commit_count_answered> counts { payloads<commit_count_answered>(test.execute(count_commits_request { "main", "feature" })) };
    REQUIRE(counts.size() == 1);
    CHECK(counts[0].count == 3);
    for (const test::recorded_call& call : test.process.calls())
        CHECK(call.no_optional_locks);
}

TEST_CASE("inspect_path tells a repository root from a folder inside it", "[operation][setup]")
{
    harness test { {}, false };
    test.files.add_directory(u8"C:/work/repo/src");
    test.files.add_directory(u8"C:/work/plain");
    test.process.when_once({ "rev-parse" }, scripted_response { 0, "C:/work/repo/.git\nC:/work/repo\n" });
    test.process.when_once({ "rev-parse" }, scripted_response { 128, {}, { "fatal: not a git repository (or any of the parent directories): .git" } });

    const auto inspect = [&test](const std::u8string& path) {
        git_operation_job job {};
        job.id = 1;
        job.operation = inspect_path_request { path };
        std::vector<operation_event> events {};
        test.runner.execute(job, [&events](operation_event event) { events.push_back(std::move(event)); });
        const std::vector<path_inspected> found { payloads<path_inspected>(events) };
        REQUIRE(found.size() == 1);
        return found[0];
    };

    const path_inspected inside { inspect(u8"C:/work/repo/src") };
    CHECK(inside.kind == path_inspection_kind::inside_repository);
    CHECK(inside.root == u8"C:/work/repo");
    CHECK(inspect(u8"C:/work/plain").kind == path_inspection_kind::directory);
    CHECK(inspect(u8"C:/work/missing").kind == path_inspection_kind::missing);
    CHECK(test.process.calls()[0].working_directory == "C:/work/repo/src");
}

TEST_CASE("refresh_status reads only the working tree: no git dir, refs, stash, remotes or log", "[operation][refresh]")
{
    harness test {};
    const std::string head { test::sha_of(2) };
    test.process.when({ "status" }, scripted_response { 0, test::status_header(head, "main", "origin/main") + test::status_modified("a.txt") });

    const std::vector<operation_event> events { test.execute(refresh_status_request {}, 4) };

    CHECK(test.process.command_names() == text_list { "status", "diff", "diff" });
    for (const test::recorded_call& call : test.process.calls())
        CHECK(call.no_optional_locks);
    CHECK(payloads<snapshot_loaded>(events).empty());
    CHECK(payloads<commits_loaded>(events).empty());
    const std::vector<status_loaded> loaded { payloads<status_loaded>(events) };
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].summary.head_sha == head);
    CHECK(loaded[0].summary.changed_file_count == 1);
    CHECK(loaded[0].unstaged.size() == 1);
    CHECK(finished_of(events).state == operation_state::succeeded);
}

TEST_CASE("a status cut off at the capture limit fails instead of showing a partial list", "[operation][refresh][failure]")
{
    // 잘린 마지막 경로가 상위 폴더 이름과 같으면 버리기가 폴더 전체에 닿을 수 있다. 잘린 목록은 보이지 않는다.
    const std::size_t limit { git_limits_for(git_command_class::local_query).maximum_captured_bytes_per_stream };
    const std::string huge_status { test::status_header(test::sha_of(1), "main") + test::status_modified(std::string(limit, 'a')) };

    for (const bool full_refresh : { true, false })
    {
        harness test {};
        test.process.when({ "status" }, scripted_response { 0, huge_status });

        const std::vector<operation_event> events { full_refresh ? test.execute(refresh_request {}) : test.execute(refresh_status_request {}) };

        CHECK(payloads<snapshot_loaded>(events).empty());
        CHECK(payloads<status_loaded>(events).empty());
        const operation_finished finished { finished_of(events) };
        CHECK(finished.state == operation_state::failed);
        REQUIRE(finished.failure.has_value());
        CHECK(finished.failure->kind == failure_kind::other);
        CHECK(finished.failure->summary == tr(u8"git.output_too_large"));
    }
}

TEST_CASE("a write command whose output overflows the capture limit still succeeds", "[operation][commit]")
{
    // 쓰기는 이미 일을 마쳤으므로 stdout이 잘렸다고 실패로 바꾸지 않는다.
    harness test {};
    const std::size_t limit { git_limits_for(git_command_class::local_write).maximum_captured_bytes_per_stream };
    test.process.when({ "commit" }, scripted_response { 0, std::string(limit + 1, 'x') });

    CHECK(finished_of(test.execute(commit_request { "subject", false, false })).state == operation_state::succeeded);
}

TEST_CASE("a file diff over the capture limit still loads the part that fits", "[operation][diff]")
{
    // 큰 파일의 diff는 예전처럼 잘린 채 보이고 화면이 줄 수 상한 안내를 붙인다. diff 칸 전체가 오류로 바뀌지 않는다.
    harness test {};
    const std::size_t limit { git_limits_for(git_command_class::local_query).maximum_captured_bytes_per_stream };
    std::string huge_diff { "diff --git a/big.txt b/big.txt\n--- a/big.txt\n+++ b/big.txt\n@@ -1,1 +1,1 @@\n" };
    while (huge_diff.size() <= limit)
        huge_diff.append("+line\n");
    test.process.when({ "diff" }, scripted_response { 0, huge_diff });

    diff_file_request request {};
    request.path = "big.txt";
    request.view = diff_view::unstaged;
    const std::vector<operation_event> events { test.execute(request) };

    CHECK(payloads<file_diff_loaded>(events).size() == 1);
    CHECK(finished_of(events).state == operation_state::succeeded);
}

TEST_CASE("trash_files sends only existing in-repository files to the recycle bin and runs no git", "[operation][trash]")
{
    harness test {};
    test.files.put(u8"C:/work/repo/new.txt", "x");
    test.files.put(u8"C:/work/repo/sub/other.txt", "y");
    test.files.put(u8"C:/work/repo/keep.txt", "z");

    const operation_finished done { finished_of(test.execute(trash_files_request { { "new.txt", "sub/other.txt", "already-gone.txt" } })) };
    CHECK(done.state == operation_state::succeeded);
    CHECK(done.summary == u8"휴지통으로 보냈습니다.");
    CHECK(test.files.recycled() == std::vector<std::u8string> { u8"C:/work/repo\\new.txt", u8"C:/work/repo\\sub\\other.txt" });
    CHECK_FALSE(test.files.content(u8"C:/work/repo/new.txt").has_value());
    CHECK(test.files.content(u8"C:/work/repo/keep.txt").has_value());
    CHECK(test.process.calls().empty());
}

TEST_CASE("trash_files refuses paths outside the repository and does not touch anything", "[operation][trash]")
{
    harness test {};
    test.files.put(u8"C:/work/repo/new.txt", "x");

    for (const char* const path : { "../outside.txt", ".git/config", "/abs.txt", "C:/Windows/x.txt", "a/../../b" })
    {
        const operation_finished done { finished_of(test.execute(trash_files_request { { "new.txt", path } })) };
        CHECK(done.state == operation_state::failed);
        REQUIRE(done.failure.has_value());
        CHECK(done.failure->kind == failure_kind::invalid_request);
    }
    CHECK(finished_of(test.execute(trash_files_request {})).state == operation_state::failed);
    CHECK(test.files.recycled().empty());
    CHECK(test.files.content(u8"C:/work/repo/new.txt").has_value());
}

TEST_CASE("trash_files fails when the recycle bin refuses", "[operation][trash]")
{
    harness test {};
    test.files.put(u8"C:/work/repo/new.txt", "x");
    test.files.refuse_recycling();
    const operation_finished done { finished_of(test.execute(trash_files_request { { "new.txt" } })) };
    CHECK(done.state == operation_state::failed);
    CHECK(test.files.content(u8"C:/work/repo/new.txt").has_value());
}

TEST_CASE("a diff reads with the requested context and flags a textconv driver from the attributes", "[operation][diff]")
{
    harness test {};
    test.process.when({ "diff" }, scripted_response { 0, "" });
    test.process.when({ "check-attr" }, scripted_response { 0, std::string { "doc.docx\0diff\0word\0", 19 } });
    test.process.when({ "config" }, scripted_response { 0, "antiword\n" });

    diff_file_request request {};
    request.path = "doc.docx";
    request.view = diff_view::unstaged;
    request.context_lines = 7;
    const std::vector<operation_event> events { test.execute(request) };

    const std::vector<file_diff_loaded> loaded { payloads<file_diff_loaded>(events) };
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].diff.textconv_driver == "word");
    const std::vector<test::recorded_call> calls { test.process.calls() };
    REQUIRE(calls.size() >= 3);
    CHECK(join(calls[0].arguments).find("-U7") != std::string::npos);
    CHECK(join(calls[1].arguments) == "check-attr -z diff -- doc.docx");
    CHECK(join(calls[2].arguments) == "config --get diff.word.textconv");
}

TEST_CASE("a diff driver without a textconv command, no driver at all, and untracked files show no badge", "[operation][diff]")
{
    SECTION("the driver has no textconv")
    {
        harness test {};
        test.process.when({ "check-attr" }, scripted_response { 0, std::string { "a.txt\0diff\0cpp\0", 15 } });
        test.process.when({ "config" }, scripted_response { 1, "" });
        diff_file_request request {};
        request.path = "a.txt";
        CHECK(payloads<file_diff_loaded>(test.execute(request)).front().diff.textconv_driver.empty());
    }
    SECTION("the attribute is unspecified")
    {
        harness test {};
        test.process.when({ "check-attr" }, scripted_response { 0, std::string { "a.txt\0diff\0unspecified\0", 23 } });
        diff_file_request request {};
        request.path = "a.txt";
        CHECK(payloads<file_diff_loaded>(test.execute(request)).front().diff.textconv_driver.empty());
        // 드라이버가 없으면 config를 부르지 않는다.
        for (const std::string& name : test.process.command_names())
            CHECK(name != "config");
    }
    SECTION("untracked files compare against /dev/null and do not follow attributes")
    {
        harness test {};
        diff_file_request request {};
        request.path = "new.txt";
        request.untracked = true;
        CHECK(payloads<file_diff_loaded>(test.execute(request)).front().diff.textconv_driver.empty());
        for (const std::string& name : test.process.command_names())
            CHECK(name != "check-attr");
    }
    SECTION("a failing check-attr never blocks the diff")
    {
        harness test {};
        test.process.when({ "check-attr" }, scripted_response { 128, "" });
        diff_file_request request {};
        request.path = "a.txt";
        const std::vector<operation_event> events { test.execute(request) };
        CHECK(finished_of(events).state == operation_state::succeeded);
        CHECK(payloads<file_diff_loaded>(events).front().diff.textconv_driver.empty());
    }
}

TEST_CASE("blame_file streams the porcelain output into one file_blame_loaded", "[operation][blame]")
{
    harness test {};
    // 가짜 runner는 stdout을 반으로 나눠 두 조각으로 넘긴다. 해석기가 조각 경계를 이어 읽어야 한다.
    test.process.when({ "blame", "--porcelain" }, scripted_response { 0, test::read_fixture("blame_renamed.bin") });

    const blame_file_request request { .path = "b[12].txt", .revision = "HEAD" };
    const std::vector<operation_event> events { test.execute(request) };

    CHECK(finished_of(events).state == operation_state::succeeded);
    const std::vector<file_blame_loaded> loaded { payloads<file_blame_loaded>(events) };
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].blame.request == request);
    CHECK(loaded[0].blame.origins.size() == 4);
    REQUIRE(loaded[0].blame.lines.size() == 8);
    CHECK(loaded[0].blame.lines[1].text == "TWO");
    CHECK_FALSE(loaded[0].blame.binary);

    const std::vector<test::recorded_call> calls { test.process.calls() };
    REQUIRE(calls.size() == 1);
    CHECK(join(calls[0].arguments) == "blame --porcelain HEAD -- b[12].txt");
    // 읽기 lane의 명령은 잠금을 잡지 않는다.
    CHECK(calls[0].no_optional_locks);
}

TEST_CASE("blame_file of the working tree passes no revision and reports a binary file without lines", "[operation][blame]")
{
    harness test {};
    test.process.when({ "blame", "--porcelain" }, scripted_response { 0, test::read_fixture("blame_binary.bin") });

    const std::vector<operation_event> events { test.execute(blame_file_request { .path = "blob.bin" }) };

    const std::vector<file_blame_loaded> loaded { payloads<file_blame_loaded>(events) };
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].blame.binary);
    CHECK(loaded[0].blame.lines.empty());
    CHECK(join(test.process.calls().front().arguments) == "blame --porcelain -- blob.bin");
}

TEST_CASE("blame_file reads output larger than the capture limit", "[operation][blame]")
{
    // 큰 파일의 blame 출력은 캡처 상한을 넘는다. 모으지 않고 조각을 받는 대로 읽으므로 잘리지 않는다.
    harness test {};
    const std::size_t limit { git_limits_for(git_command_class::local_query).maximum_captured_bytes_per_stream };
    const std::string sha { test::sha_of(1) };
    std::string output { sha + " 1 1 1\nauthor Morgan\nauthor-mail <morgan@example.com>\nauthor-time 1700000000\nsummary big\nfilename big.txt\n\tfirst\n" };
    std::size_t line_count { 1 };
    const std::string filler(200, 'x');
    while (output.size() <= limit)
    {
        ++line_count;
        output.append(sha).append(" ").append(std::to_string(line_count)).append(" ").append(std::to_string(line_count)).append(" 1\n\t").append(filler).append("\n");
    }
    test.process.when({ "blame" }, scripted_response { 0, output });

    const std::vector<operation_event> events { test.execute(blame_file_request { .path = "big.txt" }) };

    CHECK(finished_of(events).state == operation_state::succeeded);
    const std::vector<file_blame_loaded> loaded { payloads<file_blame_loaded>(events) };
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].blame.lines.size() == line_count);
    CHECK(loaded[0].blame.origins.size() == 1);
}

TEST_CASE("blame_file fails without a result when git fails, the output is broken, or the request is invalid", "[operation][blame]")
{
    SECTION("git reports a missing path")
    {
        harness test {};
        test.process.when({ "blame" }, scripted_response { 128, "", { "fatal: no such path 'gone.txt' in HEAD" } });
        const std::vector<operation_event> events { test.execute(blame_file_request { .path = "gone.txt" }) };

        const operation_finished done { finished_of(events) };
        CHECK(done.state == operation_state::failed);
        REQUIRE(done.failure.has_value());
        CHECK(done.failure->detail.find(u8"no such path") != std::u8string::npos);
        CHECK(payloads<file_blame_loaded>(events).empty());
    }
    SECTION("git prints part of a blame and then fails")
    {
        harness test {};
        const std::string whole { test::read_fixture("blame_renamed.bin") };
        test.process.when({ "blame" }, scripted_response { 128, whole.substr(0, whole.size() / 2), { "fatal: unable to read blob" } });
        const std::vector<operation_event> events { test.execute(blame_file_request { .path = "b[12].txt" }) };

        CHECK(finished_of(events).state == operation_state::failed);
        CHECK(payloads<file_blame_loaded>(events).empty());
    }
    SECTION("the output is not blame output")
    {
        harness test {};
        test.process.when({ "blame" }, scripted_response { 0, "something else entirely\n" });
        const std::vector<operation_event> events { test.execute(blame_file_request { .path = "a.txt" }) };

        const operation_finished done { finished_of(events) };
        CHECK(done.state == operation_state::failed);
        REQUIRE(done.failure.has_value());
        CHECK(done.failure->summary == tr(u8"git.blame_parse_failed"));
        CHECK(payloads<file_blame_loaded>(events).empty());
    }
    SECTION("an option in place of the revision never reaches git")
    {
        harness test {};
        const std::vector<operation_event> events { test.execute(blame_file_request { .path = "a.txt", .revision = "--contents=x" }) };

        const operation_finished done { finished_of(events) };
        CHECK(done.state == operation_state::failed);
        REQUIRE(done.failure.has_value());
        CHECK(done.failure->kind == failure_kind::invalid_request);
        CHECK(test.process.calls().empty());
    }
}

TEST_CASE("file_history asks for one commit more than the limit to learn whether it was cut", "[operation][file_history]")
{
    // 고정 출력에는 커밋이 넷 있다.
    const std::string four { test::read_fixture("file_log_renamed.bin") };

    SECTION("more commits than the limit")
    {
        harness test {};
        test.process.when({ "--literal-pathspecs", "log", "--follow" }, scripted_response { 0, four });
        const file_history_request request { .path = "b[12].txt", .revision = {}, .max_count = 3 };
        const std::vector<operation_event> events { test.execute(request) };

        CHECK(finished_of(events).state == operation_state::succeeded);
        const std::vector<file_history_loaded> loaded { payloads<file_history_loaded>(events) };
        REQUIRE(loaded.size() == 1);
        CHECK(loaded[0].history.request == request);
        CHECK(loaded[0].history.truncated);
        // 넘친 하나는 버린다.
        REQUIRE(loaded[0].history.entries.size() == 3);
        CHECK(loaded[0].history.entries[0].commit.subject == "fourth");
        CHECK(loaded[0].history.entries[1].status == 'R');
        CHECK(loaded[0].history.entries[1].path == "b[12].txt");
        CHECK(loaded[0].history.entries[1].previous_path == "a[1].txt");
        CHECK(loaded[0].history.entries[2].path == "a[1].txt");

        const std::vector<test::recorded_call> calls { test.process.calls() };
        REQUIRE(calls.size() == 1);
        CHECK(join(calls[0].arguments).find("--max-count=4 HEAD -- b[12].txt") != std::string::npos);
        CHECK(calls[0].no_optional_locks);
    }
    SECTION("exactly as many commits as the limit")
    {
        harness test {};
        test.process.when({ "--literal-pathspecs", "log", "--follow" }, scripted_response { 0, four });
        const std::vector<operation_event> events { test.execute(file_history_request { .path = "b[12].txt", .revision = "main", .max_count = 4 }) };

        const std::vector<file_history_loaded> loaded { payloads<file_history_loaded>(events) };
        REQUIRE(loaded.size() == 1);
        CHECK_FALSE(loaded[0].history.truncated);
        CHECK(loaded[0].history.entries.size() == 4);
        CHECK(join(test.process.calls().front().arguments).find("--max-count=5 main -- b[12].txt") != std::string::npos);
    }
    SECTION("no limit")
    {
        harness test {};
        test.process.when({ "--literal-pathspecs", "log", "--follow" }, scripted_response { 0, four });
        const std::vector<operation_event> events { test.execute(file_history_request { .path = "b[12].txt", .revision = {}, .max_count = 0 }) };

        const std::vector<file_history_loaded> loaded { payloads<file_history_loaded>(events) };
        REQUIRE(loaded.size() == 1);
        CHECK_FALSE(loaded[0].history.truncated);
        CHECK(loaded[0].history.entries.size() == 4);
        CHECK(join(test.process.calls().front().arguments).find("--max-count") == std::string::npos);
    }
    SECTION("a path git has never seen is an empty history")
    {
        // git은 없는 경로의 이력을 실패가 아니라 빈 출력으로 답한다.
        harness test {};
        const std::vector<operation_event> events { test.execute(file_history_request { .path = "nope.txt" }) };

        CHECK(finished_of(events).state == operation_state::succeeded);
        const std::vector<file_history_loaded> loaded { payloads<file_history_loaded>(events) };
        REQUIRE(loaded.size() == 1);
        CHECK(loaded[0].history.entries.empty());
        CHECK_FALSE(loaded[0].history.truncated);
    }
    SECTION("git fails in a repository without commits")
    {
        harness test {};
        test.process.when({ "--literal-pathspecs", "log" }, scripted_response { 128, "", { "fatal: bad revision 'HEAD'" } });
        const std::vector<operation_event> events { test.execute(file_history_request { .path = "a.txt" }) };

        CHECK(finished_of(events).state == operation_state::failed);
        CHECK(payloads<file_history_loaded>(events).empty());
    }
}

namespace {
    // 검색 결과의 순서는 committer 시간이 정한다. 그 시간과 작성자를 정할 수 있는 log 레코드다.
    std::string search_record(const std::string& sha, const std::int64_t committer_time, const std::string& subject, const std::string& author = "Morgan", const bool with_body = false)
    {
        std::string record { sha };
        record.append("\x1f\x1f").append(author).append("\x1f").append("morgan@example.com").append("\x1f").append("1700000000").append("\x1f");
        record.append(std::to_string(committer_time)).append("\x1f").append(subject);
        if (with_body)
            record.append("\x1f").append("a body line\n");
        record.push_back('\0');
        return record;
    }

    // 한 필드의 검색 명령에만 맞는 가짜 응답의 앞 토큰이다. `option`은 `--grep=<질의>`나 `--author=<질의>`다.
    text_list search_command(const std::string& option)
    {
        return { "log", "--topo-order", "-z", "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%ct%x1f%s", "--no-mailmap", "-i", "--fixed-strings", option };
    }

    text_list shas_of(const std::vector<commit_info>& commits)
    {
        text_list shas {};
        for (const commit_info& commit : commits)
            shas.push_back(commit.sha);
        return shas;
    }
} // namespace

TEST_CASE("search_commits merges the message and author matches, newest first and without duplicates", "[operation][commit_search]")
{
    harness test {};
    // 메시지 검색과 작성자 검색은 저마다 topo 순서로 온다. 3번 커밋은 양쪽에 다 맞는다.
    const std::string by_message { search_record(test::sha_of(3), 300, "Fix three") + search_record(test::sha_of(1), 100, "fix one") + search_record(test::sha_of(5), 50, "fix five") };
    const std::string by_author { search_record(test::sha_of(4), 50, "four", "Fix Bot") + search_record(test::sha_of(2), 200, "two", "Fix Bot") + search_record(test::sha_of(3), 300, "Fix three", "Fix Bot") };
    test.process.when(search_command("--grep=Fix"), scripted_response { 0, by_message });
    test.process.when(search_command("--author=Fix"), scripted_response { 0, by_author });

    // 앞뒤 공백은 떼고 찾되, 결과에는 받은 요청을 그대로 싣는다.
    const search_commits_request request { .query = "  Fix\t" };
    const std::vector<operation_event> events { test.execute(request) };

    CHECK(finished_of(events).state == operation_state::succeeded);
    const std::vector<commit_search_loaded> loaded { payloads<commit_search_loaded>(events) };
    REQUIRE(loaded.size() == 1);
    CHECK(loaded[0].search.request == request);
    CHECK_FALSE(loaded[0].search.truncated);
    // committer 시간이 늦은 커밋이 앞이고, 시간이 같은 4번과 5번은 SHA 순이다.
    CHECK(shas_of(loaded[0].search.commits) == text_list { test::sha_of(3), test::sha_of(2), test::sha_of(1), test::sha_of(4), test::sha_of(5) });
    for (const commit_info& commit : loaded[0].search.commits)
        CHECK_FALSE(commit.body.has_value());

    const std::vector<test::recorded_call> calls { test.process.calls() };
    // `Fix`는 16진수가 아니다. SHA를 풀지 않는다.
    REQUIRE(calls.size() == 2);
    // 더 있는지 알려고 상한(기본 1000)보다 하나 더 청한다.
    CHECK(join(calls[0].arguments).find("--fixed-strings --grep=Fix --max-count=1001 --branches --remotes --tags HEAD --") != std::string::npos);
    CHECK(join(calls[1].arguments).find("--fixed-strings --author=Fix --max-count=1001 --branches --remotes --tags HEAD --") != std::string::npos);
    // 읽기 lane의 명령은 잠금을 잡지 않는다.
    CHECK(calls[0].no_optional_locks);
    CHECK(calls[1].no_optional_locks);
}

TEST_CASE("search_commits reports that it was cut when either search or the merged result exceeds the limit", "[operation][commit_search]")
{
    harness test {};
    const std::string one { search_record(test::sha_of(1), 100, "one") };
    const std::string two { search_record(test::sha_of(2), 200, "two") };
    const std::string three { search_record(test::sha_of(3), 300, "three") };
    const std::string four { search_record(test::sha_of(4), 400, "four") };

    SECTION("the message search alone has more than the limit")
    {
        test.process.when(search_command("--grep=o"), scripted_response { 0, three + two + one });
        const std::vector<operation_event> events { test.execute(search_commits_request { .query = "o", .max_count = 2 }) };

        const std::vector<commit_search_loaded> loaded { payloads<commit_search_loaded>(events) };
        REQUIRE(loaded.size() == 1);
        CHECK(loaded[0].search.truncated);
        CHECK(shas_of(loaded[0].search.commits) == text_list { test::sha_of(3), test::sha_of(2) });
        CHECK(join(test.process.calls().front().arguments).find("--max-count=3 ") != std::string::npos);
    }
    SECTION("the author search alone has more than the limit")
    {
        test.process.when(search_command("--author=o"), scripted_response { 0, three + two + one });
        const std::vector<operation_event> events { test.execute(search_commits_request { .query = "o", .max_count = 2 }) };

        const std::vector<commit_search_loaded> loaded { payloads<commit_search_loaded>(events) };
        REQUIRE(loaded.size() == 1);
        CHECK(loaded[0].search.truncated);
        CHECK(loaded[0].search.commits.size() == 2);
    }
    SECTION("each search fits but together they do not")
    {
        test.process.when(search_command("--grep=o"), scripted_response { 0, two + one });
        test.process.when(search_command("--author=o"), scripted_response { 0, four + three });
        const std::vector<operation_event> events { test.execute(search_commits_request { .query = "o", .max_count = 2 }) };

        const std::vector<commit_search_loaded> loaded { payloads<commit_search_loaded>(events) };
        REQUIRE(loaded.size() == 1);
        CHECK(loaded[0].search.truncated);
        // 합친 것 가운데 새 커밋을 남긴다.
        CHECK(shas_of(loaded[0].search.commits) == text_list { test::sha_of(4), test::sha_of(3) });
    }
    SECTION("both searches find the same commits, exactly as many as the limit")
    {
        test.process.when(search_command("--grep=o"), scripted_response { 0, two + one });
        test.process.when(search_command("--author=o"), scripted_response { 0, two + one });
        const std::vector<operation_event> events { test.execute(search_commits_request { .query = "o", .max_count = 2 }) };

        const std::vector<commit_search_loaded> loaded { payloads<commit_search_loaded>(events) };
        REQUIRE(loaded.size() == 1);
        CHECK_FALSE(loaded[0].search.truncated);
        CHECK(shas_of(loaded[0].search.commits) == text_list { test::sha_of(2), test::sha_of(1) });
    }
    SECTION("no limit")
    {
        test.process.when(search_command("--grep=o"), scripted_response { 0, four + three + two + one });
        const std::vector<operation_event> events { test.execute(search_commits_request { .query = "o", .max_count = 0 }) };

        const std::vector<commit_search_loaded> loaded { payloads<commit_search_loaded>(events) };
        REQUIRE(loaded.size() == 1);
        CHECK_FALSE(loaded[0].search.truncated);
        CHECK(loaded[0].search.commits.size() == 4);
        for (const test::recorded_call& call : test.process.calls())
            CHECK(join(call.arguments).find("--max-count") == std::string::npos);
    }
    SECTION("nothing matches")
    {
        const std::vector<operation_event> events { test.execute(search_commits_request { .query = "nothing" }) };

        CHECK(finished_of(events).state == operation_state::succeeded);
        const std::vector<commit_search_loaded> loaded { payloads<commit_search_loaded>(events) };
        REQUIRE(loaded.size() == 1);
        CHECK(loaded[0].search.commits.empty());
        CHECK_FALSE(loaded[0].search.truncated);
    }
}

TEST_CASE("search_commits resolves a hexadecimal query as a commit hash prefix and keeps it only when the graph reaches it", "[operation][commit_search]")
{
    harness test {};
    // `sha_of`는 `aaaa…`로 시작한다. 질의는 대문자여도 된다.
    const std::string sha { test::sha_of(7) };

    SECTION("a prefix of a commit the graph reaches is added to the result")
    {
        test.process.when(search_command("--grep=AAAA"), scripted_response { 0, search_record(test::sha_of(1), 100, "mentions aaaa") });
        test.process.when({ "rev-parse", "--verify" }, scripted_response { 0, sha + "\n" });
        // 출력이 비면 그래프의 ref에서 닿는다.
        test.process.when({ "rev-list" }, scripted_response { 0, "" });
        test.process.when({ "log", "-1" }, scripted_response { 0, search_record(sha, 900, "found by hash", "Robin", true) });
        const std::vector<operation_event> events { test.execute(search_commits_request { .query = "AAAA" }) };

        CHECK(finished_of(events).state == operation_state::succeeded);
        const std::vector<commit_search_loaded> loaded { payloads<commit_search_loaded>(events) };
        REQUIRE(loaded.size() == 1);
        REQUIRE(shas_of(loaded[0].search.commits) == text_list { sha, test::sha_of(1) });
        CHECK(loaded[0].search.commits[0].subject == "found by hash");
        CHECK(loaded[0].search.commits[0].author_name == "Robin");
        // 메시지로 찾은 커밋과 같은 모양이다. 본문을 싣지 않는다.
        CHECK_FALSE(loaded[0].search.commits[0].body.has_value());

        const std::vector<test::recorded_call> calls { test.process.calls() };
        REQUIRE(calls.size() == 5);
        // `^{commit}`이 없으면 같은 앞부분의 tree·blob도 풀리고, 없는 40자 SHA도 그대로 되돌아온다.
        CHECK(join(calls[2].arguments) == "rev-parse --verify --quiet --end-of-options AAAA^{commit}");
        CHECK(join(calls[3].arguments) == "rev-list --max-count=1 " + sha + " --not --branches --remotes --tags HEAD --");
        CHECK(join(calls[4].arguments).find(sha + " --") != std::string::npos);
        for (const test::recorded_call& call : calls)
            CHECK(call.no_optional_locks);
    }
    SECTION("a commit no ref reaches is left out")
    {
        test.process.when({ "rev-parse", "--verify" }, scripted_response { 0, sha + "\n" });
        // 닿지 않으면 git이 그 SHA를 낸다.
        test.process.when({ "rev-list" }, scripted_response { 0, sha + "\n" });
        const std::vector<operation_event> events { test.execute(search_commits_request { .query = "aaaa" }) };

        CHECK(finished_of(events).state == operation_state::succeeded);
        const std::vector<commit_search_loaded> loaded { payloads<commit_search_loaded>(events) };
        REQUIRE(loaded.size() == 1);
        CHECK(loaded[0].search.commits.empty());
        CHECK(test.process.command_names() == text_list { "log", "log", "rev-parse", "rev-list" });
    }
    SECTION("no such commit, or a prefix of several commits, is not a failure")
    {
        test.process.when({ "rev-parse", "--verify" }, scripted_response { 1 });
        const std::vector<operation_event> events { test.execute(search_commits_request { .query = "aaaa" }) };

        CHECK(finished_of(events).state == operation_state::succeeded);
        REQUIRE(payloads<commit_search_loaded>(events).size() == 1);
        CHECK(test.process.command_names() == text_list { "log", "log", "rev-parse" });
    }
    SECTION("a ref whose name looks like a hash is not a hash match")
    {
        // `cafe`라는 브랜치가 있으면 git은 그 ref가 가리키는 커밋을 준다. SHA가 `cafe`로 시작하지 않는다.
        test.process.when({ "rev-parse", "--verify" }, scripted_response { 0, sha + "\n" });
        const std::vector<operation_event> events { test.execute(search_commits_request { .query = "cafe" }) };

        const std::vector<commit_search_loaded> loaded { payloads<commit_search_loaded>(events) };
        REQUIRE(loaded.size() == 1);
        CHECK(loaded[0].search.commits.empty());
        CHECK(test.process.command_names() == text_list { "log", "log", "rev-parse" });
    }
    SECTION("a commit the message search already found is not read again")
    {
        test.process.when(search_command("--grep=aaaa"), scripted_response { 0, search_record(sha, 900, "mentions aaaa") });
        test.process.when({ "rev-parse", "--verify" }, scripted_response { 0, sha + "\n" });
        const std::vector<operation_event> events { test.execute(search_commits_request { .query = "aaaa" }) };

        const std::vector<commit_search_loaded> loaded { payloads<commit_search_loaded>(events) };
        REQUIRE(loaded.size() == 1);
        CHECK(shas_of(loaded[0].search.commits) == text_list { sha });
        CHECK(test.process.command_names() == text_list { "log", "log", "rev-parse" });
    }
    SECTION("only 4 to 64 hexadecimal digits are tried as a hash")
    {
        for (const std::string& query : text_list { "aaa", "aaag", "aa aa", "fix", std::string(65, 'a') })
        {
            INFO("query " << query);
            harness other {};
            other.process.when({ "rev-parse", "--verify" }, scripted_response { 0, sha + "\n" });
            const std::vector<operation_event> events { other.execute(search_commits_request { .query = query }) };

            CHECK(finished_of(events).state == operation_state::succeeded);
            CHECK(other.process.command_names() == text_list { "log", "log" });
        }
    }
}

TEST_CASE("search_commits fails without a result when the query is blank or git fails", "[operation][commit_search]")
{
    SECTION("a blank query never reaches git")
    {
        for (const std::string query : { "", "   ", " \t\r\n" })
        {
            harness test {};
            const std::vector<operation_event> events { test.execute(search_commits_request { .query = query }) };

            const operation_finished done { finished_of(events) };
            CHECK(done.state == operation_state::failed);
            REQUIRE(done.failure.has_value());
            CHECK(done.failure->kind == failure_kind::invalid_request);
            CHECK(done.failure->summary == tr(u8"git.search_query_empty"));
            CHECK(test.process.calls().empty());
            CHECK(payloads<commit_search_loaded>(events).empty());
        }
    }
    SECTION("a line break inside the query never reaches git")
    {
        // git은 줄바꿈으로 패턴을 나눠 어느 한 줄만 맞아도 찾는다. 글자 그대로의 한 덩어리가 아니다.
        harness test {};
        const std::vector<operation_event> events { test.execute(search_commits_request { .query = "one\ntwo" }) };

        const operation_finished done { finished_of(events) };
        CHECK(done.state == operation_state::failed);
        REQUIRE(done.failure.has_value());
        CHECK(done.failure->kind == failure_kind::invalid_request);
        CHECK(test.process.calls().empty());
    }
    SECTION("the message search fails")
    {
        harness test {};
        test.process.when(search_command("--grep=fix"), scripted_response { 128, "", { "fatal: bad revision 'HEAD'" } });
        const std::vector<operation_event> events { test.execute(search_commits_request { .query = "fix" }) };

        const operation_finished done { finished_of(events) };
        CHECK(done.state == operation_state::failed);
        REQUIRE(done.failure.has_value());
        CHECK(done.failure->detail.find(u8"bad revision") != std::u8string::npos);
        CHECK(payloads<commit_search_loaded>(events).empty());
        // 앞 검색이 실패하면 뒤 검색을 돌리지 않는다.
        CHECK(test.process.calls().size() == 1);
    }
    SECTION("the author search fails after the message search found commits")
    {
        harness test {};
        test.process.when(search_command("--grep=fix"), scripted_response { 0, search_record(test::sha_of(1), 100, "fix one") });
        // 출력을 일부 낸 뒤 실패한다. 반쪽 결과를 내면 안 된다.
        test.process.when(search_command("--author=fix"), scripted_response { 128, search_record(test::sha_of(2), 200, "two"), { "fatal: unable to read tree" } });
        const std::vector<operation_event> events { test.execute(search_commits_request { .query = "fix" }) };

        CHECK(finished_of(events).state == operation_state::failed);
        CHECK(payloads<commit_search_loaded>(events).empty());
    }
    SECTION("asking whether the graph reaches the commit fails")
    {
        harness test {};
        test.process.when({ "rev-parse", "--verify" }, scripted_response { 0, test::sha_of(7) + "\n" });
        test.process.when({ "rev-list" }, scripted_response { 128, "", { "fatal: bad object" } });
        const std::vector<operation_event> events { test.execute(search_commits_request { .query = "aaaa" }) };

        CHECK(finished_of(events).state == operation_state::failed);
        CHECK(payloads<commit_search_loaded>(events).empty());
    }
    SECTION("resolving the hash fails for a reason other than not finding it")
    {
        harness test {};
        test.process.when({ "rev-parse", "--verify" }, scripted_response { 128, "", { "fatal: not a git repository" } });
        const std::vector<operation_event> events { test.execute(search_commits_request { .query = "aaaa" }) };

        CHECK(finished_of(events).state == operation_state::failed);
        CHECK(payloads<commit_search_loaded>(events).empty());
    }
}

TEST_CASE("apply_patch passes --unidiff-zero for zero-context diffs", "[operation][worktree]")
{
    harness test {};
    apply_patch_request request {};
    request.patch = "x";
    request.unidiff_zero = true;
    CHECK(finished_of(test.execute(request)).state == operation_state::succeeded);
    CHECK(join(test.process.calls().back().arguments).find("--unidiff-zero") != std::string::npos);
}

TEST_CASE("verify_signatures sends the commits on stdin and says why an ssh signature could not be checked", "[operation][signing]")
{
    harness test {};
    // allowedSignersFile이 설정에 없다 (종료 코드 1) -> 검증 명령이 빈 값으로 덮어 읽는다.
    test.process.when({ "config", "--type=path" }, scripted_response { 1 });
    const std::string first(40, 'a');
    const std::string second(40, 'b');
    std::string records { first };
    records += "\x1f";
    records += "U\x1f\x1f";
    records += "SHA256:abc";
    records.push_back('\0');
    test.process.when({ "-c", "gpg.ssh.allowedSignersFile=", "log" }, scripted_response { 0, records });

    const std::vector<operation_event> events { test.execute(verify_signatures_request { { first, second } }) };
    CHECK(finished_of(events).state == operation_state::succeeded);
    const std::vector<test::recorded_call> calls { test.process.calls() };
    REQUIRE(calls.size() == 2);
    CHECK(calls[1].arguments == text_list { "-c", "gpg.ssh.allowedSignersFile=", "log", "--no-walk=unsorted", "--stdin", "-z", "--format=%H%x1f%G?%x1f%GS%x1f%GK" });
    CHECK(calls[1].standard_input == first + "\n" + second + "\n");
    CHECK(calls[1].no_optional_locks);

    const std::vector<signatures_verified> verified { payloads<signatures_verified>(events) };
    REQUIRE(verified.size() == 1);
    REQUIRE(verified[0].signatures.size() == 2);
    // 서명자가 비고 키가 SHA256:인 U는 목록이 없어 대조하지 못한 것이다.
    CHECK(verified[0].signatures[0].status == signature_status::cannot_check);
    CHECK(verified[0].signatures[0].note == signature_note::allowed_signers_missing);
    // git이 돌려주지 않은 커밋도 "읽는 중"으로 남지 않고 확인할 수 없음으로 풀린다.
    CHECK(verified[0].signatures[1].sha == second);
    CHECK(verified[0].signatures[1].status == signature_status::cannot_check);
    CHECK(verified[0].signatures[1].note == signature_note::none);
}

TEST_CASE("a failed verification still resolves every requested commit so the badge does not wait forever", "[operation][signing]")
{
    harness test {};
    const std::string first(40, 'a');
    const std::string second(40, 'b');
    test.process.when({ "log" }, scripted_response { 128, "", { "fatal: bad object " + first } });
    const std::vector<operation_event> events { test.execute(verify_signatures_request { { first, second } }) };
    const operation_finished finished { finished_of(events) };
    CHECK(finished.state == operation_state::failed);
    const std::vector<signatures_verified> verified { payloads<signatures_verified>(events) };
    REQUIRE(verified.size() == 1);
    REQUIRE(verified[0].signatures.size() == 2);
    for (const commit_signature& signature : verified[0].signatures)
        CHECK(signature.status == signature_status::cannot_check);

    // 완전한 SHA가 아니면 git을 부르지 않고 같은 방식으로 끝난다.
    harness invalid {};
    const std::vector<operation_event> rejected { invalid.execute(verify_signatures_request { { "HEAD" } }) };
    CHECK(finished_of(rejected).failure->kind == failure_kind::invalid_request);
    CHECK(payloads<signatures_verified>(rejected).size() == 1);
    const text_list names { invalid.process.command_names() };
    CHECK(std::ranges::find(names, std::string { "log" }) == names.end());
}

TEST_CASE("commit and tag requests carry the signing flags to git", "[operation][signing]")
{
    harness test {};
    static_cast<void>(test.execute(commit_request { .message = "m", .sign = true }));
    CHECK(test.process.calls().back().arguments == text_list { "commit", "-S", "-F", "-" });
    static_cast<void>(test.execute(commit_request { .message = "m", .sign = false }));
    CHECK(test.process.calls().back().arguments == text_list { "commit", "--no-gpg-sign", "-F", "-" });
    static_cast<void>(test.execute(commit_request { .message = "m" }));
    CHECK(test.process.calls().back().arguments == text_list { "commit", "-F", "-" });

    // 서명한 태그는 항상 annotated다 (`annotated`가 거짓이어도).
    static_cast<void>(test.execute(create_tag_request { .name = "v1", .message = "release", .sign = true }));
    CHECK(test.process.calls().back().arguments == text_list { "tag", "-s", "-F", "-", "--", "v1" });
    CHECK(test.process.calls().back().standard_input == "release\n");
    static_cast<void>(test.execute(create_tag_request { .name = "v2", .annotated = true, .message = "m", .sign = false }));
    CHECK(test.process.calls().back().arguments == text_list { "tag", "-a", "--no-sign", "-F", "-", "--", "v2" });
    static_cast<void>(test.execute(create_tag_request { .name = "v3", .sign = false }));
    CHECK(test.process.calls().back().arguments == text_list { "tag", "--no-sign", "--", "v3" });
    static_cast<void>(test.execute(create_tag_request { .name = "v4" }));
    CHECK(test.process.calls().back().arguments == text_list { "tag", "--", "v4" });
}

TEST_CASE("signing failures end with the failure kind and the gpg or ssh summary", "[operation][signing]")
{
    harness test {};
    scripted_response gpg { 128, "", {} };
    gpg.standard_error_lines
        = { "error: gpg failed to sign the data:", "[GNUPG:] BEGIN_SIGNING H10", "gpg: signing failed: No pinentry", "[GNUPG:] FAILURE sign 67108949", "", "fatal: failed to write commit object" };
    test.process.when({ "commit" }, gpg);
    const operation_finished finished { finished_of(test.execute(commit_request { .message = "m", .sign = true })) };
    REQUIRE(finished.failure.has_value());
    CHECK(finished.failure->kind == failure_kind::signing_passphrase_failed);
    CHECK(finished.failure->summary == u8"gpg가 passphrase를 받지 못했습니다 — gpg-agent 또는 pinentry 설정을 확인하세요.");
    CHECK(finished.failure->detail.find(u8"No pinentry") != std::u8string::npos);

    harness ssh {};
    scripted_response missing { 128, "", {} };
    missing.standard_error_lines = { "error: Couldn't load public key C:/nowhere/id.pub: No such file or directory?", "", "fatal: failed to write commit object" };
    ssh.process.when({ "commit" }, missing);
    const operation_finished ssh_finished { finished_of(ssh.execute(commit_request { .message = "m", .sign = true })) };
    REQUIRE(ssh_finished.failure.has_value());
    CHECK(ssh_finished.failure->kind == failure_kind::signing_key_missing);
    CHECK(ssh_finished.failure->summary.find(u8".pub") != std::u8string::npos);
}
