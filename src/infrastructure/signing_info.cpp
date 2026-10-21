#include "infrastructure/signing_info.h"

#include "application/process_request.h"
#include "domain/path_syntax.h"
#include "domain/text_catalog.h"
#include "infrastructure/git_execution_policy.h"
#include "infrastructure/git_signing_commands.h"
#include "infrastructure/git_signing_parser.h"

#include <chrono>
#include <stdexcept>
#include <utility>

namespace cyella {
    namespace {
        constexpr std::chrono::seconds gpg_listing_timeout { 30 };

        class capture_sink final : public process_output_sink
        {
        public:
            void on_record(const process_output_record& record) override
            {
                if (record.stream != process_stream::standard_error)
                    return;
                if (standard_error.empty() == false)
                    standard_error.push_back(u8'\n');
                standard_error.append(record.text);
            }

            void on_chunk(const process_output_chunk& chunk) override
            {
                if (chunk.stream == process_stream::standard_output)
                    standard_output.append(chunk.bytes);
            }

            std::string standard_output {};
            std::u8string standard_error {};
        };

        struct captured
        {
            process_result process {};
            std::string standard_output {};
            std::u8string standard_error {};

            [[nodiscard]] bool exited_with(const std::int32_t code) const noexcept
            {
                return process.completion == process_completion::exited && process.exit_code.has_value() && *process.exit_code == code;
            }
        };

        std::u8string to_utf8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::u8string parent_of(const std::u8string_view path)
        {
            std::u8string trimmed { path };
            while (trimmed.empty() == false && (trimmed.back() == u8'\\' || trimmed.back() == u8'/'))
                trimmed.pop_back();
            const std::size_t cut { trimmed.find_last_of(u8"\\/") };
            if (cut == std::u8string::npos || cut == 0)
                return {};
            return trimmed.substr(0, cut);
        }

        std::u8string join(std::u8string base, const std::u8string_view tail)
        {
            while (base.empty() == false && (base.back() == u8'\\' || base.back() == u8'/'))
                base.pop_back();
            base.push_back(u8'\\');
            base.append(tail);
            return base;
        }

        std::string trim_line(std::string text)
        {
            while (text.empty() == false && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
                text.pop_back();
            return text;
        }

        std::u8string first_line(const std::u8string& text)
        {
            const std::size_t end { text.find(u8'\n') };
            std::u8string line { text.substr(0, end) };
            while (line.empty() == false && (line.back() == u8'\r' || line.back() == u8' '))
                line.pop_back();
            return line;
        }

        const std::u8string& git_directory(const signing_info_sources& sources, const bool use_repository)
        {
            // 전역 설정은 어느 폴더에서 읽고 써도 같다. 지역 설정은 저장소 폴더여야 한다.
            if (use_repository)
                return sources.repository_path.empty() ? sources.fallback_directory : sources.repository_path;
            return sources.fallback_directory.empty() ? sources.repository_path : sources.fallback_directory;
        }

        captured run_git(const signing_info_sources& sources, git_arguments arguments, const git_command_class command_class, const bool use_repository)
        {
            captured result {};
            capture_sink sink {};
            git_request_options options {};
            options.raw_standard_output = true;
            const process_request request { make_git_process_request(sources.git_executable, git_directory(sources, use_repository), std::move(arguments), command_class, options) };
            result.process = sources.runner->run(request, &sink, sources.cancellation);
            result.standard_output = std::move(sink.standard_output);
            result.standard_error = std::move(sink.standard_error);
            return result;
        }

        std::int64_t seconds_now()
        {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        }

        // gpg --list-secret-keys를 돌려 키 목록을 채운다. 읽지 못하면 이유를 `gpg_problem`에 남긴다.
        void list_gpg_keys(const signing_info_sources& sources, signing_info_snapshot& info)
        {
            info.gpg_pending = false;
            const std::string configured { info.config.effective.at(signing_key_id::gpg_program).value };
            const std::u8string configured_text { to_utf8(configured) };
            // 절대 경로인 gpg.program을 먼저 쓴다. `gpg`처럼 이름만 있으면 프로세스 요청이 PATH를 찾지 않으므로 Git for Windows의 gpg를 쓴다.
            if (configured.empty() == false && is_absolute_windows_path(configured_text))
            {
                if (sources.probe != nullptr && sources.probe->probe(configured_text) != path_kind::file)
                {
                    info.gpg_problem = tr(u8"signing.gpg_program_not_found", { { u8"path", configured_text } });
                    return;
                }
                info.gpg_executable = configured_text;
            }
            else
                info.gpg_executable = info.default_gpg;
            if (info.gpg_executable.empty())
            {
                info.gpg_problem = tr(u8"signing.gpg_not_found");
                return;
            }

            process_request request {};
            request.executable = info.gpg_executable;
            request.working_directory = parent_of(info.gpg_executable);
            for (const std::u8string& argument : make_gpg_list_secret_keys_args())
                request.arguments.push_back(argument);
            request.timeout = gpg_listing_timeout;
            request.maximum_captured_bytes_per_stream = 4u * 1024u * 1024u;
            request.standard_output_mode = process_output_mode::raw;
            capture_sink sink {};
            const process_result result { sources.runner->run(request, &sink, sources.cancellation) };
            if (result.completion != process_completion::exited || result.exit_code.has_value() == false)
            {
                info.gpg_problem = tr(u8"signing.gpg_run_failed");
                return;
            }
            if (*result.exit_code != 0)
            {
                info.gpg_problem = tr(u8"signing.gpg_list_failed", { { u8"reason", first_line(sink.standard_error) } });
                return;
            }
            info.gpg_keys = parse_gpg_secret_keys(sink.standard_output, sources.now != 0 ? sources.now : seconds_now());
            info.gpg_keys_read = true;
        }
    } // namespace

