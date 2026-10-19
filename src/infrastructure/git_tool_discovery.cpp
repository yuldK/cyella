#include "infrastructure/git_tool_discovery.h"

#include "application/process_request.h"
#include "domain/path_syntax.h"
#include "infrastructure/git_command_runner.h"
#include "infrastructure/git_execution_policy.h"
#include "infrastructure/git_version.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

namespace cyella {
    namespace {
        char8_t ascii_uppercase(const char8_t value) noexcept
        {
            if (value >= u8'a' && value <= u8'z')
                return static_cast<char8_t>(value - u8'a' + u8'A');
            return value;
        }

        bool ascii_equal_ignoring_case(const std::u8string_view left, const std::u8string_view right) noexcept
        {
            if (left.size() != right.size())
                return false;
            for (std::size_t index = 0; index < left.size(); ++index)
                if (ascii_uppercase(left[index]) != ascii_uppercase(right[index]))
                    return false;
            return true;
        }

        bool is_separator(const char8_t value) noexcept
        {
            return value == u8'\\' || value == u8'/';
        }

        std::u8string_view trim_ascii_spaces(std::u8string_view value) noexcept
        {
            while (value.empty() == false && (value.front() == u8' ' || value.front() == u8'\t'))
                value.remove_prefix(1);
            while (value.empty() == false && (value.back() == u8' ' || value.back() == u8'\t'))
                value.remove_suffix(1);
            return value;
        }

        std::u8string strip_quotes(const std::u8string_view value)
        {
            std::u8string result {};
            result.reserve(value.size());
            for (const char8_t character : value)
                if (character != u8'"')
                    result.push_back(character);
            return result;
        }

        void remove_trailing_separators(std::u8string& value) noexcept
        {
            // 드라이브 루트(`C:\`)는 마지막 구분자를 남겨야 경로가 유효하다.
            while (value.size() > 1 && is_separator(value.back()))
            {
                if (value.size() == 3 && value[1] == u8':')
                    return;
                value.pop_back();
            }
        }

        std::u8string join_path(const std::u8string_view directory, const std::u8string_view file)
        {
            std::u8string result { directory };
            if (result.empty() == false && is_separator(result.back()) == false)
                result.push_back(u8'\\');
            result.append(file);
            return result;
        }

        void add_unique_candidate(std::vector<git_tool_candidate>& candidates, std::u8string executable)
        {
            const auto duplicate {
                std::ranges::find_if(candidates, [&executable](const git_tool_candidate& value) { return ascii_equal_ignoring_case(value.executable, executable); }),
            };
            if (duplicate != candidates.end())
                return;
            candidates.push_back({ std::move(executable), false });
        }

        // 알려진 기본 설치 위치다. `PATH`에 등록하지 않고 설치하는 배포판이 있어 마지막
        // 순서로만 확인한다.
        std::vector<std::u8string> default_relative_directories()
        {
            return {
                std::u8string { u8"Git\\cmd" },
                std::u8string { u8"Git\\bin" },
            };
        }

        diagnostic make_tool_diagnostic(const diagnostic_code code, const diagnostic_severity severity, const std::u8string_view message)
        {
            diagnostic value {};
            value.code = code;
            value.severity = severity;
            value.message = message;
            return value;
        }

        git_tool_info make_unavailable_tool(const git_tool_availability availability, const diagnostic_code code, const bool manually_configured, std::u8string executable)
        {
            git_tool_info info {};
            info.availability = availability;
            info.executable = std::move(executable);
            info.manually_configured = manually_configured;
            // 도구 부재는 앱을 멈추는 오류가 아니다. 저장소 열기 화면은 계속 쓸 수 있으므로
            // warning으로 보고한다.
            info.diagnostics.push_back(make_tool_diagnostic(code, diagnostic_severity::warning, git_tool_unavailable_message(availability)));
            return info;
        }

        std::optional<git_tool_candidate> find_existing_candidate(const std::vector<git_tool_candidate>& candidates, const file_probe& probe)
        {
            for (const git_tool_candidate& candidate : candidates)
                if (probe.probe(candidate.executable) == path_kind::file)
                    return candidate;
            return std::nullopt;
        }

