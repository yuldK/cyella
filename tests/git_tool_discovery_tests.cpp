#include "infrastructure/git_tool_discovery.h"

#include "infrastructure/git_version.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_process_runner.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace cyella;

namespace {
    // 존재하는 파일 목록을 값으로 받는 probe다.
    class fake_probe final : public file_probe
    {
    public:
        explicit fake_probe(std::set<std::u8string> files)
            : files_ { std::move(files) }
        {}

        [[nodiscard]] path_kind probe(const std::u8string_view absolute_path) const noexcept override
        {
            return files_.contains(std::u8string { absolute_path }) ? path_kind::file : path_kind::missing;
        }

    private:
        std::set<std::u8string> files_ {};
    };

    // `--version`에 정해 둔 출력과 종료 코드로 답하는 runner다.
    class fake_runner final : public process_runner
    {
    public:
        fake_runner(std::u8string output, const std::int32_t exit_code)
            : output_ { std::move(output) }
            , exit_code_ { exit_code }
        {}

        [[nodiscard]] process_result run(const process_request& request, process_output_sink* const sink, const process_cancellation_token&) noexcept override
        {
            last_request = request;
            ++run_count;
            if (sink != nullptr && output_.empty() == false)
            {
                process_output_record record {};
                record.text = output_;
                sink->on_record(record);
            }
            process_result result {};
            result.completion = process_completion::exited;
            result.exit_code = exit_code_;
            return result;
        }

        process_request last_request {};
        int run_count { 0 };

    private:
        std::u8string output_ {};
        std::int32_t exit_code_ { 0 };
    };

    const git_tool_environment environment {
        u8"C:\\Windows\\System32;\"C:\\Tools\\Git\\cmd\";relative\\bin;;c:\\windows\\system32\\;C:\\Tools\\Git\\cmd",
        { u8"C:\\Program Files", u8"C:\\Program Files (x86)" },
    };
} // namespace

TEST_CASE("git version parser reads the first version token", "[git_tool]")
{
    const auto windows_build { parse_git_tool_version(u8"git version 2.52.0.windows.1") };
    REQUIRE(windows_build.has_value());
    CHECK(*windows_build == git_tool_version { 2, 52, 0 });

    const auto short_form { parse_git_tool_version(u8"git version 2.43") };
    REQUIRE(short_form.has_value());
    CHECK(*short_form == git_tool_version { 2, 43, 0 });

    CHECK_FALSE(parse_git_tool_version(u8"not a version").has_value());
    CHECK_FALSE(parse_git_tool_version(u8"").has_value());
}

TEST_CASE("git minimum version is 2.43", "[git_tool]")
{
    CHECK(meets_minimum_git_version({ 2, 43, 0 }));
    CHECK(meets_minimum_git_version({ 2, 52, 1 }));
    CHECK(meets_minimum_git_version({ 3, 0, 0 }));
    CHECK_FALSE(meets_minimum_git_version({ 2, 42, 9 }));
    CHECK_FALSE(meets_minimum_git_version({ 1, 99, 0 }));
}

TEST_CASE("search path splitting drops relative duplicates and quotes", "[git_tool]")
{
    const std::vector<std::u8string> directories { split_search_path(environment.path_environment) };

    REQUIRE(directories.size() == 2);
    CHECK(directories[0] == u8"C:\\Windows\\System32");
    CHECK(directories[1] == u8"C:\\Tools\\Git\\cmd");
}

TEST_CASE("candidates follow PATH first and the default install locations last", "[git_tool]")
{
    const std::vector<git_tool_candidate> candidates { git_tool_candidates(u8"", environment) };

    REQUIRE(candidates.size() == 6);
    CHECK(candidates[0].executable == u8"C:\\Windows\\System32\\git.exe");
    CHECK(candidates[1].executable == u8"C:\\Tools\\Git\\cmd\\git.exe");
    CHECK(candidates[2].executable == u8"C:\\Program Files\\Git\\cmd\\git.exe");
    CHECK(candidates[5].executable == u8"C:\\Program Files (x86)\\Git\\bin\\git.exe");
    for (const git_tool_candidate& candidate : candidates)
        CHECK_FALSE(candidate.manually_configured);
}

