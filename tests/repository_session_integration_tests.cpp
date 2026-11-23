// 저장소 세션을 실제 git과 임시 저장소(로컬 bare 원격 포함)로 끝까지 돌린다. 실행기는 실제
// Win32 process runner를 쓰고, job은 수동 dispatcher가 test 스레드에서 차례로 실행한다. 모두
// `[integration]` 태그를 가지며 git이 없으면 건너뛴다.
#include "application/git_workspace.h"
#include "domain/commit_graph_layout.h"
#include "infrastructure/git_command_builder.h"
#include "infrastructure/git_log_parser.h"
#include "infrastructure/git_operation_runner.h"
#include "infrastructure/git_tool_discovery.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_process_runner.h"
#include "platform/win32/win32_workspace_files.h"

#include "fake_git_runner.h"
#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {
    using namespace cyella;
    using cyella::test::run_git;
    using cyella::test::text_list;

    std::string trimmed(std::string text)
    {
        while (text.empty() == false && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        return text;
    }

    void git_ok(const std::filesystem::path& directory, const text_list& arguments, const std::string_view input = {})
    {
        const test::git_run_result result { run_git(directory, arguments, input) };
        INFO("git " << [&arguments] {
            std::string joined {};
            for (const std::string& argument : arguments)
                joined.append(argument).push_back(' ');
            return joined;
        }());
        INFO("stderr: " << result.standard_error);
        REQUIRE(result.exit_code == 0);
    }

    std::string git_text(const std::filesystem::path& directory, const text_list& arguments)
    {
        const test::git_run_result result { run_git(directory, arguments) };
        REQUIRE(result.exit_code == 0);
        return trimmed(result.standard_output);
    }

    void commit_file(const std::filesystem::path& repo, const std::string& path, const std::string_view content, const std::string& message)
    {
        test::write_file(repo / std::filesystem::path { std::u8string { path.begin(), path.end() } }, content);
        git_ok(repo, { "add", "--", path });
        git_ok(repo, { "commit", "-q", "-m", message });
    }

    std::filesystem::path make_repository(const std::filesystem::path& path)
    {
        std::filesystem::create_directories(path);
        git_ok(path, { "init", "-q", "-b", "main" });
        return path;
    }

    // 실제 git을 쓰는 세션이다. 사건은 바로 반영하지 않고 settle에서 차례로 넘긴다.
    class live_workspace
    {
    public:
        explicit live_workspace(git_operation_runner_options options = {})
            : process_ { win32::make_process_runner() }
            , files_ { win32::make_workspace_files() }
        {
            const std::unique_ptr<file_probe> probe { win32::make_file_probe() };
            tool_ = resolve_git_tool({}, win32::current_git_tool_environment(), *process_, *probe, {});
            REQUIRE(tool_.usable());
            executor_ = std::make_unique<git_operation_runner>(*process_, tool_.executable, *files_, options);
            git_workspace_options workspace_options {};
            workspace_options.refresh_on_open = false;
            workspace_ = std::make_unique<git_workspace>(*executor_, dispatcher_.dispatcher(), events_.sink(), nullptr, workspace_options);
        }

        std::string open(const std::filesystem::path& path)
        {
            const std::string id { workspace_->open_repository(path.generic_u8string()) };
            refresh(id);
            return id;
        }

        // operation을 내고 자동 후속 작업(refresh, ls-remote)까지 모두 끝낸다.
        operation_id run(const std::string& repository, git_operation operation)
        {
            const operation_id id { workspace_->submit(repository, std::move(operation)) };
            REQUIRE(id != 0);
            settle();
            return id;
        }

        void refresh(const std::string& repository)
        {
            static_cast<void>(run(repository, refresh_request {}));
        }

        void settle()
        {
            for (int round { 0 }; round < 64; ++round)
            {
                while (dispatcher_.run())
                {
                }
                const std::vector<operation_event> taken { events_.take() };
                if (taken.empty() && dispatcher_.pending() == 0)
                    return;
                for (const operation_event& event : taken)
                    workspace_->apply(event);
            }
            FAIL("operations did not settle");
        }

        [[nodiscard]] const operation_status& status(const operation_id id) const
        {
            const operation_status* const found { workspace_->find_operation(id) };
            REQUIRE(found != nullptr);
            return *found;
        }

        [[nodiscard]] const repository_session& session(const std::string& repository) const
        {
            const repository_session* const found { workspace_->find_session(repository) };
            REQUIRE(found != nullptr);
            return *found;
        }

        // 실패했으면 이유를 보여 준다.
        void require_succeeded(const operation_id id) const
        {
            const operation_status& found { status(id) };
            if (found.failure.has_value())
            {
                const std::string summary { found.failure->summary.begin(), found.failure->summary.end() };
                const std::string detail { found.failure->detail.begin(), found.failure->detail.end() };
                INFO("failure: " << summary);
                INFO("detail: " << detail);
                REQUIRE(found.state == operation_state::succeeded);
            }
            REQUIRE(found.state == operation_state::succeeded);
        }

    private:
        std::unique_ptr<process_runner> process_ {};
        std::unique_ptr<workspace_files> files_ {};
        git_tool_info tool_ {};
        std::unique_ptr<git_operation_runner> executor_ {};
        test::manual_dispatcher dispatcher_ {};
        test::event_queue events_ {};
        std::unique_ptr<git_workspace> workspace_ {};
    };

    const ref* find_ref(const repository_session& session, const std::string_view full_name)
    {
        for (const ref& entry : session.snapshot().refs)
            if (entry.full_name == full_name)
                return &entry;
        return nullptr;
    }

    // 원격 하나와 그 clone 둘이다. clone `one`이 세션의 대상이다.
    struct remote_fixture
    {
        remote_fixture()
        {
            git_ok(directory.path(), { "init", "-q", "--bare", "-b", "main", bare.string() });
            git_ok(directory.path(), { "clone", "-q", bare.string(), one.string() });
            commit_file(one, "a.txt", "one\n", "first");
            git_ok(one, { "push", "-q", "origin", "main" });
            git_ok(directory.path(), { "clone", "-q", bare.string(), two.string() });
        }

        test::temp_directory directory {};
        std::filesystem::path bare { directory.path() / "origin.git" };
        std::filesystem::path one { directory.path() / "one" };
        std::filesystem::path two { directory.path() / "two" };
    };

    // feature가 main에서 갈라져 세 커밋을 갖고, main은 feature의 두 번째 커밋과 같은 줄을 고친다.
    // feature를 main 위로 rebase하면 2/3번째 커밋에서 충돌한다.
    std::filesystem::path make_conflicting_branches(const std::filesystem::path& root)
    {
        const std::filesystem::path repo { make_repository(root / "repo") };
        commit_file(repo, "a.txt", "1\n2\n3\n", "init");
        git_ok(repo, { "switch", "-q", "-c", "feature" });
        commit_file(repo, "f.txt", "f1\n", "feat one");
        commit_file(repo, "a.txt", "feature\n2\n3\n", "feat two");
        commit_file(repo, "f.txt", "f1\nf3\n", "feat three");
        git_ok(repo, { "switch", "-q", "main" });
        commit_file(repo, "a.txt", "main\n2\n3\n", "main change");
        return repo;
    }
} // namespace