        git_tool_info probe_tool_version(const git_tool_candidate& candidate, process_runner& runner, const process_cancellation_token& token)
        {
            std::vector<std::u8string> arguments {};
            arguments.push_back(std::u8string { u8"--version" });

            // 작업 디렉터리는 실행 파일이 있는 디렉터리를 쓴다. 요청 검증이 절대 경로를
            // 요구하고, 이 시점에는 대상 저장소가 정해지지 않았기 때문이다.
            const process_request request {
                make_git_process_request(candidate.executable, executable_directory(candidate.executable), std::move(arguments), git_command_class::tool_probe),
            };
            const git_command_result result { run_git_command(runner, request, token) };

            if (result.succeeded() == false)
            {
                // 사용자가 직접 지정한 경로가 실행되지 않으면 자동 탐색으로 물러서지
                // 않는다. 의도한 값이 조용히 무시되는 편이 더 위험하다.
                const git_tool_availability availability { candidate.manually_configured ? git_tool_availability::path_invalid : git_tool_availability::version_unreadable };
                const diagnostic_code code { candidate.manually_configured ? diagnostic_code::git_tool_path_invalid : diagnostic_code::git_tool_version_unreadable };
                return make_unavailable_tool(availability, code, candidate.manually_configured, candidate.executable);
            }

            const std::u8string first_line { result.first_output_line() };
            const std::optional<git_tool_version> version { parse_git_tool_version(first_line) };
            if (version.has_value() == false)
            {
                const diagnostic_code code { diagnostic_code::git_tool_version_unreadable };
                git_tool_info info { make_unavailable_tool(git_tool_availability::version_unreadable, code, candidate.manually_configured, candidate.executable) };
                info.reported_version = first_line;
                return info;
            }

            git_tool_info info {};
            info.executable = candidate.executable;
            info.manually_configured = candidate.manually_configured;
            info.reported_version = first_line;
            info.version = *version;
            if (meets_minimum_git_version(*version) == false)
            {
                info.availability = git_tool_availability::too_old;
                info.diagnostics.push_back(make_tool_diagnostic(diagnostic_code::git_tool_too_old, diagnostic_severity::warning, git_tool_unavailable_message(git_tool_availability::too_old)));
                return info;
            }

            info.availability = git_tool_availability::available;
            return info;
        }

        git_tool_info resolve_git_tool_impl(
            const std::u8string_view configured_executable, const git_tool_environment& environment, process_runner& runner, const file_probe& probe, const process_cancellation_token& token)
        {
            const bool manually_configured { configured_executable.empty() == false };
            if (manually_configured && is_absolute_windows_path(configured_executable) == false)
                return make_unavailable_tool(git_tool_availability::path_invalid, diagnostic_code::git_tool_path_invalid, true, std::u8string { configured_executable });

            const std::vector<git_tool_candidate> candidates { git_tool_candidates(configured_executable, environment) };
            const std::optional<git_tool_candidate> existing { find_existing_candidate(candidates, probe) };
            if (existing.has_value() == false)
            {
                const git_tool_availability availability { manually_configured ? git_tool_availability::path_invalid : git_tool_availability::not_found };
                const diagnostic_code code { manually_configured ? diagnostic_code::git_tool_path_invalid : diagnostic_code::git_tool_not_found };
                return make_unavailable_tool(availability, code, manually_configured, manually_configured ? std::u8string { configured_executable } : std::u8string {});
            }

            return probe_tool_version(*existing, runner, token);
        }
    } // namespace

    std::vector<std::u8string> split_search_path(const std::u8string_view path_environment)
    {
        std::vector<std::u8string> directories {};
        std::size_t begin { 0 };
        while (begin <= path_environment.size())
        {
            const std::size_t end { std::min(path_environment.find(u8';', begin), path_environment.size()) };
            std::u8string entry { strip_quotes(trim_ascii_spaces(path_environment.substr(begin, end - begin))) };
            begin = end + 1;

            remove_trailing_separators(entry);
            if (entry.empty() || is_absolute_windows_path(entry) == false)
                continue;
            const auto duplicate {
                std::ranges::find_if(directories, [&entry](const std::u8string& value) { return ascii_equal_ignoring_case(value, entry); }),
            };
            if (duplicate == directories.end())
                directories.push_back(std::move(entry));
        }
        return directories;
    }

    std::vector<git_tool_candidate> git_tool_candidates(const std::u8string_view configured_executable, const git_tool_environment& environment)
    {
        std::vector<git_tool_candidate> candidates {};
        if (configured_executable.empty() == false)
        {
            // 지정 경로가 있으면 다른 후보를 만들지 않는다. 자동 탐색으로 조용히
            // 물러서면 사용자가 지정한 값이 무시된 사실을 알 수 없다.
            candidates.push_back({ std::u8string { configured_executable }, true });
            return candidates;
        }

        for (const std::u8string& directory : split_search_path(environment.path_environment))
            add_unique_candidate(candidates, join_path(directory, git_executable_name));

        for (const std::u8string& program_files : environment.program_files_directories)
        {
            if (program_files.empty() || is_absolute_windows_path(program_files) == false)
                continue;
            for (const std::u8string& relative : default_relative_directories())
                add_unique_candidate(candidates, join_path(join_path(program_files, relative), git_executable_name));
        }
        return candidates;
    }

    std::u8string executable_directory(const std::u8string_view executable)
    {
        std::u8string value { executable };
        remove_trailing_separators(value);
        const std::size_t separator { value.find_last_of(u8"/\\") };
        if (separator == std::u8string::npos)
            return value;
        if (separator == 2 && value.size() >= 3 && value[1] == u8':')
        {
            value.resize(3);
            return value;
        }
        value.resize(separator);
        return value;
    }

    git_tool_info resolve_git_tool(
        const std::u8string_view configured_executable, const git_tool_environment& environment, process_runner& runner, const file_probe& probe, const process_cancellation_token& token) noexcept
    {
        try
        {
            return resolve_git_tool_impl(configured_executable, environment, runner, probe, token);
        }
        catch (...)
        {
            git_tool_info info {};
            info.availability = git_tool_availability::unknown;
            return info;
        }
    }
} // namespace cyella
