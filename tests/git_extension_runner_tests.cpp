// submodule·worktree·Git LFS operation을 가짜 runner로 돌려 명령 순서, 환경, 진행률, 실패 분류를 본다. git과 디스크가 필요 없다.
#include "infrastructure/git_operation_runner.h"

#include "fake_git_runner.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {
    using namespace cyella;
    using cyella::test::fake_git_runner;
    using cyella::test::fake_workspace_files;
    using cyella::test::scripted_response;
    using cyella::test::text_list;

    struct harness
    {
        harness()
            : runner { process, u8"C:/Git/cmd/git.exe", files, {} }
        {
            process.when({ "rev-parse", "--absolute-git-dir" }, scripted_response { 0, "C:/work/repo/.git\nC:/work/repo\n" });
        }

        std::vector<operation_event> execute(git_operation operation)
        {
            git_operation_job job {};
            job.id = 3;
            job.repository_id = "repo";
            job.repository_path = u8"C:/work/repo";
            job.operation = std::move(operation);
            job.generation = 1;
            std::vector<operation_event> events {};
            runner.execute(job, [&events](operation_event event) { events.push_back(std::move(event)); });
            return events;
        }

        fake_git_runner process {};
        fake_workspace_files files {};
        git_operation_runner runner;
    };

    operation_finished finished_of(const std::vector<operation_event>& events)
    {
        REQUIRE(events.empty() == false);
        const auto* const finished { std::get_if<operation_finished>(&events.back().payload) };
        REQUIRE(finished != nullptr);
        return *finished;
    }

    std::vector<operation_progress> progress_of(const std::vector<operation_event>& events)
    {
        std::vector<operation_progress> found {};
        for (const operation_event& event : events)
            if (const auto* const payload { std::get_if<operation_progressed>(&event.payload) })
                found.push_back(payload->progress);
        return found;
    }

    const std::string lfs_version { "git-lfs/3.7.1 (GitHub; windows amd64; go 1.25.1; git b84b3384)\n" };
} // namespace

TEST_CASE("lfs_pull installs the filter locally when it is missing and reads progress from stdout", "[operation][extension][lfs]")
{
    harness test {};
    test.process.when({ "lfs", "version" }, scripted_response { 0, lfs_version });
    // 설정이 없으면 `config --get`이 1로 끝난다.
    test.process.when({ "config", "--get", "filter.lfs.smudge" }, scripted_response { 1 });
    std::string output { "Downloading LFS objects:   0% (0/1), 0 B | 0 B/s     \r" };
    output += "Downloading LFS objects: 100% (1/1), 5.0 KB | 1.0 KB/s, done.\r";
    output += "Filtering content: 100% (1/1), 5.0 KB | 5.0 KB/s, done.\n";
    test.process.when({ "lfs", "pull" }, scripted_response { 0, output });

    const std::vector<operation_event> events { test.execute(lfs_pull_request {}) };

    CHECK(finished_of(events).state == operation_state::succeeded);
    CHECK(test.process.command_names() == text_list { "lfs", "config", "lfs", "lfs" });
    const std::vector<test::recorded_call> calls { test.process.calls() };
    REQUIRE(calls.size() == 4);
    CHECK(calls[2].arguments == text_list { "lfs", "install", "--local" });
    CHECK(calls[3].arguments == text_list { "lfs", "pull" });
    // 진행률은 stdout으로 나오므로 터미널이 아니어도 내라고 알린다.
    CHECK(calls[3].variable(u8"GIT_LFS_FORCE_PROGRESS") == std::optional<std::string> { "1" });
    CHECK_FALSE(calls[0].variable(u8"GIT_LFS_FORCE_PROGRESS").has_value());

    const std::vector<operation_progress> progress { progress_of(events) };
    REQUIRE(progress.size() >= 2);
    CHECK(progress.front().phase_text == u8"Downloading LFS objects");
    CHECK(std::ranges::any_of(progress, [](const operation_progress& value) { return value.phase_text == u8"Filtering content" && value.fraction.has_value() && *value.fraction == 1.0; }));
}

TEST_CASE("lfs_pull skips the install when the filter is already configured", "[operation][extension][lfs]")
{
    harness test {};
    test.process.when({ "lfs", "version" }, scripted_response { 0, lfs_version });
    test.process.when({ "config", "--get", "filter.lfs.smudge" }, scripted_response { 0, "git-lfs smudge -- %f\n" });
    const std::vector<operation_event> events { test.execute(lfs_pull_request {}) };
    CHECK(finished_of(events).state == operation_state::succeeded);
    CHECK(test.process.command_names() == text_list { "lfs", "config", "lfs" });
}

