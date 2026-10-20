// PR·MR 체크아웃을 실제 git과 로컬 bare 원격으로 돌린다. 원격에는 `update-ref`로 `refs/pull/<n>/head`·`refs/merge-requests/<n>/head`를
// 만들어 서비스가 내는 ref를 흉내 낸다. 네트워크는 쓰지 않는다. 모두 `[integration]` 태그를 가지며 git이 없으면 건너뛴다.
#include "application/git_workspace.h"
#include "infrastructure/git_operation_runner.h"
#include "infrastructure/git_tool_discovery.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_process_runner.h"
#include "platform/win32/win32_workspace_files.h"

#include "fake_git_runner.h"
#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {
    using namespace cyella;
    using cyella::test::run_git;
    using cyella::test::text_list;

    void git_ok(const std::filesystem::path& directory, const text_list& arguments)
    {
        const test::git_run_result result { run_git(directory, arguments) };
        INFO("stderr: " << result.standard_error);
        REQUIRE(result.exit_code == 0);
    }

    std::string git_text(const std::filesystem::path& directory, const text_list& arguments)
    {
        const test::git_run_result result { run_git(directory, arguments) };
        REQUIRE(result.exit_code == 0);
        std::string text { result.standard_output };
        while (text.empty() == false && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        return text;
    }

    class live_workspace
    {
    public:
        live_workspace()
            : process_ { win32::make_process_runner() }
            , files_ { win32::make_workspace_files() }
        {
            const std::unique_ptr<file_probe> probe { win32::make_file_probe() };
            tool_ = resolve_git_tool({}, win32::current_git_tool_environment(), *process_, *probe, {});
            if (tool_.usable() == false)
                return;
            executor_ = std::make_unique<git_operation_runner>(*process_, tool_.executable, *files_, git_operation_runner_options {});
            git_workspace_options options {};
            options.refresh_on_open = false;
            workspace_ = std::make_unique<git_workspace>(*executor_, dispatcher_.dispatcher(), events_.sink(), nullptr, options);
        }

        [[nodiscard]] bool available() const noexcept
        {
            return workspace_ != nullptr;
        }

        std::string open(const std::filesystem::path& path)
        {
            const std::string id { workspace_->open_repository(path.generic_u8string()) };
            static_cast<void>(run(id, refresh_request {}));
            return id;
        }

        operation_id run(const std::string& repository, git_operation operation)
        {
            const operation_id id { workspace_->submit(repository, std::move(operation)) };
            REQUIRE(id != 0);
            settle();
            return id;
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

    private:
        std::unique_ptr<process_runner> process_ {};
        std::unique_ptr<workspace_files> files_ {};
        git_tool_info tool_ {};
        std::unique_ptr<git_operation_runner> executor_ {};
        test::manual_dispatcher dispatcher_ {};
        test::event_queue events_ {};
        std::unique_ptr<git_workspace> workspace_ {};
    };

    // bare 원격, 커밋을 만드는 작업 저장소(`author`), 사용자의 clone(`local`)이다. author의 `feature` 브랜치 끝을 원격의 PR ref로 올린다.
    struct pull_fixture
    {
        pull_fixture()
        {
            git_ok(directory.path(), { "init", "-q", "--bare", "-b", "main", bare.string() });
            git_ok(directory.path(), { "clone", "-q", bare.string(), author.string() });
            test::write_file(author / "a.txt", "one\n");
            git_ok(author, { "add", "a.txt" });
            git_ok(author, { "commit", "-q", "-m", "first" });
            git_ok(author, { "push", "-q", "origin", "main" });
            git_ok(author, { "switch", "-q", "-c", "feature" });
            test::write_file(author / "pr.txt", "from the pull request\n");
            git_ok(author, { "add", "pr.txt" });
            git_ok(author, { "commit", "-q", "-m", "pull request change" });
            feature_sha = git_text(author, { "rev-parse", "HEAD" });
            // PR 브랜치를 원격에 올리지 않고 ref만 만든다 (서비스는 PR ref를 따로 둔다).
            git_ok(author, { "push", "-q", "origin", "feature:refs/pull/7/head", "feature:refs/merge-requests/8/head" });
            git_ok(directory.path(), { "clone", "-q", bare.string(), local.string() });
        }

        test::temp_directory directory {};
        std::filesystem::path bare { directory.path() / "origin.git" };
        std::filesystem::path author { directory.path() / "author" };
        std::filesystem::path local { directory.path() / "local" };
        std::string feature_sha {};
    };
} // namespace

TEST_CASE("integration: a GitHub pull request ref is fetched into pr/N and checked out", "[hosting][integration]")
{
    live_workspace live {};
    if (live.available() == false)
        SKIP("git is not installed");
    pull_fixture fixture {};
    const std::string id { live.open(fixture.local) };
    REQUIRE(live.session(id).snapshot().summary.current_branch == "main");

    const operation_id fetch { live.run(id, fetch_request { .remote = "origin", .prune = false, .refspec = "pull/7/head:pr/7" }) };
    REQUIRE(live.status(fetch).state == operation_state::succeeded);
    // 가져온 것은 지정한 ref 하나뿐이다: 로컬 브랜치 pr/7이 생겼고 현재 브랜치와 워킹 트리는 그대로다.
    CHECK(git_text(fixture.local, { "rev-parse", "pr/7" }) == fixture.feature_sha);
    CHECK(git_text(fixture.local, { "rev-parse", "--abbrev-ref", "HEAD" }) == "main");
    CHECK(std::filesystem::exists(fixture.local / "pr.txt") == false);
    // 실행한 명령줄은 refspec 하나다 (--prune 없음).
    const operation_status& fetched { live.status(fetch) };
    REQUIRE(fetched.commands.empty() == false);
    const std::string command { fetched.commands.front().command_line.begin(), fetched.commands.front().command_line.end() };
    CHECK(command.find("pull/7/head:pr/7") != std::string::npos);
    CHECK(command.find("--prune") == std::string::npos);

    const operation_id checkout { live.run(id, checkout_branch_request { .name = "pr/7" }) };
    REQUIRE(live.status(checkout).state == operation_state::succeeded);
    CHECK(live.session(id).snapshot().summary.current_branch == "pr/7");
    CHECK(test::read_file(fixture.local / "pr.txt") == "from the pull request\n");
    CHECK(live.session(id).snapshot().summary.head_sha == fixture.feature_sha);
}

TEST_CASE("integration: a GitLab merge request ref is fetched into mr/N", "[hosting][integration]")
{
    live_workspace live {};
    if (live.available() == false)
        SKIP("git is not installed");
    pull_fixture fixture {};
    const std::string id { live.open(fixture.local) };
    const operation_id fetch { live.run(id, fetch_request { .remote = "origin", .prune = false, .refspec = "merge-requests/8/head:mr/8" }) };
    REQUIRE(live.status(fetch).state == operation_state::succeeded);
    CHECK(git_text(fixture.local, { "rev-parse", "mr/8" }) == fixture.feature_sha);
    REQUIRE(live.status(live.run(id, checkout_branch_request { .name = "mr/8" })).state == operation_state::succeeded);
    CHECK(live.session(id).snapshot().summary.current_branch == "mr/8");
}

TEST_CASE("integration: fetching a pull request again fast-forwards a moved ref and keeps local work safe", "[hosting][integration]")
{
    live_workspace live {};
    if (live.available() == false)
        SKIP("git is not installed");
    pull_fixture fixture {};
    const std::string id { live.open(fixture.local) };
    REQUIRE(live.status(live.run(id, fetch_request { .remote = "origin", .prune = false, .refspec = "pull/7/head:pr/7" })).state == operation_state::succeeded);

    // 작성자가 PR에 커밋을 더 올렸다: 같은 refspec이 앞으로 감기로 따라간다.
    test::write_file(fixture.author / "pr.txt", "second revision\n");
    git_ok(fixture.author, { "commit", "-q", "-am", "revision two" });
    const std::string second { git_text(fixture.author, { "rev-parse", "HEAD" }) };
    git_ok(fixture.author, { "push", "-q", "origin", "HEAD:refs/pull/7/head" });
    REQUIRE(live.status(live.run(id, fetch_request { .remote = "origin", .prune = false, .refspec = "pull/7/head:pr/7" })).state == operation_state::succeeded);
    CHECK(git_text(fixture.local, { "rev-parse", "pr/7" }) == second);

    // 사용자가 pr/7에 로컬 커밋을 더한 뒤 PR이 강제 push되면, 덮어쓰지 않고 거절된다.
    git_ok(fixture.local, { "switch", "-q", "pr/7" });
    test::write_file(fixture.local / "local.txt", "my work\n");
    git_ok(fixture.local, { "add", "local.txt" });
    git_ok(fixture.local, { "commit", "-q", "-m", "my local commit" });
    const std::string mine { git_text(fixture.local, { "rev-parse", "HEAD" }) };
    git_ok(fixture.local, { "switch", "-q", "main" });
    git_ok(fixture.author, { "reset", "-q", "--hard", "HEAD~1" });
    git_ok(fixture.author, { "push", "-q", "-f", "origin", "HEAD:refs/pull/7/head" });
    const operation_id rejected { live.run(id, fetch_request { .remote = "origin", .prune = false, .refspec = "pull/7/head:pr/7" }) };
    CHECK(live.status(rejected).state == operation_state::failed);
    CHECK(git_text(fixture.local, { "rev-parse", "pr/7" }) == mine);
}
