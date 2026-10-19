#include "infrastructure/git_execution_policy.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

using namespace cyella;

namespace {
    const process_environment_override* find_override(const std::vector<process_environment_override>& overrides, const std::u8string_view name)
    {
        for (const process_environment_override& entry : overrides)
            if (entry.name == name)
                return &entry;
        return nullptr;
    }

    process_request make_request(const git_command_class command_class, const git_request_options& options = {})
    {
        return make_git_process_request(u8"C:/Git/cmd/git.exe", u8"C:/work/repo", { u8"status", u8"--porcelain=v2" }, command_class, options);
    }
} // namespace

TEST_CASE("git requests start with the common arguments", "[policy]")
{
    const process_request request { make_request(git_command_class::local_write) };

    const std::vector<std::u8string> expected { u8"-c", u8"core.quotepath=false", u8"-c", u8"gc.auto=0", u8"-c", u8"color.ui=false", u8"--no-pager", u8"status", u8"--porcelain=v2" };
    CHECK(request.arguments == expected);
    CHECK(request.executable == u8"C:/Git/cmd/git.exe");
    CHECK(request.working_directory == u8"C:/work/repo");
}

TEST_CASE("git read queries skip optional locks so they can run beside writes", "[policy][lanes]")
{
    // 읽기는 쓰기와 다른 lane에서 병렬로 돈다. status가 index.lock을 잡지 않아야 한다.
    const process_request query { make_request(git_command_class::local_query) };
    std::vector<std::u8string> expected { git_common_arguments() };
    expected.insert(expected.end(), { u8"--no-optional-locks", u8"status", u8"--porcelain=v2" });
    CHECK(query.arguments == expected);

    for (const git_command_class command_class : { git_command_class::local_write, git_command_class::network, git_command_class::tool_probe })
    {
        const process_request request { make_request(command_class) };
        CHECK(std::find(request.arguments.begin(), request.arguments.end(), std::u8string { u8"--no-optional-locks" }) == request.arguments.end());
    }
}

TEST_CASE("git requests run in the C locale without an editor", "[policy]")
{
    for (const std::u8string_view helper : { std::u8string_view {}, std::u8string_view { u8"C:/cyella/askpass.exe" } })
    {
        git_request_options options {};
        options.askpass_helper = helper;
        const process_request request { make_request(git_command_class::network, options) };

        const process_environment_override* const locale { find_override(request.environment_overrides, u8"LC_ALL") };
        REQUIRE(locale != nullptr);
        CHECK(locale->value == std::optional<std::u8string> { u8"C" });
        const process_environment_override* const editor { find_override(request.environment_overrides, u8"GIT_EDITOR") };
        REQUIRE(editor != nullptr);
        CHECK(editor->value == std::optional<std::u8string> { u8"true" });
        CHECK(validate_process_request(request).empty());
    }
}

TEST_CASE("git requests never prompt on the terminal", "[policy]")
{
    const process_request request { make_request(git_command_class::local_query) };

    const process_environment_override* const prompt { find_override(request.environment_overrides, u8"GIT_TERMINAL_PROMPT") };
    REQUIRE(prompt != nullptr);
    REQUIRE(prompt->value.has_value());
    CHECK(*prompt->value == u8"0");
}

TEST_CASE("git requests leave the ssh command and credential manager alone", "[policy]")
{
    const process_request request { make_request(git_command_class::network) };

    CHECK(find_override(request.environment_overrides, u8"GIT_SSH_COMMAND") == nullptr);
    CHECK(find_override(request.environment_overrides, u8"GCM_INTERACTIVE") == nullptr);
    CHECK(find_override(request.environment_overrides, u8"GIT_OPTIONAL_LOCKS") == nullptr);
}

TEST_CASE("git requests without a helper remove inherited askpass variables", "[policy][askpass]")
{
    const process_request request { make_request(git_command_class::network) };

    for (const std::u8string_view name : { u8"GIT_ASKPASS", u8"SSH_ASKPASS", u8"SSH_ASKPASS_REQUIRE" })
    {
        const process_environment_override* const entry { find_override(request.environment_overrides, name) };
        REQUIRE(entry != nullptr);
        CHECK_FALSE(entry->value.has_value());
    }
}