TEST_CASE("integration: an annotated tag is pushed, seen on the remote after fetch and deleted there", "[session][integration][tags]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    remote_fixture fixture {};
    live_workspace live {};
    const std::string repo { live.open(fixture.one) };
    const std::string head { git_text(fixture.one, { "rev-parse", "HEAD" }) };

    live.require_succeeded(live.run(repo, create_tag_request { "v1.0", head, true, "release 1.0" }));
    const ref* tag { find_ref(live.session(repo), "refs/tags/v1.0") };
    REQUIRE(tag != nullptr);
    CHECK(tag->is_annotated_tag);
    CHECK(tag->target_sha == head);
    CHECK(tag->remotes.empty());

    live.require_succeeded(live.run(repo, push_tag_request { "origin", "v1.0" }));
    const operation_id fetch { live.run(repo, fetch_request { "origin" }) };
    live.require_succeeded(fetch);
    tag = find_ref(live.session(repo), "refs/tags/v1.0");
    REQUIRE(tag != nullptr);
    CHECK(tag->remotes == text_list { "origin" });
    CHECK(git_text(fixture.bare, { "tag", "--list" }) == "v1.0");

    live.require_succeeded(live.run(repo, delete_remote_tag_request { "origin", "v1.0" }));
    tag = find_ref(live.session(repo), "refs/tags/v1.0");
    REQUIRE(tag != nullptr);
    CHECK(tag->remotes.empty());
    CHECK(git_text(fixture.bare, { "tag", "--list" }).empty());

    // fetch 뒤에도 원격에 없는 것으로 남는다.
    live.require_succeeded(live.run(repo, fetch_request { "origin" }));
    CHECK(find_ref(live.session(repo), "refs/tags/v1.0")->remotes.empty());
}

TEST_CASE("integration: a rebase conflict waits for a decision, aborts cleanly and continues after resolving", "[session][integration][conflict]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path path { make_conflicting_branches(directory.path()) };
    const std::string feature_before { git_text(path, { "rev-parse", "feature" }) };
    live_workspace live {};
    const std::string repo { live.open(path) };

    const operation_id rebase { live.run(repo, rebase_request { "feature", "main" }) };
    const operation_status& waiting { live.status(rebase) };
    REQUIRE(waiting.state == operation_state::needs_input);
    REQUIRE(waiting.needs_input.has_value());
    CHECK(waiting.needs_input->kind == input_kind::conflict);
    CHECK(waiting.needs_input->sequence == repository_operation_state::rebase);
    CHECK(waiting.needs_input->files == text_list { "a.txt" });
    CHECK_FALSE(waiting.cancellable);

    const repository_summary& stopped { live.session(repo).snapshot().summary };
    CHECK(stopped.in_progress.kind == repository_operation_state::rebase);
    CHECK(stopped.in_progress.step == 2);
    CHECK(stopped.in_progress.total == 3);
    CHECK(stopped.in_progress.current_summary == "feat two");
    CHECK(stopped.in_progress.onto == git_text(path, { "rev-parse", "main" }));
    CHECK(stopped.in_progress.head_name == "refs/heads/feature");
    CHECK(live.session(repo).snapshot().conflicts.size() == 1);

    SECTION("abort clears the in-progress state")
    {
        live.require_succeeded(live.run(repo, sequence_control_request { operation_kind::rebase_abort }));
        const repository_summary& summary { live.session(repo).snapshot().summary };
        CHECK(summary.in_progress.kind == repository_operation_state::none);
        CHECK(summary.operation_state == repository_operation_state::none);
        CHECK(summary.current_branch == "feature");
        CHECK(git_text(path, { "rev-parse", "feature" }) == feature_before);
        CHECK_FALSE(live.session(repo).awaiting_input().has_value());
    }

    SECTION("resolving with ours and continuing rewrites the branch")
    {
        live.require_succeeded(live.run(repo, resolve_file_request { "a.txt", conflict_side::ours }));
        CHECK(live.session(repo).snapshot().conflicts.empty());
        live.require_succeeded(live.run(repo, sequence_control_request { operation_kind::rebase_continue }));

        const repository_session& session { live.session(repo) };
        CHECK(session.snapshot().summary.in_progress.kind == repository_operation_state::none);
        CHECK(session.snapshot().summary.current_branch == "feature");
        CHECK(git_text(path, { "rev-parse", "feature" }) != feature_before);

        // 그래프가 새 이력을 보인다. main의 커밋이 이제 feature의 조상이고, 같은 내용이 된 feat two는 빠진다.
        live.require_succeeded(live.run(repo, is_ancestor_request { "main", "feature" }));
        CHECK(session.ancestry("main", "feature") == true);
        std::vector<std::string> subjects {};
        for (const commit_info& commit : session.graph_commits())
            subjects.push_back(commit.subject);
        CHECK(subjects == text_list { "feat three", "feat one", "main change", "init" });
        CHECK(session.graph_rows().size() == 4);
    }
}

TEST_CASE("integration: a blocked checkout asks first and stash_and_reapply keeps the changes", "[session][integration][checkout]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path path { make_repository(directory.path() / "repo") };
    commit_file(path, "a.txt", "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n", "init");
    git_ok(path, { "switch", "-q", "-c", "other" });
    commit_file(path, "a.txt", "other\n2\n3\n4\n5\n6\n7\n8\n9\n10\n", "other edits line 1");
    git_ok(path, { "switch", "-q", "main" });
    // 다른 줄을 고쳤으므로 pop은 깨끗하게 합쳐진다. 그래도 switch는 파일이 다르다는 이유로 거절한다.
    test::write_file(path / "a.txt", "1\n2\n3\n4\n5\n6\n7\n8\n9\nlocal ten\n");

    live_workspace live {};
    const std::string repo { live.open(path) };
    CHECK(live.session(repo).has_wip_row());

    const operation_id ask { live.run(repo, checkout_branch_request { "other", checkout_policy::ask }) };
    const operation_status& blocked { live.status(ask) };
    REQUIRE(blocked.state == operation_state::needs_input);
    REQUIRE(blocked.needs_input.has_value());
    CHECK(blocked.needs_input->kind == input_kind::dirty_worktree);
    CHECK(blocked.needs_input->files == text_list { "a.txt" });
    CHECK(blocked.needs_input->target == "other");
    CHECK(live.session(repo).snapshot().summary.current_branch == "main");

    const operation_id moved { live.run(repo, checkout_branch_request { "other", checkout_policy::stash_and_reapply }) };
    live.require_succeeded(moved);
    const std::vector<operation_command>& commands { live.status(moved).commands };
    REQUIRE(commands.size() == 3);
    CHECK(std::u8string_view { commands[0].command_line }.find(u8"stash push") != std::u8string_view::npos);
    CHECK(std::u8string_view { commands[1].command_line }.find(u8"switch") != std::u8string_view::npos);
    CHECK(std::u8string_view { commands[2].command_line }.find(u8"stash pop") != std::u8string_view::npos);

    CHECK(live.session(repo).snapshot().summary.current_branch == "other");
    CHECK(test::read_file(path / "a.txt") == "other\n2\n3\n4\n5\n6\n7\n8\n9\nlocal ten\n");
    CHECK(live.session(repo).snapshot().stashes.empty());
}

