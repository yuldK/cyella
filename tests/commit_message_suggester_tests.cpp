// 실제 git과 임시 저장소, 그리고 test 도우미 실행 파일을 "AI 명령" 자리에 놓고 커밋 메시지 제안의 worker 쪽을 끝까지 돌린다.
// 어떤 test도 실제 AI 명령을 띄우지 않는다.
#include "git_test_support.h"
#include "infrastructure/commit_message_suggester.h"
#include "infrastructure/git_tool_discovery.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_process_runner.h"
#include "process_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
    using namespace cyella;

    bool contains(const std::u8string& text, const std::u8string_view part)
    {
        return text.find(part) != std::u8string::npos;
    }

    std::u8string find_git(process_runner& runner)
    {
        const std::unique_ptr<file_probe> probe { win32::make_file_probe() };
        const git_tool_info info { resolve_git_tool(u8"", win32::current_git_tool_environment(), runner, *probe, {}) };
        return info.usable() ? info.executable : std::u8string {};
    }

    // 커밋 하나가 있고 그 위에 변경을 staged해 둔 저장소다. lock 파일과 `.env`도 함께 staged한다.
    struct suggestion_fixture
    {
        suggestion_fixture()
        {
            test::isolate_git_environment();
            runner = win32::make_process_runner();
            git = find_git(*runner);
            REQUIRE(test::run_git(root.path(), std::vector<std::string> { "init", "-q", "-b", "main" }).exit_code == 0);
        }

        void commit_first()
        {
            test::write_file(root.path() / "src" / "timer.cpp", "void start()\n{\n}\n");
            REQUIRE(test::run_git(root.path(), std::vector<std::string> { "add", "-A" }).exit_code == 0);
            REQUIRE(test::run_git(root.path(), std::vector<std::string> { "commit", "-q", "-m", "feat: first timer" }).exit_code == 0);
        }

        void stage_changes()
        {
            test::write_file(root.path() / "src" / "timer.cpp", "void start()\n{\n    schedule_next();\n}\n");
            test::write_file(root.path() / ".env", "TOKEN=hunter2-do-not-send\n");
            test::write_file(root.path() / "package-lock.json", "{ \"lockfileVersion\": 3, \"marker\": \"lock-body-do-not-send\" }\n");
            REQUIRE(test::run_git(root.path(), std::vector<std::string> { "add", "-A" }).exit_code == 0);
        }

        [[nodiscard]] commit_message_suggester suggester(const std::chrono::milliseconds timeout = std::chrono::seconds { 30 }) const
        {
            commit_message_suggester_options options {};
            // `child`만 test 도우미로 풀린다. 나머지 이름은 "못 찾음"이다.
            options.resolve_executable = [](const std::u8string_view name) -> std::optional<std::u8string> {
                if (name == u8"child")
                    return test::to_u8string(std::filesystem::absolute(test::test_child_path()).string());
                if (name == u8"script")
                    return std::u8string { u8"C:\\Tools\\shim.CMD" };
                return std::nullopt;
            };
            options.working_directory = test::to_u8string(std::filesystem::absolute(test::test_child_path()).parent_path().string());
            options.timeout = timeout;
            return commit_message_suggester { *runner, git, options };
        }

        [[nodiscard]] commit_suggestion_job job(std::u8string executable, std::u8string arguments) const
        {
            commit_suggestion_job value {};
            value.serial = 7;
            value.repository_id = "repo";
            value.repository_path = test::to_u8string(root.path().string());
            value.executable = std::move(executable);
            value.arguments = std::move(arguments);
            return value;
        }

        test::temp_directory root {};
        std::unique_ptr<process_runner> runner {};
        std::u8string git {};
    };
} // namespace

TEST_CASE("the suggester sends the staged change to the command on stdin and returns its answer", "[commit_suggestion][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not available");
    suggestion_fixture fixture {};
    REQUIRE(fixture.git.empty() == false);
    fixture.commit_first();
    fixture.stage_changes();

    // 도우미가 표준 입력을 그대로 되돌려 주므로 "답"이 곧 보낸 프롬프트다.
    const commit_suggestion_result_intent result { fixture.suggester().suggest(fixture.job(u8"child", u8"echo_stdin"), process_cancellation_token {}) };

    INFO(test::to_string(result.error));
    REQUIRE(result.state == commit_suggestion_state::succeeded);
    CHECK(result.serial == 7);
    CHECK(result.repository_id == "repo");
    CHECK(result.message.subject == u8"Write a git commit message for the staged changes described below.");
    const std::u8string& prompt { result.message.body };
    CHECK(contains(prompt, u8"- feat: first timer"));
    CHECK(contains(prompt, u8"- src/timer.cpp (+1 -0)"));
    CHECK(contains(prompt, u8"diff --git a/src/timer.cpp b/src/timer.cpp"));
    CHECK(contains(prompt, u8"+    schedule_next();"));
    // 비밀로 보이는 파일과 lock 파일은 이름과 줄 수만 간다.
    CHECK(contains(prompt, u8"- .env (+1 -0) [content not included]"));
    CHECK(contains(prompt, u8"- package-lock.json (+1 -0) [content not included]"));
    CHECK_FALSE(contains(prompt, u8"hunter2-do-not-send"));
    CHECK_FALSE(contains(prompt, u8"lock-body-do-not-send"));
    CHECK_FALSE(contains(prompt, u8"diff --git a/.env"));
}

