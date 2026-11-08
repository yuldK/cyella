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

        // `git config --unset-all`이 지울 키가 없을 때의 종료 코드다. 지울 것이 없으면 이미 원하는 상태다.
        constexpr int config_key_missing { 5 };

        git_command_result run_config(process_runner& runner, const std::u8string_view executable, const std::u8string_view repository_path, std::vector<std::u8string> arguments,
            const git_command_class command_class, const git_identity_options& options, const process_cancellation_token& token)
        {
            process_request request { make_git_process_request(executable, repository_path, std::move(arguments), command_class, make_options(options)) };
            request.timeout = identity_timeout;
            return run_git_command(runner, request, token);
        }

        std::u8string failure_message(const git_command_result& run)
        {
            std::u8string message { first_line_of(run.standard_error_text()) };
            return message.empty() ? tr(u8"git.identity.write_failed") : message;
        }
    } // namespace

    std::vector<std::u8string> make_identity_read_arguments()
    {
        // 출력은 `범위\tfile:경로\t키 값`이다. 마지막 값이 git이 쓰는 값이라 해석기가 그것을 고른다. 키 이름은 소문자로 맞춰 비교된다.
        return { u8"config", u8"--show-scope", u8"--show-origin", u8"--get-regexp", u8"^(user\\.(name|email|signingkey)|credential\\.username|core\\.sshcommand)$" };
    }

    std::vector<std::u8string> make_identity_write_arguments(const std::u8string_view key, const std::u8string_view value)
    {
        // `--local`을 명시해 전역 설정으로 새지 않게 한다. `--`는 `-`로 시작하는 값이 옵션으로 읽히지 않게 한다.
        return { u8"config", u8"--local", u8"--", std::u8string { key }, std::u8string { value } };
    }

    std::vector<std::u8string> make_identity_unset_arguments(const std::u8string_view key)
    {
        // 같은 키가 여러 줄이면 `--unset`은 거절하므로 모두 지운다.
        return { u8"config", u8"--local", u8"--unset-all", u8"--", std::u8string { key } };
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

    git_identity_write git_identity_gateway::apply(const std::u8string_view repository_path, const local_identity_change& change, const process_cancellation_token& token) const
    {
        git_identity_write result {};
        if (change.clear == false && (change.user_name.empty() || change.user_email.empty()))
        {
            result.message = tr(u8"git.identity.name_and_email_required");
            return result;
        }
        const auto write = [&](const std::u8string_view key, const std::u8string_view value) {
            const git_command_result run { run_config(*runner_, git_executable_, repository_path, make_identity_write_arguments(key, value), git_command_class::local_write, options_, token) };
            if (run.succeeded() == false)
                result.message = failure_message(run);
            return run.succeeded();
        };
        const auto unset = [&](const std::u8string_view key) {
            const git_command_result run { run_config(*runner_, git_executable_, repository_path, make_identity_unset_arguments(key), git_command_class::local_write, options_, token) };
            const bool done { run.succeeded() || (run.process.completion == process_completion::exited && run.process.exit_code == config_key_missing) };
            if (done == false)
                result.message = failure_message(run);
            return done;
        };
        // 저장소의 `core.sshCommand`가 프로필이 쓴 꼴인가. 사용자가 손으로 쓴 명령(프록시, 다른 ssh)은 앱이 지우지 않는다.
        const auto ssh_command_is_ours = [&]() {
            const git_command_result run {
                run_config(*runner_, git_executable_, repository_path, { u8"config", u8"--local", u8"--get", u8"core.sshCommand" }, git_command_class::local_query, options_, token)
            };
            return run.succeeded() && profile_ssh_key_of(run.first_output_line()).has_value();
        };

        if (change.clear)
        {
            if (unset(u8"user.name") == false || unset(u8"user.email") == false || unset(u8"credential.username") == false)
                return result;
            if (ssh_command_is_ours() && unset(u8"core.sshCommand") == false)
                return result;
            result.ok = true;
            result.message = tr(u8"git.identity.cleared");
            return result;
        }
        if (write(u8"user.name", change.user_name) == false || write(u8"user.email", change.user_email) == false)
            return result;
        if (change.credential_username.empty() ? unset(u8"credential.username") == false : write(u8"credential.username", change.credential_username) == false)
            return result;
        if (change.ssh_command.empty() == false)
        {
            if (write(u8"core.sshCommand", change.ssh_command) == false)
                return result;
        }
        else if (ssh_command_is_ours() && unset(u8"core.sshCommand") == false)
            return result;
        result.ok = true;
        result.message = tr(u8"git.identity.applied");
        return result;
    }
} // namespace cyella