TEST_CASE("a configured path is the only candidate", "[git_tool]")
{
    const std::vector<git_tool_candidate> candidates { git_tool_candidates(u8"D:\\git\\git.exe", environment) };

    REQUIRE(candidates.size() == 1);
    CHECK(candidates[0].executable == u8"D:\\git\\git.exe");
    CHECK(candidates[0].manually_configured);
}

TEST_CASE("resolving finds the first existing candidate and reads its version", "[git_tool]")
{
    const fake_probe probe { { u8"C:\\Tools\\Git\\cmd\\git.exe", u8"C:\\Program Files\\Git\\cmd\\git.exe" } };
    fake_runner runner { u8"git version 2.52.0.windows.1", 0 };

    const git_tool_info info { resolve_git_tool(u8"", environment, runner, probe, {}) };

    CHECK(info.availability == git_tool_availability::available);
    CHECK(info.usable());
    CHECK(info.executable == u8"C:\\Tools\\Git\\cmd\\git.exe");
    CHECK(info.version == git_tool_version { 2, 52, 0 });
    CHECK_FALSE(info.manually_configured);
    // 버전 확인은 실행 파일이 있는 디렉터리에서 짧은 제한 시간으로 한다.
    CHECK(runner.last_request.working_directory == u8"C:\\Tools\\Git\\cmd");
    CHECK(runner.last_request.timeout.has_value());
}

TEST_CASE("resolving reports a missing tool without running anything", "[git_tool]")
{
    const fake_probe probe { {} };
    fake_runner runner { u8"", 0 };

    const git_tool_info info { resolve_git_tool(u8"", environment, runner, probe, {}) };

    CHECK(info.availability == git_tool_availability::not_found);
    CHECK_FALSE(info.usable());
    CHECK(runner.run_count == 0);
    REQUIRE(info.diagnostics.size() == 1);
    CHECK(info.diagnostics[0].code == diagnostic_code::git_tool_not_found);
}

TEST_CASE("resolving reports a git older than 2.43", "[git_tool]")
{
    const fake_probe probe { { u8"C:\\Tools\\Git\\cmd\\git.exe" } };
    fake_runner runner { u8"git version 2.42.0.windows.2", 0 };

    const git_tool_info info { resolve_git_tool(u8"", environment, runner, probe, {}) };

    CHECK(info.availability == git_tool_availability::too_old);
    CHECK_FALSE(info.usable());
    CHECK(info.version == git_tool_version { 2, 42, 0 });
}

TEST_CASE("resolving does not fall back from a broken configured path", "[git_tool]")
{
    const fake_probe probe { { u8"C:\\Tools\\Git\\cmd\\git.exe" } };
    fake_runner runner { u8"git version 2.52.0", 0 };

    const git_tool_info relative { resolve_git_tool(u8"git.exe", environment, runner, probe, {}) };
    CHECK(relative.availability == git_tool_availability::path_invalid);

    const git_tool_info missing { resolve_git_tool(u8"D:\\nowhere\\git.exe", environment, runner, probe, {}) };
    CHECK(missing.availability == git_tool_availability::path_invalid);
    CHECK(missing.manually_configured);
    CHECK(runner.run_count == 0);
}

TEST_CASE("resolving reports unreadable output and failing commands", "[git_tool]")
{
    const fake_probe probe { { u8"C:\\Tools\\Git\\cmd\\git.exe" } };

    fake_runner garbled { u8"hello", 0 };
    CHECK(resolve_git_tool(u8"", environment, garbled, probe, {}).availability == git_tool_availability::version_unreadable);

    fake_runner failing { u8"", 1 };
    CHECK(resolve_git_tool(u8"", environment, failing, probe, {}).availability == git_tool_availability::version_unreadable);
}

TEST_CASE("the installed git is found with the real runner", "[git_tool][integration]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    const std::unique_ptr<file_probe> probe { win32::make_file_probe() };

    const git_tool_info info { resolve_git_tool(u8"", win32::current_git_tool_environment(), *runner, *probe, {}) };

    if (info.availability == git_tool_availability::not_found)
        SKIP("git is not installed on this machine");
    REQUIRE(info.availability == git_tool_availability::available);
    CHECK(info.version >= minimum_supported_git_version());
    CHECK(info.reported_version.starts_with(u8"git version"));
}
