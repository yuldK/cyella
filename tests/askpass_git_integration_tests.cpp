#include "askpass_test_support.h"
#include "git_test_support.h"
#include "infrastructure/git_execution_policy.h"
#include "infrastructure/git_tool_discovery.h"
#include "platform/win32/win32_askpass_server.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_process_runner.h"

#include <catch2/catch_test_macros.hpp>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {
    using namespace cyella;
    using namespace cyella::test;

    std::u8string find_git(process_runner& runner)
    {
        // 이 파일의 시험은 앱의 process runner로 git을 띄워 `run_git`의 환경 격리를 거치지 않는다. 사용자 전역 설정(credential.username 등)이
        // 질문의 수를 바꾸지 않도록 먼저 격리한다. 시험을 하나만 따로 돌려도 격리가 빠지지 않게 시험마다 이 함수를 거친다.
        isolate_git_environment();
        const std::unique_ptr<file_probe> probe { win32::make_file_probe() };
        const git_tool_info info { resolve_git_tool(u8"", win32::current_git_tool_environment(), runner, *probe, {}) };
        return info.usable() ? info.executable : std::u8string {};
    }

#if defined(_WIN32)
    // 실행 파일에 실린 manifest 글이다. 없거나 읽지 못하면 빈 문자열이다.
    std::string embedded_manifest(const std::filesystem::path& executable)
    {
        const HMODULE library { ::LoadLibraryExW(executable.c_str(), nullptr, LOAD_LIBRARY_AS_DATAFILE) };
        if (library == nullptr)
            return {};

        std::string text {};
        const HRSRC resource { ::FindResourceW(library, MAKEINTRESOURCEW(1), RT_MANIFEST) };
        if (resource != nullptr)
        {
            const HGLOBAL loaded { ::LoadResource(library, resource) };
            const void* const bytes { loaded != nullptr ? ::LockResource(loaded) : nullptr };
            if (bytes != nullptr)
                text.assign(static_cast<const char*>(bytes), ::SizeofResource(library, resource));
        }
        ::FreeLibrary(library);
        return text;
    }
#endif

    // 질문을 모두 기록하고 종류별로 정해진 답을 준다.
    class scripted_answers
    {
    public:
        [[nodiscard]] askpass_prompt_handler handler()
        {
            return [this](askpass_prompt_request request, askpass_reply_callback reply) {
                {
                    const std::lock_guard<std::mutex> lock { mutex_ };
                    prompts_.push_back(request.prompt);
                    operations_.push_back(request.operation_id);
                }
                switch (request.prompt.kind)
                {
                case credential_prompt_kind::https_username:
                    reply(askpass_reply::with_answer(u8"alice"));
                    break;
                case credential_prompt_kind::https_password:
                    reply(askpass_reply::with_answer(u8"pw"));
                    break;
                default:
                    reply(askpass_reply::cancelled());
                    break;
                }
            };
        }

        [[nodiscard]] std::vector<credential_prompt> prompts()
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            return prompts_;
        }

        [[nodiscard]] std::vector<std::u8string> operations()
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            return operations_;
        }

    private:
        std::mutex mutex_ {};
        std::vector<credential_prompt> prompts_ {};
        std::vector<std::u8string> operations_ {};
    };

    process_request make_credential_fill_request(const std::u8string& git, const askpass_endpoint& endpoint, const std::u8string_view operation)
    {
        git_request_options options {};
        options.askpass_helper = to_u8string(askpass_helper_path().generic_string());
        options.askpass = session_for(endpoint, operation);
        // 출력을 가공 없이 받아 `username=`·`password=` 줄을 그대로 확인한다.
        options.raw_standard_output = true;
        options.standard_input = "protocol=https\nhost=example.invalid\n\n";
        const std::filesystem::path directory { std::filesystem::temp_directory_path() };
        // `credential.helper=`로 사용자의 helper(Git Credential Manager 등)를 끈다. 저장된 자격 증명이 있어도 질문이 나오게 한다.
        return make_git_process_request(git, to_u8string(directory.generic_string()), { u8"-c", u8"credential.helper=", u8"credential", u8"fill" }, git_command_class::network, options);
    }
} // namespace