TEST_CASE("integration: fetch from the bare remote reports its command and updates behind", "[session][integration][remote]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    remote_fixture fixture {};
    commit_file(fixture.two, "b.txt", "two\n", "from two");
    git_ok(fixture.two, { "push", "-q", "origin", "main" });

    live_workspace live {};
    const std::string repo { live.open(fixture.one) };
    CHECK(live.session(repo).snapshot().summary.behind == 0);

    const operation_id fetch { live.run(repo, fetch_request { "origin" }) };
    live.require_succeeded(fetch);
    const operation_status& status { live.status(fetch) };
    // 로컬 전송은 진행 표시를 내지 않을 수도 있다. 그래도 명령과 종료 코드는 반드시 남는다.
    REQUIRE(status.commands.empty() == false);
    CHECK(std::u8string_view { status.commands[0].command_line }.find(u8"fetch") != std::u8string_view::npos);
    CHECK(status.commands[0].exit_code == 0);
    CHECK((status.progress.phase_text.empty() == false || status.commands.size() == 1));

    const repository_summary& summary { live.session(repo).snapshot().summary };
    CHECK(summary.behind == 1);
    CHECK(summary.ahead == 0);
    CHECK(find_ref(live.session(repo), "refs/heads/main")->behind == 1);
}

TEST_CASE("integration: fast_forward moves a branch that is not checked out only when it is a fast-forward", "[session][integration][fast_forward]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path path { make_repository(directory.path() / "repo") };
    commit_file(path, "a.txt", "1\n", "one");
    git_ok(path, { "branch", "behind" });
    git_ok(path, { "branch", "side" });
    commit_file(path, "a.txt", "2\n", "two");
    git_ok(path, { "switch", "-q", "side" });
    commit_file(path, "s.txt", "s\n", "side work");
    git_ok(path, { "switch", "-q", "main" });
    const std::string main_sha { git_text(path, { "rev-parse", "main" }) };
    const std::string side_sha { git_text(path, { "rev-parse", "side" }) };

    live_workspace live {};
    const std::string repo { live.open(path) };

    live.require_succeeded(live.run(repo, fast_forward_request { "behind", "main" }));
    CHECK(git_text(path, { "rev-parse", "behind" }) == main_sha);
    CHECK(find_ref(live.session(repo), "refs/heads/behind")->target_sha == main_sha);

    const operation_id refused { live.run(repo, fast_forward_request { "side", "main" }) };
    const operation_status& status { live.status(refused) };
    REQUIRE(status.state == operation_state::failed);
    REQUIRE(status.failure.has_value());
    CHECK(status.failure->kind == failure_kind::not_possible_to_fast_forward);
    CHECK(git_text(path, { "rev-parse", "side" }) == side_sha);
}

TEST_CASE("integration: merge into a branch that is not current checks it out first", "[session][integration][merge]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path path { make_repository(directory.path() / "repo") };
    commit_file(path, "a.txt", "1\n", "one");
    git_ok(path, { "switch", "-q", "-c", "feature" });
    commit_file(path, "f.txt", "f\n", "feature work");

    live_workspace live {};
    const std::string repo { live.open(path) };
    REQUIRE(live.session(repo).snapshot().summary.current_branch == "feature");

    const operation_id merge { live.run(repo, merge_request { "main", "feature" }) };
    live.require_succeeded(merge);
    const std::vector<operation_command>& commands { live.status(merge).commands };
    REQUIRE(commands.size() == 3);
    CHECK(std::u8string_view { commands[1].command_line }.find(u8"switch -- main") != std::u8string_view::npos);
    CHECK(std::u8string_view { commands[2].command_line }.find(u8"merge --no-edit -- feature") != std::u8string_view::npos);
    CHECK(live.session(repo).snapshot().summary.current_branch == "main");
    CHECK(git_text(path, { "rev-parse", "main" }) == git_text(path, { "rev-parse", "feature" }));
}

TEST_CASE("integration: a streaming refresh of about 500 commits with merges arrives in batches and matches a one-shot layout", "[session][integration][graph]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path path { make_repository(directory.path() / "repo") };

    // 프로세스 하나로 이력을 만든다. 커밋마다 git을 부르면 Debug의 test 시간 제한을 넘는다.
    std::string stream {};
    int mark { 0 };
    int main_tip { 0 };
    std::int64_t time { 1775000000 };
    const auto commit = [&stream, &mark, &time](const std::string& branch, const int from, const int merge, const std::string& message) {
        ++mark;
        stream.append("commit refs/heads/" + branch + "\nmark :" + std::to_string(mark) + "\n");
        stream.append("committer Cy Committer <cy@example.com> " + std::to_string(time++) + " +0000\n");
        stream.append("data " + std::to_string(message.size()) + "\n" + message + "\n");
        if (from != 0)
            stream.append("from :" + std::to_string(from) + "\n");
        if (merge != 0)
            stream.append("merge :" + std::to_string(merge) + "\n");
        stream.append("\n");
        return mark;
    };
    for (int index { 1 }; index <= 400; ++index)
    {
        main_tip = commit("main", main_tip, 0, "main " + std::to_string(index));
        if (index % 20 == 0)
        {
            // 몇 커밋 앞에서 갈라진 곁가지 셋을 만들고 main으로 합친다.
            int side { commit("side" + std::to_string(index), main_tip - 3, 0, "side a " + std::to_string(index)) };
            side = commit("side" + std::to_string(index), side, 0, "side b " + std::to_string(index));
            side = commit("side" + std::to_string(index), side, 0, "side c " + std::to_string(index));
            main_tip = commit("main", main_tip, side, "merge " + std::to_string(index));
        }
    }
    git_ok(path, { "fast-import", "--quiet" }, stream);
    git_ok(path, { "reset", "-q", "--hard", "main" });

    git_operation_runner_options options {};
    options.log_batch_size = 64;
    live_workspace live { options };
    const std::string repo { live.open(path) };
    const repository_session& session { live.session(repo) };

    CHECK_FALSE(session.has_wip_row());
    CHECK(session.history_complete());
    CHECK(session.loaded_commit_count() == static_cast<std::size_t>(mark));
    CHECK(session.commit_batch_count() > 1);

    // 같은 log를 한 번에 해석해 한 번에 배치한 결과와 행이 같아야 한다.
    const test::git_run_result log { run_git(path, make_git_log_args()) };
    REQUIRE(log.exit_code == 0);
    std::vector<graph_commit> commits {};
    for (const commit_info& commit_entry : parse_git_log(log.standard_output))
        commits.push_back(graph_commit { commit_entry.sha, commit_entry.parents });
    commit_graph_layout one_shot {};
    const std::vector<graph_row> expected { one_shot.append(commits) };
    REQUIRE(expected.size() == session.graph_rows().size());
    CHECK(expected == session.graph_rows());
    CHECK(session.graph_lane_count() == one_shot.max_lane_count());
}