TEST_CASE("too many files to exclude means no diff text is sent at all", "[commit_suggestion][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not available");
    suggestion_fixture fixture {};
    REQUIRE(fixture.git.empty() == false);
    fixture.commit_first();
    fixture.stage_changes();
    // 가릴 파일이 한 번에 뺄 수 있는 수를 넘는다. 다 빼지 못한 패치를 보내지 않고 목록만 보낸다.
    for (std::size_t index { 0 }; index <= commit_suggestion_excluded_path_limit; ++index)
        test::write_file(fixture.root.path() / "env" / (".env." + std::to_string(index)), "KEY=bulk-secret-do-not-send\n");
    REQUIRE(test::run_git(fixture.root.path(), std::vector<std::string> { "add", "-A" }).exit_code == 0);

    const commit_suggestion_result_intent result { fixture.suggester().suggest(fixture.job(u8"child", u8"echo_stdin"), process_cancellation_token {}) };

    INFO(test::to_string(result.error));
    REQUIRE(result.state == commit_suggestion_state::succeeded);
    CHECK(contains(result.message.body, u8"No diff text is included."));
    // 목록은 경로 순서로 앞의 200개만 적히고 나머지는 수로 준다.
    CHECK(contains(result.message.body, u8"- .env (+1 -0) [content not included]"));
    CHECK(contains(result.message.body, u8"more files"));
    CHECK_FALSE(contains(result.message.body, u8"diff --git"));
    CHECK_FALSE(contains(result.message.body, u8"bulk-secret-do-not-send"));
    CHECK_FALSE(contains(result.message.body, u8"schedule_next"));
}

TEST_CASE("a repository without commits asks for the screen language", "[commit_suggestion][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not available");
    suggestion_fixture fixture {};
    REQUIRE(fixture.git.empty() == false);
    fixture.stage_changes();

    commit_suggestion_job job { fixture.job(u8"child", u8"echo_stdin") };
    job.language = ui_language::english;
    job.instruction = u8"Use the imperative mood";
    const commit_suggestion_result_intent result { fixture.suggester().suggest(job, process_cancellation_token {}) };

    INFO(test::to_string(result.error));
    REQUIRE(result.state == commit_suggestion_state::succeeded);
    CHECK(contains(result.message.body, u8"Write the message in English."));
    CHECK(contains(result.message.body, u8"Use the imperative mood"));
    CHECK(contains(result.message.body, u8"diff --git a/src/timer.cpp b/src/timer.cpp"));
}

TEST_CASE("nothing staged fails before any command starts", "[commit_suggestion][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not available");
    suggestion_fixture fixture {};
    REQUIRE(fixture.git.empty() == false);
    fixture.commit_first();

    // 명령 이름이 틀렸어도 그 오류가 아니라 "staged 없음"이 먼저다.
    const commit_suggestion_result_intent result { fixture.suggester().suggest(fixture.job(u8"missing", u8""), process_cancellation_token {}) };
    CHECK(result.state == commit_suggestion_state::failed);
    CHECK(result.error == tr(u8"commit_suggestion.nothing_staged"));
}