TEST_CASE("the LFS operations report a missing git-lfs instead of running", "[operation][extension][lfs]")
{
    harness test {};
    test.process.when({ "lfs", "version" }, scripted_response { 1, {}, { "git: 'lfs' is not a git command. See 'git --help'." } });
    for (git_operation operation : { git_operation { lfs_pull_request {} }, git_operation { lfs_fetch_request {} }, git_operation { lfs_track_request { "*.psd" } } })
    {
        const operation_finished finished { finished_of(test.execute(std::move(operation))) };
        REQUIRE(finished.state == operation_state::failed);
        REQUIRE(finished.failure.has_value());
        CHECK(finished.failure->kind == failure_kind::not_found);
        CHECK(finished.failure->summary.find(u8"git-lfs") != std::u8string::npos);
    }
    const text_list names { test.process.command_names() };
    CHECK(std::ranges::none_of(names, [](const std::string& name) { return name == "config"; }));
}

TEST_CASE("lfs_track runs git lfs track with the pattern after an option terminator", "[operation][extension][lfs]")
{
    harness test {};
    test.process.when({ "lfs", "version" }, scripted_response { 0, lfs_version });
    CHECK(finished_of(test.execute(lfs_track_request { "*.psd" })).state == operation_state::succeeded);
    CHECK(test.process.calls().back().arguments == text_list { "lfs", "track", "--", "*.psd" });

    harness bad {};
    bad.process.when({ "lfs", "version" }, scripted_response { 0, lfs_version });
    const operation_finished rejected { finished_of(bad.execute(lfs_track_request { "a\nb" })) };
    REQUIRE(rejected.failure.has_value());
    CHECK(rejected.failure->kind == failure_kind::invalid_request);
}

TEST_CASE("a checkout refused because the branch lives in another worktree is classified with its folder", "[operation][extension][worktree]")
{
    harness test {};
    test.process.when({ "switch" }, scripted_response { 128, {}, { "fatal: 'feat' is already used by worktree at 'C:/work/repo-feat'" } });
    const operation_finished finished { finished_of(test.execute(checkout_branch_request { .name = "feat" })) };
    REQUIRE(finished.state == operation_state::failed);
    REQUIRE(finished.failure.has_value());
    CHECK(finished.failure->kind == failure_kind::branch_checked_out_elsewhere);
    CHECK(finished.failure->summary.find(u8"C:/work/repo-feat") != std::u8string::npos);
}

TEST_CASE("worktree_remove refuses the main and current worktrees and locked ones without force", "[operation][extension][worktree]")
{
    const std::string head { "HEAD 1111111111111111111111111111111111111111\n" };
    std::string list { "worktree C:/work/repo\n" + head + "branch refs/heads/main\n\n" };
    list += "worktree C:/work/locked\n" + head + "branch refs/heads/locked\nlocked usb\n\n";
    list += "worktree C:/work/free\n" + head + "detached\n";
    harness test {};
    test.process.when({ "worktree", "list" }, scripted_response { 0, list });

    const operation_finished main_tree { finished_of(test.execute(worktree_remove_request { u8"C:\\work\\repo", true })) };
    REQUIRE(main_tree.failure.has_value());
    CHECK(main_tree.failure->kind == failure_kind::refused);

    const operation_finished locked { finished_of(test.execute(worktree_remove_request { u8"c:/work/LOCKED", false })) };
    REQUIRE(locked.failure.has_value());
    CHECK(locked.failure->kind == failure_kind::refused);
    CHECK(locked.failure->summary.find(u8"usb") != std::u8string::npos);

    const operation_finished unknown { finished_of(test.execute(worktree_remove_request { u8"C:/work/other", false })) };
    REQUIRE(unknown.failure.has_value());
    CHECK(unknown.failure->kind == failure_kind::not_found);

    // 여기까지는 remove 명령이 한 번도 나가지 않았다.
    CHECK(std::ranges::none_of(test.process.calls(), [](const test::recorded_call& call) { return call.arguments.size() > 1 && call.arguments[0] == "worktree" && call.arguments[1] == "remove"; }));

    CHECK(finished_of(test.execute(worktree_remove_request { u8"C:/work/free", false })).state == operation_state::succeeded);
    CHECK(test.process.calls().back().arguments == text_list { "worktree", "remove", "--", "C:/work/free" });
    // 잠긴 것을 강제로 지우면 `--force`가 두 번이다.
    CHECK(finished_of(test.execute(worktree_remove_request { u8"C:/work/locked", true })).state == operation_state::succeeded);
    CHECK(test.process.calls().back().arguments == text_list { "worktree", "remove", "--force", "--force", "--", "C:/work/locked" });
}