TEST_CASE("integration: a Korean file name survives status, diff and log --name-status", "[session][integration][unicode]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path path { make_repository(directory.path() / "repo") };
    const std::string name { "한글 파일.txt" };
    commit_file(path, name, "base\n", "add korean file");
    test::write_file(path / std::filesystem::path { std::u8string { name.begin(), name.end() } }, "base\nchanged\n");

    live_workspace live {};
    const std::string repo { live.open(path) };
    const repository_session& session { live.session(repo) };

    REQUIRE(session.snapshot().unstaged.size() == 1);
    CHECK(session.snapshot().unstaged[0].path == name);
    CHECK(session.snapshot().unstaged[0].lines_added == 1u);

    live.require_succeeded(live.run(repo, diff_file_request { name, diff_view::unstaged }));
    REQUIRE(session.selected_diff().has_value());
    REQUIRE(session.selected_diff()->files.size() == 1);
    CHECK(session.selected_diff()->files[0].new_path == name);
    CHECK(session.selected_diff()->files[0].lines_added == 1);

    const std::string head { session.snapshot().summary.head_sha };
    live.require_succeeded(live.run(repo, show_commit_request { head }));
    REQUIRE(session.selected_commit().has_value());
    REQUIRE(session.selected_commit()->files.size() == 1);
    CHECK(session.selected_commit()->files[0].path == name);
    CHECK(session.selected_commit()->files[0].status == 'A');
    CHECK(session.selected_commit()->commit.subject == "add korean file");
}

TEST_CASE("integration: show_range compares two commits and a range from the root in sha1 and sha256 repositories", "[session][integration][range]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    for (const std::string format : { "sha1", "sha256" })
    {
        INFO("object format " << format);
        test::temp_directory directory {};
        const std::filesystem::path path { directory.path() / "repo" };
        std::filesystem::create_directories(path);
        git_ok(path, { "init", "-q", "-b", "main", "--object-format=" + format });
        commit_file(path, "a.txt", "one\n", "first");
        const std::string first { git_text(path, { "rev-parse", "HEAD" }) };
        commit_file(path, "b.txt", "two\n", "second");
        commit_file(path, "a.txt", "one\nmore\n", "third");
        const std::string third { git_text(path, { "rev-parse", "HEAD" }) };

        live_workspace live {};
        const std::string repo { live.open(path) };
        const repository_session& session { live.session(repo) };

        // 두 커밋 비교: 첫 커밋 → 셋째 커밋. 범위가 아니므로 커밋 수가 없다.
        const show_range_request pair { .base = first, .tip = third, .range = false };
        live.require_succeeded(live.run(repo, pair));
        REQUIRE(session.selected_range().has_value());
        CHECK(session.selected_range()->request == pair);
        CHECK(session.selected_range()->base == first);
        CHECK_FALSE(session.selected_range()->commit_count.has_value());
        REQUIRE(session.selected_range()->files.size() == 2);
        CHECK(session.selected_range()->files[0].path == "a.txt");
        CHECK(session.selected_range()->files[0].status == 'M');
        CHECK(session.selected_range()->files[0].lines_added == 1u);
        CHECK(session.selected_range()->files[1].path == "b.txt");
        CHECK(session.selected_range()->files[1].status == 'A');

        // 루트부터의 범위: 왼쪽은 이 저장소 형식의 빈 트리다.
        const show_range_request root { .base = {}, .tip = third, .range = true };
        live.require_succeeded(live.run(repo, root));
        REQUIRE(session.selected_range().has_value());
        CHECK(session.selected_range()->base == git_text(path, { "hash-object", "-t", "tree", "--stdin" }));
        CHECK(session.selected_range()->base.size() == (format == "sha256" ? 64u : 40u));
        CHECK(session.selected_range()->commit_count == 3u);
        REQUIRE(session.selected_range()->files.size() == 2);
        CHECK(session.selected_range()->files[0].status == 'A');

        // 범위의 파일 diff는 같은 왼쪽 리비전을 쓴다.
        live.require_succeeded(live.run(repo, diff_file_request { .path = "a.txt", .view = diff_view::range, .sha = third, .base = session.selected_range()->base }));
        REQUIRE(session.selected_diff().has_value());
        REQUIRE(session.selected_diff()->files.size() == 1);
        CHECK(session.selected_diff()->files[0].lines_added == 2);
        live.require_succeeded(live.run(repo, diff_file_request { .path = "a.txt", .view = diff_view::range, .sha = third, .base = first }));
        REQUIRE(session.selected_diff().has_value());
        REQUIRE(session.selected_diff()->files.size() == 1);
        CHECK(session.selected_diff()->files[0].lines_added == 1);
    }
}
TEST_CASE("integration: blame_file and file_history follow a renamed file with glob characters in sha1 and sha256 repositories", "[session][integration][blame]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    for (const std::string format : { "sha1", "sha256" })
    {
        INFO("object format " << format);
        const std::size_t hash_size { format == "sha256" ? 64u : 40u };
        test::temp_directory directory {};
        const std::filesystem::path path { directory.path() / "repo" };
        std::filesystem::create_directories(path);
        git_ok(path, { "init", "-q", "-b", "main", "--object-format=" + format });
        // 이름의 `[12]`는 glob으로 읽히면 `b1.txt`에 맞는다. 글자 그대로 그 파일 하나만 따라가야 한다.
        commit_file(path, "a[1].txt", "one\ntwo\nthree\nfour\nfive\nsix\n", "first");
        const std::string first { git_text(path, { "rev-parse", "HEAD" }) };
        commit_file(path, "b1.txt", "decoy\n", "decoy");
        const std::string decoy { git_text(path, { "rev-parse", "HEAD" }) };
        git_ok(path, { "mv", "a[1].txt", "b[12].txt" });
        commit_file(path, "b[12].txt", "one\nTWO\nthree\nfour\nfive\nsix\n", "rename");
        const std::string renamed { git_text(path, { "rev-parse", "HEAD" }) };
        commit_file(path, "b1.txt", "decoy\nchanged\n", "decoy change");
        commit_file(path, "b[12].txt", "one\nTWO\nthree\nfour\nfive\nsix\nseven\n", "append");
        const std::string head { git_text(path, { "rev-parse", "HEAD" }) };

        live_workspace live {};
        const std::string repo { live.open(path) };
        const repository_session& session { live.session(repo) };

        // 커밋의 blame: 이름을 바꾸기 전의 줄은 옛 경로의 커밋으로 거슬러 올라간다.
        const blame_file_request at_head { .path = "b[12].txt", .revision = head };
        live.require_succeeded(live.run(repo, at_head));
        REQUIRE(session.selected_blame().has_value());
        {
            const file_blame& blame { *session.selected_blame() };
            CHECK(blame.request == at_head);
            CHECK_FALSE(blame.binary);
            REQUIRE(blame.lines.size() == 7);
            REQUIRE(blame.origins.size() == 3);
            const blame_origin& one { blame.origins[blame.lines[0].origin] };
            CHECK(blame.lines[0].text == "one");
            CHECK(one.sha == first);
            CHECK(one.sha.size() == hash_size);
            CHECK(one.path == "a[1].txt");
            CHECK(one.summary == "first");
            CHECK(one.previous_sha.empty());
            const blame_origin& two { blame.origins[blame.lines[1].origin] };
            CHECK(blame.lines[1].text == "TWO");
            CHECK(two.sha == renamed);
            CHECK(two.path == "b[12].txt");
            // `previous`는 파일을 그 전에 바꾼 커밋이 아니라 이름을 바꾼 커밋의 부모이고, 거기서는 옛 경로다.
            CHECK(two.previous_sha == decoy);
            CHECK(two.previous_path == "a[1].txt");
            // 셋째 줄부터는 다시 첫 커밋이다. 커밋 정보가 다시 오지 않아도 같은 주인을 가리킨다.
            CHECK(blame.lines[2].origin == blame.lines[0].origin);
            CHECK(blame.lines[5].origin == blame.lines[0].origin);
            CHECK(blame.origins[blame.lines[6].origin].sha == head);
            CHECK(blame.lines[6].final_line == 7);
            for (const blame_origin& origin : blame.origins)
                CHECK_FALSE(origin.uncommitted);
        }

        // 작업 트리의 blame: 커밋하지 않은 줄은 0으로만 된 해시로 온다.
        test::write_file(path / "b[12].txt", "zero\none\nTWO\nthree\nfour\nfive\nsix\nseven\n");
        live.require_succeeded(live.run(repo, blame_file_request { .path = "b[12].txt" }));
        REQUIRE(session.selected_blame().has_value());
        {
            const file_blame& blame { *session.selected_blame() };
            CHECK(blame.request.revision.empty());
            REQUIRE(blame.lines.size() == 8);
            const blame_origin& zero { blame.origins[blame.lines[0].origin] };
            CHECK(blame.lines[0].text == "zero");
            CHECK(zero.uncommitted);
            CHECK(zero.sha == std::string(hash_size, '0'));
            CHECK(zero.previous_sha == head);
            CHECK(blame.lines[1].text == "one");
            CHECK(blame.lines[1].original_line == 1);
            CHECK(blame.lines[1].final_line == 2);
            for (std::size_t index { 1 }; index < blame.lines.size(); ++index)
                CHECK_FALSE(blame.origins[blame.lines[index].origin].uncommitted);
        }
        git_ok(path, { "checkout", "-q", "--", "b[12].txt" });

        // 파일 이력: 이름 바꾸기를 넘어 첫 커밋까지 가고, `b1.txt`만 고친 커밋은 섞이지 않는다.
        const file_history_request whole { .path = "b[12].txt" };
        live.require_succeeded(live.run(repo, whole));
        REQUIRE(session.selected_history().has_value());
        {
            const file_history& history { *session.selected_history() };
            CHECK(history.request == whole);
            CHECK_FALSE(history.truncated);
            REQUIRE(history.entries.size() == 3);
            CHECK(history.entries[0].commit.sha == head);
            CHECK(history.entries[0].commit.subject == "append");
            CHECK(history.entries[0].status == 'M');
            CHECK(history.entries[0].path == "b[12].txt");
            CHECK(history.entries[1].commit.sha == renamed);
            CHECK(history.entries[1].status == 'R');
            CHECK(history.entries[1].path == "b[12].txt");
            CHECK(history.entries[1].previous_path == "a[1].txt");
            CHECK(history.entries[2].commit.sha == first);
            CHECK(history.entries[2].commit.sha.size() == hash_size);
            CHECK(history.entries[2].status == 'A');
            CHECK(history.entries[2].path == "a[1].txt");
            CHECK(history.entries[2].commit.parents.empty());
        }

        // 상한보다 커밋이 많으면 상한까지만 주고 더 있다고 알린다.
        live.require_succeeded(live.run(repo, file_history_request { .path = "b[12].txt", .revision = {}, .max_count = 2 }));
        REQUIRE(session.selected_history().has_value());
        CHECK(session.selected_history()->truncated);
        REQUIRE(session.selected_history()->entries.size() == 2);
        CHECK(session.selected_history()->entries[1].commit.sha == renamed);

        // 옛 리비전에서는 그 리비전에서의 경로로 묻는다.
        live.require_succeeded(live.run(repo, file_history_request { .path = "a[1].txt", .revision = first }));
        REQUIRE(session.selected_history().has_value());
        REQUIRE(session.selected_history()->entries.size() == 1);
        CHECK(session.selected_history()->entries[0].commit.sha == first);
        CHECK_FALSE(session.selected_history()->truncated);
    }
}