    std::u8string locate_default_gpg(const std::u8string_view git_executable, const file_probe& probe)
    {
        std::u8string directory { parent_of(git_executable) };
        for (int level { 0 }; level < 4 && directory.empty() == false; ++level)
        {
            const std::u8string candidate { join(join(directory, u8"usr\\bin"), u8"gpg.exe") };
            if (probe.probe(candidate) == path_kind::file)
                return candidate;
            directory = parent_of(directory);
        }
        return {};
    }

    signing_info_snapshot collect_signing_info(const signing_info_sources& sources) noexcept
    {
        signing_info_snapshot info {};
        try
        {
            info.repository = sources.repository_path.empty() == false;
            if (sources.probe != nullptr)
                info.default_gpg = locate_default_gpg(sources.git_executable, *sources.probe);
            if (sources.runner == nullptr || sources.git_executable.empty() || git_directory(sources, true).empty())
                return info;

            // 설정이 하나도 없으면 종료 코드 1이다. 그것도 읽은 결과다.
            const captured config { run_git(sources, make_git_signing_config_args(), git_command_class::local_query, true) };
            if (config.exited_with(0) || config.exited_with(1))
            {
                info.config_read = true;
                if (config.exited_with(0))
                    info.config = parse_git_signing_config(config.standard_output);
            }

            if (info.config.effective.at(signing_key_id::gpg_ssh_allowed_signers).is_set())
            {
                const captured path { run_git(sources, make_git_allowed_signers_path_args(), git_command_class::local_query, true) };
                if (path.exited_with(0))
                {
                    info.allowed_signers_path = to_utf8(trim_line(path.standard_output));
                    info.allowed_signers_exists = sources.probe != nullptr && info.allowed_signers_path.empty() == false && sources.probe->probe(info.allowed_signers_path) == path_kind::file;
                }
            }

            if (sources.read_gpg_keys)
                list_gpg_keys(sources, info);
            else
                info.gpg_pending = true;
        }
        catch (...)
        {
            // 읽은 데까지가 결과다. 화면은 읽지 못한 부분을 "알 수 없음"으로 보인다.
        }
        return info;
    }

    void read_signing_gpg_keys(const signing_info_sources& sources, signing_info_snapshot& info) noexcept
    {
        try
        {
            list_gpg_keys(sources, info);
        }
        catch (...)
        {
            info.gpg_pending = false;
            info.gpg_problem = tr(u8"signing.gpg_list_unexpected_error");
        }
    }

    signing_write_result write_signing_config(const signing_info_sources& sources, const bool global, const std::vector<signing_change>& changes) noexcept
    {
        signing_write_result result {};
        try
        {
            if (sources.runner == nullptr || sources.git_executable.empty())
            {
                result.error = tr(u8"signing.git_not_found");
                return result;
            }
            if (global == false && sources.repository_path.empty())
            {
                result.error = tr(u8"signing.repository_tab_required");
                return result;
            }
            for (const signing_change& change : changes)
            {
                git_arguments arguments { change.value.has_value() ? make_git_signing_config_set_args(change.key, *change.value, global) : make_git_signing_config_unset_args(change.key, global) };
                const captured outcome { run_git(sources, std::move(arguments), git_command_class::local_write, global == false) };
                // 지우려는 설정이 이미 없으면 종료 코드 5다. 원하는 상태이므로 실패가 아니다.
                if (outcome.exited_with(0) || (change.value.has_value() == false && outcome.exited_with(5)))
                    continue;
                const std::u8string name { to_utf8(signing_key_name(change.key)) };
                result.error = change.value.has_value() ? tr(u8"signing.config_set_failed", { { u8"name", name } }) : tr(u8"signing.config_unset_failed", { { u8"name", name } });
                if (const std::u8string reason { first_line(outcome.standard_error) }; reason.empty() == false)
                    result.error += u8" " + reason;
                return result;
            }
            result.ok = true;
        }
        catch (const std::invalid_argument& error)
        {
            const std::string_view what { error.what() };
            result.error = tr(u8"signing.invalid_value", { { u8"detail", to_utf8(what) } });
        }
        catch (...)
        {
            result.error = tr(u8"signing.config_unexpected_error");
        }
        return result;
    }
} // namespace cyella