TEST_CASE("a command that cannot be started is reported by name", "[commit_suggestion][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not available");
    suggestion_fixture fixture {};
    REQUIRE(fixture.git.empty() == false);
    fixture.commit_first();
    fixture.stage_changes();
    const commit_message_suggester suggester { fixture.suggester() };

    const commit_suggestion_result_intent missing { suggester.suggest(fixture.job(u8"missing", u8"-p"), process_cancellation_token {}) };
    CHECK(missing.state == commit_suggestion_state::failed);
    CHECK(missing.error == tr(u8"commit_suggestion.command_not_found", { { u8"command", u8"missing" } }));

    // cmd.exe를 거쳐야 하는 shim은 띄우지 않는다. 적은 이름이든 풀린 경로든 같다.
    const commit_suggestion_result_intent resolved_script { suggester.suggest(fixture.job(u8"script", u8"-p"), process_cancellation_token {}) };
    CHECK(resolved_script.error == tr(u8"commit_suggestion.script_not_supported", { { u8"command", u8"script" } }));
    const commit_suggestion_result_intent written_script { suggester.suggest(fixture.job(u8"C:\\npm\\claude.cmd", u8"-p"), process_cancellation_token {}) };
    CHECK(written_script.error == tr(u8"commit_suggestion.script_not_supported", { { u8"command", u8"C:\\npm\\claude.cmd" } }));

    const commit_suggestion_result_intent unclosed { suggester.suggest(fixture.job(u8"child", u8"echo_stdin \"open"), process_cancellation_token {}) };
    CHECK(unclosed.error == tr(u8"commit_suggestion.arguments_invalid"));
}

TEST_CASE("a failing command reports its exit code and its first error line", "[commit_suggestion][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not available");
    suggestion_fixture fixture {};
    REQUIRE(fixture.git.empty() == false);
    fixture.commit_first();
    fixture.stage_changes();
    const commit_message_suggester suggester { fixture.suggester() };

    const commit_suggestion_result_intent failed { suggester.suggest(fixture.job(u8"child", u8"write_error_text \"not logged in\" exit 3"), process_cancellation_token {}) };
    CHECK(failed.state == commit_suggestion_state::failed);
    CHECK(contains(failed.error, u8"not logged in"));
    CHECK(contains(failed.error, u8"3"));

    // 머리말과 프롬프트를 표준 오류로 되울린 뒤 맨 끝에 오류를 적는 명령이다 (codex). 이유는 마지막 줄이다.
    const commit_suggestion_result_intent echoed {
        suggester.suggest(fixture.job(u8"child", u8"error_lines 20000 write_error_text \"banner v1\\nERROR: model is not supported\\n\\n\" exit 1"), process_cancellation_token {}),
    };
    CHECK(echoed.state == commit_suggestion_state::failed);
    CHECK(contains(echoed.error, u8"ERROR: model is not supported"));
    CHECK_FALSE(contains(echoed.error, u8"banner v1"));

    // 실패를 표준 출력으로 내는 명령도 이유가 보인다.
    const commit_suggestion_result_intent on_output { suggester.suggest(fixture.job(u8"child", u8"write_text \"quota exceeded\" exit 1"), process_cancellation_token {}) };
    CHECK(on_output.state == commit_suggestion_state::failed);
    CHECK(contains(on_output.error, u8"quota exceeded"));

    // 답이 상한을 넘으면 잘린 글을 쓰지 않는다.
    const commit_suggestion_result_intent long_answer { suggester.suggest(fixture.job(u8"child", u8"write_bytes 70000"), process_cancellation_token {}) };
    CHECK(long_answer.state == commit_suggestion_state::failed);
    CHECK(long_answer.error == tr(u8"commit_suggestion.answer_too_long"));

    // 성공으로 끝났지만 쓸 글이 없다.
    const commit_suggestion_result_intent empty { suggester.suggest(fixture.job(u8"child", u8"exit 0"), process_cancellation_token {}) };
    CHECK(empty.state == commit_suggestion_state::failed);
    CHECK(empty.error == tr(u8"commit_suggestion.empty_answer"));
}

TEST_CASE("a slow command stops at the time limit and a cancelled one says nothing", "[commit_suggestion][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not available");
    suggestion_fixture fixture {};
    REQUIRE(fixture.git.empty() == false);
    fixture.commit_first();
    fixture.stage_changes();

    const commit_suggestion_result_intent slow { fixture.suggester(std::chrono::milliseconds { 300 }).suggest(fixture.job(u8"child", u8"sleep 20000"), process_cancellation_token {}) };
    CHECK(slow.state == commit_suggestion_state::failed);
    CHECK(slow.error == tr(u8"commit_suggestion.timed_out", { { u8"command", u8"child" } }));

    // 시작 전에 취소된 일감은 git도 명령도 띄우지 않고 취소로 끝난다.
    process_cancellation_source source {};
    source.request_cancellation();
    const commit_suggestion_result_intent cancelled { fixture.suggester().suggest(fixture.job(u8"child", u8"echo_stdin"), source.token()) };
    CHECK(cancelled.state == commit_suggestion_state::cancelled);
    CHECK(cancelled.error.empty());
}