TEST_CASE("worktree_add needs an absolute folder and something to check out", "[operation][extension][worktree]")
{
    harness test {};
    const operation_finished relative { finished_of(test.execute(worktree_add_request { .path = u8"relative\\dir", .branch = "feat" })) };
    REQUIRE(relative.failure.has_value());
    CHECK(relative.failure->kind == failure_kind::invalid_request);
    const operation_finished nothing { finished_of(test.execute(worktree_add_request { .path = u8"C:/work/new" })) };
    REQUIRE(nothing.failure.has_value());
    CHECK(nothing.failure->kind == failure_kind::invalid_request);
    CHECK(std::ranges::none_of(test.process.calls(), [](const test::recorded_call& call) { return call.arguments.size() > 1 && call.arguments[1] == "add"; }));

    CHECK(finished_of(test.execute(worktree_add_request { .path = u8"C:/work/new", .new_branch = "topic", .start_point = "abc1234" })).state == operation_state::succeeded);
    CHECK(test.process.calls().back().arguments == text_list { "worktree", "add", "-b", "topic", "--", "C:/work/new", "abc1234" });
}

TEST_CASE("submodule update and sync run the matching commands", "[operation][extension][submodule]")
{
    harness test {};
    submodule_update_request update {};
    update.paths = { "libs/a" };
    CHECK(finished_of(test.execute(update)).state == operation_state::succeeded);
    CHECK(test.process.calls().back().arguments == text_list { "submodule", "update", "--init", "--recursive", "--progress", "--", "libs/a" });
    CHECK(finished_of(test.execute(submodule_sync_request { .recursive = false })).state == operation_state::succeeded);
    CHECK(test.process.calls().back().arguments == text_list { "submodule", "sync" });
}

TEST_CASE("refresh reads submodules, worktrees and LFS state only when the repository has them", "[operation][extension][refresh]")
{
    SECTION("a plain repository costs one extra command")
    {
        harness test {};
        test.process.when({ "status" }, scripted_response { 0, "# branch.oid 1111111111111111111111111111111111111111\n# branch.head main\n" });
        const std::vector<operation_event> events { test.execute(refresh_request {}) };
        CHECK(finished_of(events).state == operation_state::succeeded);
        const text_list names { test.process.command_names() };
        CHECK(std::ranges::count(names, "worktree") == 1);
        CHECK(std::ranges::none_of(names, [](const std::string& name) { return name == "submodule" || name == "lfs"; }));
    }

    SECTION("a .gitmodules file and LFS attributes turn on the other reads")
    {
        harness test {};
        test.files.put(u8"C:/work/repo/.gitmodules", "[submodule \"libs/a\"]\n\tpath = libs/a\n\turl = ../a\n");
        test.files.put(u8"C:/work/repo/.gitattributes", "*.bin filter=lfs diff=lfs merge=lfs -text\n");
        test.process.when({ "status" }, scripted_response { 0, "# branch.oid 1111111111111111111111111111111111111111\n# branch.head main\n" });
        test.process.when({ "submodule", "status" }, scripted_response { 0, "-1ecfd40f48933961e1d368341744a076db579db3 libs/a\n" });
        test.process.when({ "lfs", "version" }, scripted_response { 0, lfs_version });
        test.process.when(
            { "lfs", "ls-files" }, scripted_response { 0, "{\"files\":[{\"name\":\"a.bin\",\"size\":10,\"checkout\":true,\"downloaded\":true,\"oid\":\"" + std::string(64, 'a') + "\"}]}" });
        const std::vector<operation_event> events { test.execute(refresh_request {}) };
        std::optional<snapshot_loaded> snapshot {};
        for (const operation_event& event : events)
            if (const auto* const payload { std::get_if<snapshot_loaded>(&event.payload) })
                snapshot = *payload;
        REQUIRE(snapshot.has_value());
        REQUIRE(snapshot->snapshot.submodules.size() == 1);
        CHECK(snapshot->snapshot.submodules.front().state == submodule_state::uninitialized);
        CHECK(snapshot->snapshot.lfs.in_use);
        CHECK(snapshot->snapshot.lfs.available);
        REQUIRE(snapshot->snapshot.lfs.files.size() == 1);
        CHECK(snapshot->snapshot.lfs.files.front().path == "a.bin");
    }
}