TEST_CASE("integration: blame_file keeps CRLF and invalid UTF-8 bytes and reports binary files", "[session][integration][blame]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path path { make_repository(directory.path() / "repo") };
    git_ok(path, { "config", "core.autocrlf", "false" });
    commit_file(path, "crlf.txt", "keep\r\n\xFF\xFE not utf-8\r\nlast", "crlf");
    commit_file(path, "blob.bin", std::string_view { "a\0b\nline2\n", 10 }, "binary");

    live_workspace live {};
    const std::string repo { live.open(path) };
    const repository_session& session { live.session(repo) };

    live.require_succeeded(live.run(repo, blame_file_request { .path = "crlf.txt", .revision = "HEAD" }));
    REQUIRE(session.selected_blame().has_value());
    REQUIRE(session.selected_blame()->lines.size() == 3);
    CHECK(session.selected_blame()->lines[0].text == "keep\r");
    CHECK(session.selected_blame()->lines[1].text == "\xFF\xFE not utf-8\r");
    CHECK(session.selected_blame()->lines[2].text == "last");

    live.require_succeeded(live.run(repo, blame_file_request { .path = "blob.bin", .revision = "HEAD" }));
    REQUIRE(session.selected_blame().has_value());
    CHECK(session.selected_blame()->binary);
    CHECK(session.selected_blame()->lines.empty());
}

