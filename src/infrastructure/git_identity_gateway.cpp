#include "infrastructure/git_identity_gateway.h"

#include "domain/text_catalog.h"
#include "infrastructure/git_command_runner.h"
#include "infrastructure/git_execution_policy.h"

#include <chrono>
#include <utility>

namespace cyella {
    namespace {
        constexpr std::chrono::seconds identity_timeout { 20 };

        git_request_options make_options(const git_identity_options& options)
        {
            git_request_options request {};
            request.environment = options.environment;
            return request;
        }

        std::u8string first_line_of(const std::u8string& text)
        {
            const std::size_t end { text.find(u8'\n') };
            return text.substr(0, end);
        }
    } // namespace

    std::vector<std::u8string> make_identity_read_arguments()
    {
        // 출력은 `범위\tfile:경로\t키 값`이다. 마지막 값이 git이 쓰는 값이라 해석기가 그것을 고른다.
        return { u8"config", u8"--show-scope", u8"--show-origin", u8"--get-regexp", u8"^user\\.(name|email|signingkey)$" };
    }

    std::vector<std::u8string> make_identity_write_arguments(const std::u8string_view key, const std::u8string_view value)
    {
        // `--local`을 명시해 전역 설정으로 새지 않게 한다. `--`는 `-`로 시작하는 값이 옵션으로 읽히지 않게 한다.
        return { u8"config", u8"--local", u8"--", std::u8string { key }, std::u8string { value } };
    }

    git_identity_gateway::git_identity_gateway(process_runner& runner, std::u8string git_executable, git_identity_options options)
        : runner_ { &runner }
        , git_executable_ { std::move(git_executable) }
        , options_ { std::move(options) }
    {}

    git_identity_read git_identity_gateway::read(const std::u8string_view repository_path, const process_cancellation_token& token) const
    {
        git_identity_read result {};
        process_request request { make_git_process_request(git_executable_, repository_path, make_identity_read_arguments(), git_command_class::local_query, make_options(options_)) };
        request.timeout = identity_timeout;
        const git_command_result run { run_git_command(*runner_, request, token) };
        if (run.process.completion != process_completion::exited || run.process.exit_code.has_value() == false)
        {
            result.error = tr(u8"git.identity.config_run_failed");
            return result;
        }
        // 종료 코드 1은 맞는 키가 하나도 없다는 뜻이다 (신원이 아직 설정되지 않은 저장소).
        if (*run.process.exit_code != 0 && *run.process.exit_code != 1)
        {
            result.error = first_line_of(run.standard_error_text());
            if (result.error.empty())
                result.error = tr(u8"git.identity.config_failed");
            return result;
        }
        result.identity = parse_git_identity(run.standard_output_text(), options_.hints);
        result.ok = true;
        return result;
    }

    git_identity_write git_identity_gateway::apply(
        const std::u8string_view repository_path, const std::u8string_view user_name, const std::u8string_view user_email, const process_cancellation_token& token) const
    {
        git_identity_write result {};
        if (user_name.empty() || user_email.empty())
        {
            result.message = tr(u8"git.identity.name_and_email_required");
            return result;
        }
        const std::pair<std::u8string_view, std::u8string_view> entries[] { { u8"user.name", user_name }, { u8"user.email", user_email } };
        for (const auto& [key, value] : entries)
        {
            process_request request { make_git_process_request(git_executable_, repository_path, make_identity_write_arguments(key, value), git_command_class::local_write, make_options(options_)) };
            request.timeout = identity_timeout;
            const git_command_result run { run_git_command(*runner_, request, token) };
            if (run.succeeded() == false)
            {
                result.message = first_line_of(run.standard_error_text());
                if (result.message.empty())
                    result.message = tr(u8"git.identity.write_failed");
                return result;
            }
        }
        result.ok = true;
        result.message = tr(u8"git.identity.applied");
        return result;
    }
} // namespace cyella
