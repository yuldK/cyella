// 인터랙티브 rebase를 실제 git·편집기 도우미(`cyella-rebase-editor.exe`)·임시 저장소로 끝까지 돌린다. 실행기는 실제
// Win32 process runner를 쓰고, job은 수동 dispatcher가 test 스레드에서 차례로 실행한다. 모두 `[integration]` 태그를
// 가지며 git이 없으면 건너뛴다.
#include "application/git_workspace.h"
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

#ifndef CYELLA_REBASE_EDITOR_PATH
#error "CYELLA_REBASE_EDITOR_PATH must be defined by the build."
#endif

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
        INFO("git " << arguments.front() << " stderr: " << result.standard_error);
        REQUIRE(result.exit_code == 0);
    }

    std::string git_text(const std::filesystem::path& directory, const text_list& arguments)
    {
        const test::git_run_result result { run_git(directory, arguments) };
        INFO("stderr: " << result.standard_error);
        REQUIRE(result.exit_code == 0);
        return trimmed(result.standard_output);
    }

    // 오래된 것부터의 제목이다.
    text_list subjects(const std::filesystem::path& repo, const std::string& range)
    {
        const std::string text { git_text(repo, { "log", "--reverse", "--format=%s", range }) };
        text_list lines {};
        std::size_t begin { 0 };
        while (begin <= text.size() && text.empty() == false)
        {
            const std::size_t end { text.find('\n', begin) };
            lines.push_back(text.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
            if (end == std::string::npos)
                break;
            begin = end + 1;
        }
        return lines;
    }

    std::string full_message(const std::filesystem::path& repo, const std::string& revision)
    {
        const test::git_run_result result { run_git(repo, text_list { "log", "-1", "--format=%B", revision }) };
        REQUIRE(result.exit_code == 0);
        return result.standard_output;
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

    // main 위에 feature가 커밋 넷(one·two·three·four)을 가진다. 서로 다른 파일을 고쳐 순서를 바꿔도 충돌하지 않는다.
    std::filesystem::path make_linear_feature(const std::filesystem::path& root)
    {
        const std::filesystem::path repo { make_repository(root / "repo") };
        commit_file(repo, "base.txt", "base\n", "base");
        git_ok(repo, { "switch", "-q", "-c", "feature" });
        commit_file(repo, "one.txt", "1\n", "one");
        commit_file(repo, "two.txt", "2\n", "two");
        commit_file(repo, "three.txt", "3\n", "three");
        commit_file(repo, "four.txt", "4\n", "four");
        return repo;
    }

    // feature의 둘째 커밋이 main과 같은 줄을 고쳐 둘째 단계에서 충돌한다. 셋째 커밋은 다른 파일이다.
    std::filesystem::path make_conflicting_feature(const std::filesystem::path& root)
    {
        const std::filesystem::path repo { make_repository(root / "repo") };
        commit_file(repo, "a.txt", "1\n2\n3\n", "init");
        git_ok(repo, { "switch", "-q", "-c", "feature" });
        commit_file(repo, "f.txt", "f1\n", "feat one");
        commit_file(repo, "a.txt", "feature\n2\n3\n", "feat two");
        commit_file(repo, "g.txt", "g\n", "feat three");
        git_ok(repo, { "switch", "-q", "main" });
        commit_file(repo, "a.txt", "main\n2\n3\n", "main change");
        git_ok(repo, { "switch", "-q", "feature" });
        return repo;
    }

    class live_workspace
    {
    public:
        explicit live_workspace(const std::filesystem::path& helper = std::filesystem::path { CYELLA_REBASE_EDITOR_PATH })
            : process_ { win32::make_process_runner() }
            , files_ { win32::make_workspace_files() }
        {
            const std::unique_ptr<file_probe> probe { win32::make_file_probe() };
            tool_ = resolve_git_tool({}, win32::current_git_tool_environment(), *process_, *probe, {});
            REQUIRE(tool_.usable());
            git_operation_runner_options options {};
            options.rebase_editor_helper = helper.u8string();
            executor_ = std::make_unique<git_operation_runner>(*process_, tool_.executable, *files_, options);
            git_workspace_options workspace_options {};
            workspace_options.refresh_on_open = false;
            workspace_ = std::make_unique<git_workspace>(*executor_, dispatcher_.dispatcher(), events_.sink(), nullptr, workspace_options);
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

        [[nodiscard]] rebase_plan plan(const std::string& repository, std::string onto, std::string branch = {})
        {
            const operation_id id { run(repository, load_rebase_plan_request { std::move(onto), std::move(branch) }) };
            require_succeeded(id);
            const rebase_plan* const found { workspace_->rebase_plan_of(id) };
            REQUIRE(found != nullptr);
            return *found;
        }

        void require_succeeded(const operation_id id) const
        {
            const operation_status& found { status(id) };
            if (found.failure.has_value())
            {
                const std::string summary(found.failure->summary.begin(), found.failure->summary.end());
                const std::string detail(found.failure->detail.begin(), found.failure->detail.end());
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

    // 범위에서 제목으로 커밋을 찾는다.
    const rebase_commit& by_subject(const rebase_plan& plan, const std::string_view subject)
    {
        const auto found { std::find_if(plan.commits.begin(), plan.commits.end(), [subject](const rebase_commit& commit) { return commit.subject == subject; }) };
        INFO("commit subject: " << subject);
        REQUIRE(found != plan.commits.end());
        return *found;
    }

    rebase_todo_entry entry(const rebase_action action, const rebase_commit& commit, std::optional<std::string> message = std::nullopt)
    {
        return rebase_todo_entry { action, commit.sha, std::move(message) };
    }

    interactive_rebase_request request_of(std::string onto, std::string branch, std::vector<rebase_todo_entry> todo)
    {
        interactive_rebase_request request {};
        request.onto = std::move(onto);
        request.branch = std::move(branch);
        request.todo = std::move(todo);
        return request;
    }

    bool rebase_in_progress(const std::filesystem::path& repo)
    {
        return std::filesystem::exists(repo / ".git" / "rebase-merge");
    }
} // namespace

TEST_CASE("integration: the rebase plan lists the commits git would rebase, oldest first", "[rebase][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path repo { make_linear_feature(directory.path()) };
    commit_file(repo, "one.txt", "1\n1b\n", "fixup! one");
    live_workspace live {};
    const std::string id { live.open(repo) };

    const rebase_plan plan { live.plan(id, "main", "feature") };
    REQUIRE(plan.commits.size() == 5);
    CHECK(plan.commits.front().subject == "one");
    CHECK(plan.commits.back().subject == "fixup! one");
    CHECK(plan.commits.back().sha == git_text(repo, { "rev-parse", "feature" }));
    CHECK(plan.onto_sha == git_text(repo, { "rev-parse", "main" }));
    CHECK(plan.head_sha == git_text(repo, { "rev-parse", "feature" }));
    CHECK(plan.merge_count == 0);
    CHECK_FALSE(plan.commits.front().pushed);
    CHECK_FALSE(plan.has_remote_branches);
    CHECK(autosquash_marker_of(plan.commits.back().subject) == autosquash_marker::fixup);
    const std::vector<rebase_todo_entry> arranged { autosquash_rebase_todo(plan.commits) };
    CHECK(arranged[1].sha == plan.commits.back().sha);
    CHECK(arranged[1].action == rebase_action::fixup);
}

TEST_CASE("integration: interactive rebase reorders three commits", "[rebase][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path repo { make_linear_feature(directory.path()) };
    live_workspace live {};
    const std::string id { live.open(repo) };
    const rebase_plan plan { live.plan(id, "main", "feature") };

    const std::vector<rebase_todo_entry> todo_list {
        entry(rebase_action::pick, by_subject(plan, "three")),
        entry(rebase_action::pick, by_subject(plan, "one")),
        entry(rebase_action::pick, by_subject(plan, "two")),
        entry(rebase_action::pick, by_subject(plan, "four")),
    };
    const operation_id run { live.run(id, request_of("main", "feature", todo_list)) };
    live.require_succeeded(run);
    CHECK(subjects(repo, "main..feature") == text_list { "three", "one", "two", "four" });
    CHECK_FALSE(rebase_in_progress(repo));
    CHECK(git_text(repo, { "status", "--porcelain" }).empty());
}

TEST_CASE("integration: interactive rebase drops a commit", "[rebase][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path repo { make_linear_feature(directory.path()) };
    live_workspace live {};
    const std::string id { live.open(repo) };
    const rebase_plan plan { live.plan(id, "main", "feature") };

    const std::vector<rebase_todo_entry> todo_list {
        entry(rebase_action::pick, by_subject(plan, "one")),
        entry(rebase_action::drop, by_subject(plan, "two")),
        entry(rebase_action::pick, by_subject(plan, "three")),
        entry(rebase_action::pick, by_subject(plan, "four")),
    };
    live.require_succeeded(live.run(id, request_of("main", "feature", todo_list)));
    CHECK(subjects(repo, "main..feature") == text_list { "one", "three", "four" });
    CHECK_FALSE(std::filesystem::exists(repo / "two.txt"));
}

TEST_CASE("integration: interactive rebase squashes two commits with a custom message and fixes up another", "[rebase][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path repo { make_linear_feature(directory.path()) };
    live_workspace live {};
    const std::string id { live.open(repo) };
    const rebase_plan plan { live.plan(id, "main", "feature") };
    const std::string one_sha { by_subject(plan, "one").sha };

    const std::vector<rebase_todo_entry> todo_list {
        entry(rebase_action::pick, by_subject(plan, "one")),
        entry(rebase_action::squash, by_subject(plan, "two"), std::string { "one and two\n\nsquashed by cyella" }),
        entry(rebase_action::pick, by_subject(plan, "three")),
        entry(rebase_action::fixup, by_subject(plan, "four")),
    };
    const operation_id run { live.run(id, request_of("main", "feature", todo_list)) };
    live.require_succeeded(run);
    CHECK(subjects(repo, "main..feature") == text_list { "one and two", "three" });
    CHECK(full_message(repo, "feature~1") == "one and two\n\nsquashed by cyella\n\n");
    // fixup은 앞 커밋의 메시지를 지키고 내용만 합친다.
    CHECK(full_message(repo, "feature") == "three\n\n");
    CHECK(git_text(repo, { "show", "--format=", "--name-only", "feature" }) == "four.txt\nthree.txt");
    CHECK(git_text(repo, { "show", "--format=", "--name-only", "feature~1" }) == "one.txt\ntwo.txt");
    CHECK(live.status(run).result_summary.find(u8"4개 → 2개") != std::u8string::npos);
}

TEST_CASE("integration: interactive rebase rewords with a Korean multi-line message without CR", "[rebase][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path repo { make_linear_feature(directory.path()) };
    live_workspace live {};
    const std::string id { live.open(repo) };
    const rebase_plan plan { live.plan(id, "main", "feature") };

    const std::string message { "기능: 첫 커밋을 다시 쓴다\r\n\r\n- 본문 첫 줄\r\n- 둘째 줄 (한글)\r\n" };
    const std::vector<rebase_todo_entry> todo_list {
        entry(rebase_action::reword, by_subject(plan, "one"), message),
        entry(rebase_action::pick, by_subject(plan, "two")),
        entry(rebase_action::pick, by_subject(plan, "three")),
        entry(rebase_action::pick, by_subject(plan, "four")),
    };
    live.require_succeeded(live.run(id, request_of("main", "feature", todo_list)));
    const std::string written { full_message(repo, "feature~3") };
    CHECK(written == "기능: 첫 커밋을 다시 쓴다\n\n- 본문 첫 줄\n- 둘째 줄 (한글)\n\n");
    CHECK(written.find('\r') == std::string::npos);
    CHECK(subjects(repo, "main..feature") == text_list { "기능: 첫 커밋을 다시 쓴다", "two", "three", "four" });
    // 바꾸지 않은 커밋은 그대로다 (같은 부모 위의 pick은 새로 만들어지지 않을 수 있지만 내용은 같다).
    CHECK(full_message(repo, "feature") == "four\n\n");
}

TEST_CASE("integration: a conflict mid-sequence waits, and continue still applies the later reword", "[rebase][integration][conflict]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path repo { make_conflicting_feature(directory.path()) };
    live_workspace live {};
    const std::string id { live.open(repo) };
    const rebase_plan plan { live.plan(id, "main", "feature") };
    REQUIRE(plan.commits.size() == 3);
    const std::string two_sha { by_subject(plan, "feat two").sha };

    const std::vector<rebase_todo_entry> todo_list {
        entry(rebase_action::pick, by_subject(plan, "feat one")),
        entry(rebase_action::pick, by_subject(plan, "feat two")),
        entry(rebase_action::reword, by_subject(plan, "feat three"), std::string { "셋째를 고쳐 쓴다" }),
    };
    const operation_id run { live.run(id, request_of("main", "feature", todo_list)) };
    const operation_status& waiting { live.status(run) };
    REQUIRE(waiting.state == operation_state::needs_input);
    REQUIRE(waiting.needs_input.has_value());
    CHECK(waiting.needs_input->kind == input_kind::conflict);
    CHECK(waiting.needs_input->sequence == repository_operation_state::rebase);

    const in_progress_operation& stopped { live.session(id).snapshot().summary.in_progress };
    CHECK(stopped.kind == repository_operation_state::rebase);
    CHECK(stopped.step == 2);
    CHECK(stopped.total == 3);
    CHECK(stopped.interactive);
    CHECK(stopped.cyella_messages);
    CHECK(stopped.current_action == "pick");
    CHECK(stopped.current_sha == two_sha);

    live.require_succeeded(live.run(id, resolve_file_request { "a.txt", conflict_side::theirs }));
    live.require_succeeded(live.run(id, sequence_control_request { operation_kind::rebase_continue }));
    CHECK_FALSE(rebase_in_progress(repo));
    CHECK(subjects(repo, "main..feature") == text_list { "feat one", "feat two", "셋째를 고쳐 쓴다" });
    // 충돌을 푼 pick은 원래 메시지를 지킨다 (continue가 연 편집기에 메시지 도우미가 손대지 않는다).
    CHECK(full_message(repo, "feature~1") == "feat two\n\n");
    CHECK(live.session(id).snapshot().summary.in_progress.kind == repository_operation_state::none);
}

TEST_CASE("integration: aborting an interactive rebase mid-way restores the original HEAD", "[rebase][integration][conflict]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path repo { make_conflicting_feature(directory.path()) };
    const std::string before { git_text(repo, { "rev-parse", "feature" }) };
    live_workspace live {};
    const std::string id { live.open(repo) };
    const rebase_plan plan { live.plan(id, "main", "feature") };

    const std::vector<rebase_todo_entry> todo_list {
        entry(rebase_action::reword, by_subject(plan, "feat one"), std::string { "바뀐 첫째" }),
        entry(rebase_action::pick, by_subject(plan, "feat two")),
        entry(rebase_action::pick, by_subject(plan, "feat three")),
    };
    const operation_id run { live.run(id, request_of("main", "feature", todo_list)) };
    REQUIRE(live.status(run).state == operation_state::needs_input);
    REQUIRE(rebase_in_progress(repo));
    CHECK(std::filesystem::exists(repo / ".git" / "rebase-merge" / "cyella" / "messages"));

    live.require_succeeded(live.run(id, sequence_control_request { operation_kind::rebase_abort }));
    CHECK_FALSE(rebase_in_progress(repo));
    CHECK(git_text(repo, { "rev-parse", "HEAD" }) == before);
    CHECK(git_text(repo, { "symbolic-ref", "--short", "HEAD" }) == "feature");
    CHECK(git_text(repo, { "status", "--porcelain" }).empty());
}

TEST_CASE("integration: skipping the conflicting commit keeps the remaining reword", "[rebase][integration][conflict]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path repo { make_conflicting_feature(directory.path()) };
    live_workspace live {};
    const std::string id { live.open(repo) };
    const rebase_plan plan { live.plan(id, "main", "feature") };

    const std::vector<rebase_todo_entry> todo_list {
        entry(rebase_action::pick, by_subject(plan, "feat one")),
        entry(rebase_action::pick, by_subject(plan, "feat two")),
        entry(rebase_action::reword, by_subject(plan, "feat three"), std::string { "skip 뒤의 reword" }),
    };
    const operation_id run { live.run(id, request_of("main", "feature", todo_list)) };
    REQUIRE(live.status(run).state == operation_state::needs_input);
    live.require_succeeded(live.run(id, sequence_control_request { operation_kind::rebase_skip }));
    CHECK(subjects(repo, "main..feature") == text_list { "feat one", "skip 뒤의 reword" });
}

TEST_CASE("integration: a commit-msg hook still runs on reword", "[rebase][integration][hooks]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path repo { make_linear_feature(directory.path()) };
    // 메시지 끝에 꼬리말을 더하는 hook이다. reword가 hook을 건너뛰면 꼬리말이 없다.
    test::write_file(repo / ".git" / "hooks" / "commit-msg", "#!/bin/sh\nprintf '\\nHook-Ran: yes\\n' >> \"$1\"\n");
    live_workspace live {};
    const std::string id { live.open(repo) };
    const rebase_plan plan { live.plan(id, "main", "feature") };

    const std::vector<rebase_todo_entry> todo_list {
        entry(rebase_action::pick, by_subject(plan, "one")),
        entry(rebase_action::reword, by_subject(plan, "two"), std::string { "two, reworded" }),
        entry(rebase_action::pick, by_subject(plan, "three")),
        entry(rebase_action::pick, by_subject(plan, "four")),
    };
    live.require_succeeded(live.run(id, request_of("main", "feature", todo_list)));
    const std::string message { full_message(repo, "feature~2") };
    CHECK(message.starts_with("two, reworded\n"));
    CHECK(message.find("Hook-Ran: yes") != std::string::npos);
}

TEST_CASE("integration: interactive rebase refuses bad todos and merge ranges before touching the repository", "[rebase][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path repo { make_linear_feature(directory.path()) };
    const std::string before { git_text(repo, { "rev-parse", "feature" }) };
    live_workspace live {};
    const std::string id { live.open(repo) };
    const rebase_plan plan { live.plan(id, "main", "feature") };

    // 커밋 하나가 빠졌다.
    const std::vector<rebase_todo_entry> missing_list {
        entry(rebase_action::pick, by_subject(plan, "one")),
        entry(rebase_action::pick, by_subject(plan, "two")),
        entry(rebase_action::pick, by_subject(plan, "three")),
    };
    const operation_id missing { live.run(id, request_of("main", "feature", missing_list)) };
    REQUIRE(live.status(missing).state == operation_state::failed);
    CHECK(live.status(missing).failure->kind == failure_kind::invalid_request);
    // 첫 줄이 squash다.
    const std::vector<rebase_todo_entry> todo_list {
        entry(rebase_action::squash, by_subject(plan, "one")),
        entry(rebase_action::pick, by_subject(plan, "two")),
        entry(rebase_action::pick, by_subject(plan, "three")),
        entry(rebase_action::pick, by_subject(plan, "four")),
    };
    const operation_id leading { live.run(id, request_of("main", "feature", todo_list)) };
    CHECK(live.status(leading).failure->kind == failure_kind::invalid_request);
    CHECK(git_text(repo, { "rev-parse", "feature" }) == before);
    CHECK_FALSE(rebase_in_progress(repo));

    // 병합 커밋이 있는 범위는 확인 없이는 거절한다.
    git_ok(repo, { "switch", "-q", "-c", "side", "main" });
    commit_file(repo, "side.txt", "s\n", "side");
    git_ok(repo, { "switch", "-q", "feature" });
    git_ok(repo, { "merge", "-q", "--no-ff", "-m", "merge side", "side" });
    const rebase_plan merged { live.plan(id, "main", "feature") };
    CHECK(merged.merge_count == 1);
    interactive_rebase_request refused { request_of("main", "feature", default_rebase_todo(merged.commits)) };
    const operation_id flatten { live.run(id, refused) };
    REQUIRE(live.status(flatten).state == operation_state::failed);
    CHECK(live.status(flatten).failure->kind == failure_kind::refused);
    refused.allow_flatten_merges = true;
    live.require_succeeded(live.run(id, refused));
    CHECK(git_text(repo, { "rev-list", "--count", "--merges", "main..feature" }) == "0");
    CHECK(subjects(repo, "main..feature").size() == 5);
}

TEST_CASE("integration: commits already on a remote branch are marked as pushed", "[rebase][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path repo { make_linear_feature(directory.path()) };
    const std::filesystem::path bare { directory.path() / "origin.git" };
    git_ok(directory.path(), { "init", "-q", "--bare", "-b", "main", bare.string() });
    git_ok(repo, { "remote", "add", "origin", bare.string() });
    git_ok(repo, { "push", "-q", "origin", "feature~2:refs/heads/feature" });
    live_workspace live {};
    const std::string id { live.open(repo) };
    const rebase_plan plan { live.plan(id, "main", "feature") };
    CHECK(plan.has_remote_branches);
    CHECK(by_subject(plan, "one").pushed);
    CHECK(by_subject(plan, "two").pushed);
    CHECK_FALSE(by_subject(plan, "three").pushed);
    CHECK_FALSE(by_subject(plan, "four").pushed);
}

TEST_CASE("integration: the editor helper refuses a todo that does not match the shown range", "[rebase][integration][helper]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    const std::filesystem::path state { directory.path() / "state" };
    std::filesystem::create_directories(state / "messages");
    const std::filesystem::path rebase_merge { directory.path() / "git" / "rebase-merge" };
    std::filesystem::create_directories(rebase_merge);
    const std::string a { std::string(40, 'a') };
    const std::string b { std::string(40, 'b') };
    test::write_file(state / "todo", "reword " + a + "\npick " + b + "\n");
    test::write_file(state / "messages" / a, "new message\n");

    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    const auto run_helper = [&runner, &state](const std::filesystem::path& file) {
        process_request request {};
        request.executable = std::filesystem::path { CYELLA_REBASE_EDITOR_PATH }.u8string();
        request.arguments = { file.generic_u8string() };
        request.working_directory = state.u8string();
        request.environment_overrides = { process_environment_override { u8"CYELLA_REBASE_STATE", state.u8string() } };
        request.timeout = std::chrono::milliseconds { 30000 };
        request.maximum_captured_bytes_per_stream = 64u * 1024u;
        return runner->run(request, nullptr, process_cancellation_token {});
    };

    // 범위와 다른 목록(커밋 하나가 더 있다)이면 덮어쓰지 않고 실패한다.
    test::write_file(state / "expected", a + "\n" + b + "\n");
    test::write_file(rebase_merge / "git-rebase-todo", "pick aaaaaaa # one\npick bbbbbbb # two\npick ccccccc # three\n");
    const process_result refused { run_helper(rebase_merge / "git-rebase-todo") };
    CHECK(refused.exit_code == 1);
    CHECK(std::filesystem::exists(state / "mismatch"));
    CHECK(test::read_file(rebase_merge / "git-rebase-todo").find("ccccccc") != std::string::npos);

    // 맞으면 목록을 덮어쓰고 메시지를 rebase-merge/cyella로 옮긴다.
    test::write_file(rebase_merge / "git-rebase-todo", "pick aaaaaaa # one\npick bbbbbbb # two\n\n# Commands:\n");
    const process_result accepted { run_helper(rebase_merge / "git-rebase-todo") };
    CHECK(accepted.exit_code == 0);
    CHECK(test::read_file(rebase_merge / "git-rebase-todo") == "reword " + a + "\npick " + b + "\n");
    CHECK(test::read_file(rebase_merge / "cyella" / "messages" / a) == "new message\n");
    CHECK(std::filesystem::exists(rebase_merge / "cyella" / "active"));

    // 메시지 편집기: done의 마지막 줄이 reword면 그 메시지를, pick이면 손대지 않는다.
    const std::filesystem::path editmsg { directory.path() / "git" / "COMMIT_EDITMSG" };
    test::write_file(editmsg, "original\n");
    test::write_file(rebase_merge / "done", "pick " + b + "\n");
    CHECK(run_helper(editmsg).exit_code == 0);
    CHECK(test::read_file(editmsg) == "original\n");
    test::write_file(rebase_merge / "done", "pick " + b + "\nreword " + a + "\n");
    CHECK(run_helper(editmsg).exit_code == 0);
    CHECK(test::read_file(editmsg) == "new message\n");
}

TEST_CASE("integration: the editor helper works from a folder with spaces, quotes and Korean", "[rebase][integration][helper]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");
    test::temp_directory directory {};
    // git은 이 경로를 `sh -c`로 실행해야 한다 (공백·작은따옴표). 따옴표 처리가 틀리면 편집기를 찾지 못해 시작하지 않는다.
    const std::filesystem::path folder { directory.path() / std::filesystem::path { u8"설치 폴더's copy" } };
    std::filesystem::create_directories(folder);
    const std::filesystem::path helper { folder / "cyella-rebase-editor.exe" };
    std::filesystem::copy_file(std::filesystem::path { CYELLA_REBASE_EDITOR_PATH }, helper);
    const std::filesystem::path repo { make_linear_feature(directory.path()) };
    live_workspace live { helper };
    const std::string id { live.open(repo) };
    const rebase_plan plan { live.plan(id, "main", "feature") };

    const std::vector<rebase_todo_entry> todo_list {
        entry(rebase_action::pick, by_subject(plan, "one")),
        entry(rebase_action::reword, by_subject(plan, "two"), std::string { "via quoted helper" }),
        entry(rebase_action::pick, by_subject(plan, "four")),
        entry(rebase_action::pick, by_subject(plan, "three")),
    };
    live.require_succeeded(live.run(id, request_of("main", "feature", todo_list)));
    CHECK(subjects(repo, "main..feature") == text_list { "one", "via quoted helper", "four", "three" });
}