TEST_CASE("integration: blame_file and file_history end as failures when git cannot answer", "[session][integration][blame]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};

    SECTION("a missing path, a folder and a submodule path cannot be blamed")
    {
        const std::filesystem::path path { make_repository(directory.path() / "repo") };
        commit_file(path, "dir/a.txt", "one\n", "first");
        // 실제 submodule 없이 gitlink 항목만 만든다. blame에게는 같은 모양이다.
        git_ok(path, { "update-index", "--add", "--cacheinfo", "160000," + git_text(path, { "rev-parse", "HEAD" }) + ",sub" });
        git_ok(path, { "commit", "-q", "-m", "gitlink" });

        live_workspace live {};
        const std::string repo { live.open(path) };
        for (const std::string target : { "gone.txt", "dir", "sub" })
        {
            INFO("path " << target);
            const operation_id id { live.run(repo, blame_file_request { .path = target, .revision = "HEAD" }) };
            CHECK(live.status(id).state == operation_state::failed);
            CHECK(live.status(id).failure.has_value());
            CHECK_FALSE(live.session(repo).selected_blame().has_value());
        }
        // 작업 트리의 blame도 추적하지 않는 파일에는 실패한다.
        test::write_file(path / "untracked.txt", "new\n");
        CHECK(live.status(live.run(repo, blame_file_request { .path = "untracked.txt" })).state == operation_state::failed);

        // 이력은 없는 경로에 실패하지 않고 빈 목록을 준다.
        live.require_succeeded(live.run(repo, file_history_request { .path = "gone.txt" }));
        REQUIRE(live.session(repo).selected_history().has_value());
        CHECK(live.session(repo).selected_history()->entries.empty());
        CHECK_FALSE(live.session(repo).selected_history()->truncated);
    }

    SECTION("a repository without commits has nothing to blame and no history")
    {
        const std::filesystem::path path { make_repository(directory.path() / "empty") };
        test::write_file(path / "a.txt", "one\n");

        live_workspace live {};
        const std::string repo { live.open(path) };
        CHECK(live.status(live.run(repo, blame_file_request { .path = "a.txt" })).state == operation_state::failed);
        CHECK_FALSE(live.session(repo).selected_blame().has_value());
        CHECK(live.status(live.run(repo, file_history_request { .path = "a.txt" })).state == operation_state::failed);
        CHECK_FALSE(live.session(repo).selected_history().has_value());
    }
}

namespace {
    // `git fast-import`의 커밋 하나다. `run_git`의 환경은 작성자와 시각을 고정하므로, 작성자와 committer 시각이 다른 커밋은
    // 이 길로 만든다. 파일은 없다 (빈 트리). `parent`가 0이면 첫 커밋이다.
    std::string import_commit(const std::string& ref, const int mark, const int parent, const std::string& author, const std::int64_t time, const std::string& message)
    {
        const std::string who { author + " " + std::to_string(time) + " +0000\n" };
        std::string stream { "commit " + ref + "\nmark :" + std::to_string(mark) + "\n" };
        stream.append("author ").append(who).append("committer ").append(who);
        stream.append("data ").append(std::to_string(message.size())).append("\n").append(message).append("\n");
        if (parent != 0)
            stream.append("from :").append(std::to_string(parent)).append("\n");
        stream.append("\n");
        return stream;
    }

    text_list found_shas(const commit_search& search)
    {
        text_list shas {};
        for (const commit_info& commit : search.commits)
            shas.push_back(commit.sha);
        return shas;
    }
} // namespace

