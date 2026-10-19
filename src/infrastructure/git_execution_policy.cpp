#include "infrastructure/git_execution_policy.h"

#include <algorithm>
#include <utility>

namespace cyella {
    namespace {
        constexpr std::size_t kibibyte { 1024u };
        constexpr std::size_t mebibyte { 1024u * 1024u };

        process_environment_override set_variable(const std::u8string_view name, const std::u8string_view value)
        {
            return {
                std::u8string { name },
                std::u8string { value },
            };
        }

        process_environment_override remove_variable(const std::u8string_view name)
        {
            return {
                std::u8string { name },
                std::nullopt,
            };
        }
    } // namespace

    git_command_limits git_limits_for(const git_command_class command_class) noexcept
    {
        // 로컬 조회의 600초는 대형 저장소의 status가 수 분 걸리는 실측을 반영한 값이다.
        switch (command_class)
        {
        case git_command_class::tool_probe:
            return { std::chrono::milliseconds { 5000 }, 64u * kibibyte };
        case git_command_class::local_query:
            return { std::chrono::milliseconds { 600000 }, 8u * mebibyte };
        case git_command_class::local_write:
            return { std::chrono::milliseconds { 300000 }, 8u * mebibyte };
        case git_command_class::network:
            return { std::nullopt, 32u * mebibyte };
        }
        return { std::chrono::milliseconds { 600000 }, 8u * mebibyte };
    }

    std::vector<process_environment_override> git_environment_overrides(const std::u8string_view askpass_helper)
    {
        std::vector<process_environment_override> overrides {};
        overrides.push_back(set_variable(u8"GIT_TERMINAL_PROMPT", u8"0"));

        if (askpass_helper.empty())
        {
            // 사용자 환경에 남아 있는 askpass가 앱 밖의 창을 띄우지 않게 한다.
            overrides.push_back(remove_variable(u8"GIT_ASKPASS"));
            overrides.push_back(remove_variable(u8"SSH_ASKPASS"));
            overrides.push_back(remove_variable(u8"SSH_ASKPASS_REQUIRE"));
            return overrides;
        }

        overrides.push_back(set_variable(u8"GIT_ASKPASS", askpass_helper));
        overrides.push_back(set_variable(u8"SSH_ASKPASS", askpass_helper));
        // DISPLAY와 터미널이 없어도 ssh가 도우미를 쓰게 한다.
        overrides.push_back(set_variable(u8"SSH_ASKPASS_REQUIRE", u8"force"));
        return overrides;
    }

    std::vector<std::u8string> git_common_arguments()
    {
        return {
            // 비ASCII 경로를 8진 이스케이프 없이 UTF-8 원문으로 출력한다.
            std::u8string { u8"-c" },
            std::u8string { u8"core.quotepath=false" },
            // background `gc`가 출력 pipe를 상속한 채 남지 않게 한다.
            std::u8string { u8"-c" },
            std::u8string { u8"gc.auto=0" },
            // ANSI escape가 로그와 해석기에 섞이지 않게 한다.
            std::u8string { u8"-c" },
            std::u8string { u8"color.ui=false" },
            std::u8string { u8"--no-pager" },
        };
    }

    process_request make_git_process_request(const std::u8string_view executable, const std::u8string_view working_directory, std::vector<std::u8string> arguments,
        const git_command_class command_class, const git_request_options& options)
    {
        const git_command_limits limits { git_limits_for(command_class) };

        process_request request {};
        request.executable = executable;
        request.working_directory = working_directory;
        request.arguments = git_common_arguments();
        request.arguments.reserve(request.arguments.size() + arguments.size());
        for (std::u8string& argument : arguments)
            request.arguments.push_back(std::move(argument));
        request.environment_overrides = git_environment_overrides(options.askpass_helper);
        for (process_environment_override& entry : askpass_session_overrides(options.askpass_helper.empty() == false, options.askpass))
            request.environment_overrides.push_back(std::move(entry));
        request.standard_input = options.standard_input;
        request.timeout = limits.timeout;
        request.maximum_captured_bytes_per_stream = options.maximum_captured_bytes_per_stream.value_or(limits.maximum_captured_bytes_per_stream);
        request.maximum_record_bytes = std::max(options.maximum_record_bytes, minimum_process_record_byte_limit);
        // 로캘을 강제하지 않으므로 git이 시스템 언어 메시지를 낼 수 있다. Windows에서 그
        // 인코딩은 UTF-8일 수도 활성 code page일 수도 있어 fallback 모드를 쓴다. 유효한
        // UTF-8 레코드는 변환하지 않으므로 기계 판독 출력에는 영향이 없다.
        request.text_encoding = process_text_encoding::active_code_page_fallback;
        request.standard_output_mode = options.raw_standard_output ? process_output_mode::raw : process_output_mode::lines;
        request.standard_error_mode = process_output_mode::lines;
        return request;
    }
} // namespace cyella