TEST_CASE("git credential fill asks the app for the username and password", "[askpass][integration]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    const std::u8string git { find_git(*runner) };
    if (git.empty())
        SKIP("git is not installed");

    scripted_answers answers {};
    win32::askpass_server_start_result started { win32::start_askpass_server(answers.handler()) };
    REQUIRE(started.server != nullptr);

    collecting_sink sink {};
    const process_result result { runner->run(make_credential_fill_request(git, started.server->endpoint(), u8"op-fill"), &sink, {}) };

    REQUIRE(result.completion == process_completion::exited);
    REQUIRE(result.exit_code.has_value());
    CHECK(*result.exit_code == 0);
    const std::string output { sink.standard_output_bytes };
    CHECK(output.find("username=alice") != std::string::npos);
    CHECK(output.find("password=pw") != std::string::npos);

    // 사용자 이름과 비밀번호가 같은 operation에서 차례로 온다.
    const std::vector<credential_prompt> prompts { answers.prompts() };
    REQUIRE(prompts.size() == 2);
    CHECK(prompts[0].kind == credential_prompt_kind::https_username);
    CHECK(prompts[0].url == u8"https://example.invalid");
    CHECK(prompts[1].kind == credential_prompt_kind::https_password);
    CHECK(prompts[1].user_name == u8"alice");
    CHECK(prompts[1].host == u8"example.invalid");
    for (const std::u8string& operation : answers.operations())
        CHECK(operation == u8"op-fill");
}

TEST_CASE("git credential fill fails when the user cancels the dialog", "[askpass][integration]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    const std::u8string git { find_git(*runner) };
    if (git.empty())
        SKIP("git is not installed");

    win32::askpass_server_start_result started { win32::start_askpass_server([](askpass_prompt_request, askpass_reply_callback reply) { reply(askpass_reply::cancelled()); }) };
    REQUIRE(started.server != nullptr);

    collecting_sink sink {};
    const process_result result { runner->run(make_credential_fill_request(git, started.server->endpoint(), u8"op-cancel"), &sink, {}) };

    REQUIRE(result.completion == process_completion::exited);
    REQUIRE(result.exit_code.has_value());
    CHECK(*result.exit_code != 0);
    CHECK(sink.standard_output_bytes.find("password=") == std::string::npos);
}

TEST_CASE("git without the helper cannot prompt and fails at once", "[askpass][integration]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    const std::u8string git { find_git(*runner) };
    if (git.empty())
        SKIP("git is not installed");

    git_request_options options {};
    options.standard_input = "protocol=https\nhost=example.invalid\n\n";
    const std::u8string directory { to_u8string(std::filesystem::temp_directory_path().generic_string()) };
    const std::vector<std::u8string> arguments { u8"-c", u8"credential.helper=", u8"credential", u8"fill" };
    const process_request request { make_git_process_request(git, directory, arguments, git_command_class::local_query, options) };

    const process_result result { runner->run(request, nullptr, {}) };

    REQUIRE(result.completion == process_completion::exited);
    REQUIRE(result.exit_code.has_value());
    CHECK(*result.exit_code != 0);
}

// Windows의 manifest(longPathAware) 검사다. macOS 실행 파일에는 manifest가 없고 경로 길이 제한도 다르다.
#if defined(_WIN32)
TEST_CASE("the helper executables declare long path support in their manifest", "[askpass]")
{
    // 도우미는 git이 넘긴 경로(깊은 저장소의 `.git/rebase-merge/git-rebase-todo` 등)를 연다. 앱처럼 longPathAware를 선언하지 않으면
    // 시스템의 긴 경로 설정이 켜져 있어도 260자를 넘는 경로를 열지 못한다.
    const std::vector<std::filesystem::path> helpers { askpass_helper_path(), std::filesystem::absolute(std::filesystem::path { CYELLA_REBASE_EDITOR_PATH }) };

    for (const std::filesystem::path& helper : helpers)
    {
        INFO(helper.string());
        CHECK(embedded_manifest(helper).find("longPathAware") != std::string::npos);
    }
}
#endif