TEST_CASE("git requests with a helper route askpass to it", "[policy][askpass]")
{
    git_request_options options {};
    options.askpass_helper = u8"C:/cyella/cyella_askpass.exe";

    const process_request request { make_request(git_command_class::network, options) };

    for (const std::u8string_view name : { u8"GIT_ASKPASS", u8"SSH_ASKPASS" })
    {
        const process_environment_override* const entry { find_override(request.environment_overrides, name) };
        REQUIRE(entry != nullptr);
        REQUIRE(entry->value.has_value());
        CHECK(*entry->value == u8"C:/cyella/cyella_askpass.exe");
    }
    const process_environment_override* const require { find_override(request.environment_overrides, u8"SSH_ASKPASS_REQUIRE") };
    REQUIRE(require != nullptr);
    REQUIRE(require->value.has_value());
    CHECK(*require->value == u8"force");
    CHECK(validate_process_request(request).empty());
}

TEST_CASE("git command classes carry their own limits", "[policy][timeout]")
{
    using namespace std::chrono_literals;

    CHECK(git_limits_for(git_command_class::tool_probe).timeout == 5s);
    CHECK(git_limits_for(git_command_class::local_query).timeout == 600s);
    CHECK(git_limits_for(git_command_class::local_write).timeout == 300s);
    // 네트워크 작업은 느린 회선과 인증 대화 상자를 기다리므로 자르지 않는다.
    CHECK_FALSE(git_limits_for(git_command_class::network).timeout.has_value());

    const process_request network { make_request(git_command_class::network) };
    CHECK_FALSE(network.timeout.has_value());
    const process_request query { make_request(git_command_class::local_query) };
    CHECK(query.timeout == 600s);
}

TEST_CASE("git request options select raw output and input", "[policy][raw]")
{
    git_request_options options {};
    options.raw_standard_output = true;
    options.standard_input = "message";
    options.maximum_captured_bytes_per_stream = unlimited_process_capture_bytes;

    const process_request request { make_request(git_command_class::local_query, options) };

    CHECK(request.standard_output_mode == process_output_mode::raw);
    CHECK(request.standard_error_mode == process_output_mode::lines);
    CHECK(request.standard_input == "message");
    CHECK(request.maximum_captured_bytes_per_stream == unlimited_process_capture_bytes);
    CHECK(validate_process_request(request).empty());
}

TEST_CASE("git requests default to line output with the class capture limit", "[policy]")
{
    const process_request request { make_request(git_command_class::local_query) };

    CHECK(request.standard_output_mode == process_output_mode::lines);
    CHECK(request.standard_input.empty());
    CHECK(request.maximum_captured_bytes_per_stream == git_limits_for(git_command_class::local_query).maximum_captured_bytes_per_stream);
}

TEST_CASE("git requests carry the askpass session only together with the helper", "[policy][askpass]")
{
    git_request_options options {};
    options.askpass.pipe_name = u8"\\\\.\\pipe\\cyella-askpass-1-abc";
    options.askpass.token = u8"0123456789abcdef";
    options.askpass.operation_id = u8"op-9";

    // 도우미가 없으면 세션 값을 주지 않고 사용자 환경의 값도 지운다.
    const process_request without_helper { make_request(git_command_class::network, options) };
    for (const std::u8string_view name : { u8"CYELLA_ASKPASS_PIPE", u8"CYELLA_ASKPASS_TOKEN", u8"CYELLA_ASKPASS_OPERATION" })
    {
        const process_environment_override* const entry { find_override(without_helper.environment_overrides, name) };
        REQUIRE(entry != nullptr);
        CHECK_FALSE(entry->value.has_value());
    }

    options.askpass_helper = u8"C:/cyella/bin/cyella-askpass.exe";
    const process_request with_helper { make_request(git_command_class::network, options) };
    const process_environment_override* const pipe { find_override(with_helper.environment_overrides, u8"CYELLA_ASKPASS_PIPE") };
    const process_environment_override* const token { find_override(with_helper.environment_overrides, u8"CYELLA_ASKPASS_TOKEN") };
    const process_environment_override* const operation { find_override(with_helper.environment_overrides, u8"CYELLA_ASKPASS_OPERATION") };
    REQUIRE(pipe != nullptr);
    REQUIRE(token != nullptr);
    REQUIRE(operation != nullptr);
    CHECK(pipe->value == std::u8string { u8"\\\\.\\pipe\\cyella-askpass-1-abc" });
    CHECK(token->value == std::u8string { u8"0123456789abcdef" });
    CHECK(operation->value == std::u8string { u8"op-9" });
    CHECK(validate_process_request(with_helper).empty());
    // 토큰은 환경에만 있고 명령줄 기록에는 나오지 않는다.
    for (const std::u8string& argument : with_helper.arguments)
        CHECK(argument.find(u8"0123456789abcdef") == std::u8string::npos);
}
