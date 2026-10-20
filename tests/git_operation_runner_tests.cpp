// 실행기(git_operation_runner)를 가짜 runner로 돌려 명령 순서, 사건, 끝맺음을 본다. git과
// 디스크가 필요 없다.
#include "infrastructure/git_operation_runner.h"

#include "fake_git_runner.h"

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
            std::vector<operation_event> events {};
            runner.execute(job, [&events](operation_event event) { events.push_back(std::move(event)); });
            return events;
        }

        fake_git_runner process {};
        fake_workspace_files files {};
        git_operation_runner runner;
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
    CHECK(test.process.command_names() == text_list { "rev-parse", "status", "diff", "diff", "for-each-ref", "stash", "remote", "merge-base", "worktree", "log" });
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
    CHECK(calls[2].arguments == text_list { "apply", "--cached", "-R", "--recount", "-" });
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