TEST_CASE("integration: search_commits finds commits by message, author and hash prefix over the refs of the graph in sha1 and sha256 repositories", "[session][integration][commit_search]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    for (const std::string format : { "sha1", "sha256" })
    {
        INFO("object format " << format);
        test::temp_directory directory {};
        const std::filesystem::path path { directory.path() / "repo" };
        std::filesystem::create_directories(path);
        git_ok(path, { "init", "-q", "-b", "main", "--object-format=" + format });

        const std::string morgan { "Morgan <morgan@example.com>" };
        const std::string robin { "Robin Lee <robin@example.org>" };
        const std::string sasha { "Sasha Quill <sasha@example.net>" };
        std::string stream {};
        stream.append(import_commit("refs/heads/main", 1, 0, morgan, 1700001000, "인사말을 고친다\n"));
        stream.append(import_commit("refs/heads/main", 2, 1, robin, 1700002000, "Fix Parser\n\nThe body mentions Kumquat only here.\n"));
        stream.append(import_commit("refs/heads/main", 3, 2, morgan, 1700003000, "regex a.*b literal\n"));
        stream.append(import_commit("refs/heads/main", 4, 3, morgan, 1700004000, "regex axxb expanded with --all\n"));
        // main에서 닿지 않는 커밋들이다: 다른 브랜치, 태그만, 원격 추적 브랜치만, 그리고 어느 ref에서도 닿지 않는 커밋.
        stream.append(import_commit("refs/heads/side", 5, 2, sasha, 1700005000, "side work for example.com\n"));
        stream.append(import_commit("refs/heads/tagged", 6, 4, morgan, 1700006000, "tagged only release\n"));
        stream.append(import_commit("refs/heads/lost", 7, 4, morgan, 1700007000, "lost commit nobody points at\n"));
        stream.append(import_commit("refs/heads/remote", 8, 4, morgan, 1700008000, "remote only topic\n"));
        git_ok(path, { "fast-import", "--quiet" }, stream);

        const std::string first { git_text(path, { "rev-parse", "main~3" }) };
        const std::string second { git_text(path, { "rev-parse", "main~2" }) };
        const std::string third { git_text(path, { "rev-parse", "main~1" }) };
        const std::string fourth { git_text(path, { "rev-parse", "main" }) };
        const std::string side { git_text(path, { "rev-parse", "side" }) };
        const std::string tagged { git_text(path, { "rev-parse", "tagged" }) };
        const std::string lost { git_text(path, { "rev-parse", "lost" }) };
        const std::string remote { git_text(path, { "rev-parse", "remote" }) };
        git_ok(path, { "tag", "v1", tagged });
        git_ok(path, { "update-ref", "refs/remotes/origin/topic", remote });
        git_ok(path, { "branch", "-q", "-D", "tagged", "lost", "remote" });

        live_workspace live {};
        const std::string repo { live.open(path) };
        const auto search = [&live, &repo](const std::string& query, const std::uint32_t max_count = 1000) {
            const search_commits_request request { .query = query, .max_count = max_count };
            live.require_succeeded(live.run(repo, request));
            REQUIRE(live.session(repo).selected_search().has_value());
            const commit_search found { *live.session(repo).selected_search() };
            CHECK(found.request == request);
            return found;
        };
        const auto find = [&search](const std::string& query) { return found_shas(search(query)); };

        // 제목의 한글.
        {
            const commit_search found { search("인사") };
            REQUIRE(found_shas(found) == text_list { first });
            CHECK_FALSE(found.truncated);
            CHECK(found.commits[0].subject == "인사말을 고친다");
            CHECK(found.commits[0].author_name == "Morgan");
            CHECK(found.commits[0].author_email == "morgan@example.com");
            CHECK(found.commits[0].committer_time == 1700001000);
            CHECK(found.commits[0].parents.empty());
        }
        // 본문에만 있는 낱말, 대소문자 무시. 결과에 본문은 싣지 않는다.
        {
            const commit_search found { search("kumquat") };
            REQUIRE(found_shas(found) == text_list { second });
            CHECK(found.commits[0].subject == "Fix Parser");
            CHECK_FALSE(found.commits[0].body.has_value());
            CHECK(found.commits[0].parents == text_list { first });
        }
        CHECK(find("fix parser") == text_list { second });
        CHECK(find("  FIX PARSER  ") == text_list { second });
        // 작성자의 이름과 메일.
        CHECK(find("robin lee") == text_list { second });
        CHECK(find("ROBIN@EXAMPLE.ORG") == text_list { second });
        // 정규식 글자는 글자 그대로다. `a.*b`는 `axxb`에 맞지 않는다.
        CHECK(find("a.*b") == text_list { third });
        CHECK(find(".*") == text_list { third });
        CHECK(find("^regex").empty());
        // 옵션처럼 보이는 질의도 찾을 글일 뿐이다. `--all`이 옵션으로 읽히면 닿지 않는 커밋까지 나온다.
        CHECK(find("--all") == text_list { fourth });
        // 새 커밋이 앞이다.
        CHECK(find("regex") == text_list { fourth, third });
        // 다른 브랜치, 태그, 원격 추적 브랜치에만 있는 커밋도 찾는다. 어느 ref에서도 닿지 않는 커밋은 없다.
        CHECK(find("side work") == text_list { side });
        CHECK(find("sasha quill") == text_list { side });
        CHECK(find("tagged only") == text_list { tagged });
        CHECK(find("remote only") == text_list { remote });
        CHECK(find("lost commit").empty());
        // 메시지에 맞은 커밋(side)과 작성자에 맞은 커밋(Morgan의 것)을 합친다. 닿지 않는 커밋은 여기에도 없다.
        CHECK(find("example.com") == text_list { remote, tagged, side, fourth, third, first });
        // 상한보다 많으면 새 것부터 상한까지만 주고 더 있다고 알린다.
        {
            const commit_search found { search("example.com", 2) };
            CHECK(found.truncated);
            CHECK(found_shas(found) == text_list { remote, tagged });
            const commit_search exact { search("regex", 2) };
            CHECK_FALSE(exact.truncated);
            CHECK(exact.commits.size() == 2);
        }

        // SHA 앞부분, 대문자, 전체 SHA.
        {
            const commit_search found { search(third.substr(0, 8)) };
            REQUIRE(found_shas(found) == text_list { third });
            CHECK(found.commits[0].subject == "regex a.*b literal");
            CHECK(found.commits[0].parents == text_list { second });
            CHECK_FALSE(found.commits[0].body.has_value());
            std::string upper { third.substr(0, 8) };
            std::transform(upper.begin(), upper.end(), upper.begin(), [](const char value) { return value >= 'a' && value <= 'f' ? static_cast<char>(value - 'a' + 'A') : value; });
            CHECK(find(upper) == text_list { third });
            CHECK(find(third) == text_list { third });
            CHECK(find(side.substr(0, 10)) == text_list { side });
            CHECK(find(tagged.substr(0, 10)) == text_list { tagged });
            CHECK(find(remote.substr(0, 10)) == text_list { remote });
        }
        // 어느 ref에서도 닿지 않는 커밋은 SHA로도 나오지 않는다. 그래프에 없어 화면이 갈 곳이 없다.
        CHECK(find(lost.substr(0, 10)).empty());
        CHECK(find(lost).empty());
        // 없는 SHA는 실패가 아니라 빈 결과다.
        CHECK(find(std::string(12, '0')).empty());

        // 브랜치 이름이 16진수처럼 생기면 git은 그 이름을 먼저 푼다. SHA가 맞은 것이 아니므로 그 커밋을 내지 않는다.
        git_ok(path, { "branch", "cafe", side });
        CHECK(find("cafe").empty());
        git_ok(path, { "branch", "-q", "-D", "cafe" });

        // `.mailmap`이 이름을 바꿔도 그래프가 보이는 이름(`%an`)으로 찾는다.
        test::write_file(path / ".mailmap", "Renamed Person <renamed@example.org> Robin Lee <robin@example.org>\n");
        CHECK(find("robin lee") == text_list { second });
        CHECK(find("renamed person").empty());
        std::filesystem::remove(path / ".mailmap");

        // HEAD도 그래프의 시작점이다. HEAD만 가리키는 커밋은 찾는다.
        git_ok(path, { "checkout", "-q", "--detach", lost });
        live.refresh(repo);
        CHECK(find("lost commit") == text_list { lost });
        CHECK(find(lost.substr(0, 10)) == text_list { lost });

        // 첫 커밋이 없는 브랜치에서는 HEAD를 풀 수 없다. HEAD 없이 다른 ref에서 찾는다.
        git_ok(path, { "checkout", "-q", "--orphan", "blank" });
        live.refresh(repo);
        REQUIRE(live.session(repo).snapshot().summary.head_sha.empty());
        CHECK(find("인사") == text_list { first });
        CHECK(find(third.substr(0, 8)) == text_list { third });
        CHECK(find("lost commit").empty());
        CHECK(find(lost.substr(0, 10)).empty());
    }
}

TEST_CASE("integration: search_commits in a repository without commits finds nothing and a blank query fails", "[session][integration][commit_search]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path path { make_repository(directory.path() / "empty") };

    live_workspace live {};
    const std::string repo { live.open(path) };
    // ref도 HEAD도 없다. git은 실패하지 않고 빈 출력을 준다.
    for (const std::string query : { "anything", "abcd1234" })
    {
        INFO("query " << query);
        live.require_succeeded(live.run(repo, search_commits_request { .query = query }));
        REQUIRE(live.session(repo).selected_search().has_value());
        CHECK(live.session(repo).selected_search()->commits.empty());
        CHECK_FALSE(live.session(repo).selected_search()->truncated);
    }

    const operation_id blank { live.run(repo, search_commits_request { .query = "  " }) };
    CHECK(live.status(blank).state == operation_state::failed);
    REQUIRE(live.status(blank).failure.has_value());
    CHECK(live.status(blank).failure->kind == failure_kind::invalid_request);
    CHECK_FALSE(live.session(repo).selected_search().has_value());
}

TEST_CASE("integration: set_upstream connects a pushed branch without pushing and unset_upstream removes it", "[session][integration][upstream]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    remote_fixture fixture {};
    live_workspace live {};
    const std::string repo { live.open(fixture.one) };

    git_ok(fixture.one, { "switch", "-q", "-c", "feature" });
    commit_file(fixture.one, "f.txt", "f\n", "feature commit");
    // upstream 없이 push한다 (-u를 쓰지 않는다).
    git_ok(fixture.one, { "push", "-q", "origin", "feature" });
    live.refresh(repo);
    const ref* feature { find_ref(live.session(repo), "refs/heads/feature") };
    REQUIRE(feature != nullptr);
    CHECK(feature->upstream.empty());

    const std::string remote_head_before { git_text(fixture.bare, { "rev-parse", "refs/heads/feature" }) };
    live.require_succeeded(live.run(repo, set_upstream_request { .branch = "feature", .remote = "origin", .remote_branch = "feature" }));
    feature = find_ref(live.session(repo), "refs/heads/feature");
    REQUIRE(feature != nullptr);
    CHECK(feature->upstream == "origin/feature");
    CHECK(feature->ahead == 0);
    CHECK(feature->behind == 0);
    CHECK(live.session(repo).snapshot().summary.has_upstream);
    // push하지 않았다.
    CHECK(git_text(fixture.bare, { "rev-parse", "refs/heads/feature" }) == remote_head_before);

    // 로컬에 커밋을 더하면 앞섬이 보인다.
    commit_file(fixture.one, "g.txt", "g\n", "ahead commit");
    live.refresh(repo);
    CHECK(find_ref(live.session(repo), "refs/heads/feature")->ahead == 1);
    CHECK(live.session(repo).snapshot().summary.ahead == 1);

    live.require_succeeded(live.run(repo, unset_upstream_request { "feature" }));
    feature = find_ref(live.session(repo), "refs/heads/feature");
    REQUIRE(feature != nullptr);
    CHECK(feature->upstream.empty());
    CHECK_FALSE(live.session(repo).snapshot().summary.has_upstream);
}

TEST_CASE("integration: set_upstream refuses a remote branch that does not exist and a bad name", "[session][integration][upstream]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    remote_fixture fixture {};
    live_workspace live {};
    const std::string repo { live.open(fixture.one) };

    const operation_id missing { live.run(repo, set_upstream_request { .branch = "main", .remote = "origin", .remote_branch = "nowhere" }) };
    CHECK(live.status(missing).state == operation_state::failed);
    const operation_id option_like { live.run(repo, set_upstream_request { .branch = "main", .remote = "origin", .remote_branch = "--evil" }) };
    CHECK(live.status(option_like).state == operation_state::failed);
    REQUIRE(live.status(option_like).failure.has_value());
    CHECK(live.status(option_like).failure->kind == failure_kind::invalid_request);
    // 실패해도 기존 upstream은 그대로다.
    git_ok(fixture.one, { "branch", "--set-upstream-to=origin/main", "main" });
    live.refresh(repo);
    CHECK(find_ref(live.session(repo), "refs/heads/main")->upstream == "origin/main");
}

TEST_CASE("integration: add_remote registers a remote and the chained push publishes the branch with an upstream", "[session][integration][upstream]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path bare { directory.path() / "published.git" };
    git_ok(directory.path(), { "init", "-q", "--bare", "-b", "main", bare.string() });
    const std::filesystem::path repo_path { make_repository(directory.path() / "local") };
    commit_file(repo_path, "a.txt", "one\n", "first");
    live_workspace live {};
    const std::string repo { live.open(repo_path) };
    CHECK(live.session(repo).snapshot().summary.remotes.empty());

    live.require_succeeded(live.run(repo, add_remote_request { .name = "origin", .url = bare.string() }));
    const std::vector<remote_info>& remotes { live.session(repo).snapshot().summary.remotes };
    REQUIRE(remotes.size() == 1);
    CHECK(remotes.front().name == "origin");
    // 추가만 했고 아직 push하지 않았다.
    CHECK(git_text(bare, { "for-each-ref", "refs/heads" }).empty());

    live.require_succeeded(live.run(repo, push_execute_request { .remote = "origin", .branch = "main", .remote_branch = "main", .set_upstream = true }));
    CHECK(git_text(bare, { "rev-parse", "refs/heads/main" }) == git_text(repo_path, { "rev-parse", "HEAD" }));
    const ref* const main { find_ref(live.session(repo), "refs/heads/main") };
    REQUIRE(main != nullptr);
    CHECK(main->upstream == "origin/main");
    CHECK(live.session(repo).snapshot().summary.has_upstream);

    // 같은 이름은 git이 거절하고, 옵션처럼 보이는 이름·URL은 git을 부르기 전에 거절한다.
    const operation_id duplicate { live.run(repo, add_remote_request { .name = "origin", .url = bare.string() }) };
    CHECK(live.status(duplicate).state == operation_state::failed);
    const operation_id option_name { live.run(repo, add_remote_request { .name = "--evil", .url = bare.string() }) };
    REQUIRE(live.status(option_name).failure.has_value());
    CHECK(live.status(option_name).failure->kind == failure_kind::invalid_request);
    const operation_id option_url { live.run(repo, add_remote_request { .name = "mirror", .url = "--upload-pack=evil" }) };
    REQUIRE(live.status(option_url).failure.has_value());
    CHECK(live.status(option_url).failure->kind == failure_kind::invalid_request);
    CHECK(live.session(repo).snapshot().summary.remotes.size() == 1);
}

TEST_CASE("integration: a file with a textconv diff driver is flagged and diffed with the configured context", "[session][integration][textconv]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path repo_path { make_repository(directory.path() / "repo") };
    test::write_file(repo_path / ".gitattributes", "*.dat diff=upper\n*.txt diff=plain\n");
    git_ok(repo_path, { "config", "diff.upper.textconv", "tr a-z A-Z <" });
    git_ok(repo_path, { "config", "diff.plain.binary", "false" });
    git_ok(repo_path, { "add", ".gitattributes" });
    git_ok(repo_path, { "commit", "-q", "-m", "attributes" });
    commit_file(repo_path, "data.dat", "one\ntwo\nthree\nfour\nfive\nsix\nseven\n", "data");
    commit_file(repo_path, "plain.txt", "one\ntwo\nthree\nfour\nfive\nsix\nseven\n", "plain");
    commit_file(repo_path, "other.log", "one\ntwo\nthree\nfour\nfive\nsix\nseven\n", "other");
    for (const char* const name : { "data.dat", "plain.txt", "other.log" })
        test::write_file(repo_path / name, "one\ntwo\nthree\n4\nfive\nsix\nseven\n");

    live_workspace live {};
    const std::string repo { live.open(repo_path) };

    live.require_succeeded(live.run(repo, diff_file_request { .path = "data.dat", .view = diff_view::unstaged, .sha = {}, .untracked = false, .context_lines = 1 }));
    REQUIRE(live.session(repo).selected_diff().has_value());
    CHECK(live.session(repo).selected_diff()->textconv_driver == "upper");
    // textconv를 거친 글이라 바뀐 줄이 대문자로 보이고, 문맥은 위아래 한 줄이다.
    REQUIRE(live.session(repo).selected_diff()->files.size() == 1);
    const diff_hunk& hunk { live.session(repo).selected_diff()->files.front().hunks.front() };
    CHECK(hunk.lines.size() == 4);

    // 드라이버는 있지만 textconv가 없으면 배지가 없다.
    live.require_succeeded(live.run(repo, diff_file_request { .path = "plain.txt", .view = diff_view::unstaged }));
    REQUIRE(live.session(repo).selected_diff().has_value());
    CHECK(live.session(repo).selected_diff()->textconv_driver.empty());
    CHECK(live.session(repo).selected_diff()->files.front().hunks.front().lines.size() == 8);

    // 속성이 없는 파일도 그렇다.
    live.require_succeeded(live.run(repo, diff_file_request { .path = "other.log", .view = diff_view::unstaged, .context_lines = 0 }));
    CHECK(live.session(repo).selected_diff()->textconv_driver.empty());
    CHECK(live.session(repo).selected_diff()->files.front().hunks.front().lines.size() == 2);
}
