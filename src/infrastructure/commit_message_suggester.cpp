#include "infrastructure/commit_message_suggester.h"

#include "domain/path_syntax.h"
#include "domain/process_execution.h"
#include "domain/text_catalog.h"
#include "infrastructure/git_command_builder.h"
#include "infrastructure/git_diff_parser.h"
#include "infrastructure/git_execution_policy.h"
#include "infrastructure/process_output_pipeline.h"
#include "infrastructure/secret_masking.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace cyella {
    namespace {
        // 패치를 받아 두는 상한이다. 프롬프트에는 어차피 96 KiB까지만 실리므로 넉넉한 여유만 둔다. 넘친 뒤쪽은 버려진다.
        constexpr std::size_t patch_capture_limit { std::size_t { 2 } * 1024 * 1024 };
        // 실패 알림에 싣는 이유의 상한(바이트)이다.
        constexpr std::size_t reason_limit { 300 };
        // 제안 명령의 출력을 받아 두는 상한이다. 프롬프트를 표준 오류로 되울리는 명령(codex)이 있어 프롬프트보다 넉넉해야 한다 —
        // 모자라면 뒤가 버려지는데 실패 이유는 그 뒤에 있다. 답의 상한(`commit_suggestion_output_limit`)은 sink가 따로 지킨다.
        constexpr std::size_t command_capture_limit { std::size_t { 4 } * 1024 * 1024 };
        // 패치에서 뺄 경로를 git에 넘기는 인자의 상한(바이트)이다. Windows 명령줄은 32,767자까지라 그 절반에서 멈춘다.
        constexpr std::size_t excluded_argument_byte_limit { std::size_t { 16 } * 1024 };
        // 뺄 경로 하나가 명령줄에서 차지하는 덧붙이 길이다 (`:(top,exclude,literal)`과 따옴표·공백).
        constexpr std::size_t excluded_argument_overhead { 26 };

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

        // 제안 명령의 출력이다. 답(표준 출력)은 상한까지만 모으고, 표준 오류는 마지막 줄만 남긴다. 표준 오류에 머리말과 프롬프트를
        // 되울린 뒤 맨 끝에 오류를 적는 명령이 있어, 첫 줄이 아니라 마지막 줄이 실패의 이유다.
        class answer_sink final : public process_output_sink
        {
        public:
            void on_record(const process_output_record& record) override
            {
                if (record.stream != process_stream::standard_error)
                    return;
                for (const char8_t value : record.text)
                    if (value != u8' ' && value != u8'\t' && value != u8'\r')
                    {
                        last_error_line = record.text;
                        return;
                    }
            }

            void on_chunk(const process_output_chunk& chunk) override
            {
                if (chunk.stream != process_stream::standard_output)
                    return;
                const std::size_t room { commit_suggestion_output_limit - std::min(commit_suggestion_output_limit, answer.size()) };
                answer.append(chunk.bytes.substr(0, room));
                if (chunk.bytes.size() > room)
                    answer_too_long = true;
            }

            std::string answer {};
            bool answer_too_long { false };
            std::u8string last_error_line {};
        };

        struct captured
        {
            process_result process {};
            std::string standard_output {};
            std::u8string standard_error {};

            [[nodiscard]] bool exited_with_zero() const noexcept
            {
                return process.completion == process_completion::exited && process.exit_code.has_value() && *process.exit_code == 0;
            }
        };

        std::u8string to_utf8(const std::string_view bytes)
        {
            bool replaced { false };
            return normalize_utf8_text(std::u8string_view { reinterpret_cast<const char8_t*>(bytes.data()), bytes.size() }, replaced);
        }

        std::string to_bytes(const std::u8string_view text)
        {
            return std::string { reinterpret_cast<const char*>(text.data()), text.size() };
        }

        // 첫 줄만, 상한까지만 남긴다. UTF-8 글자 가운데에서 끊지 않는다.
        std::u8string first_line(const std::u8string_view text)
        {
            std::u8string_view line { text.substr(0, text.find(u8'\n')) };
            while (line.empty() == false && (line.back() == u8'\r' || line.back() == u8' '))
                line.remove_suffix(1);
            if (line.size() > reason_limit)
            {
                std::size_t cut { reason_limit };
                while (cut > 0 && (line[cut] & 0xc0) == 0x80)
                    --cut;
                line = line.substr(0, cut);
            }
            return std::u8string { line };
        }

        bool is_script_path(const std::u8string_view path) noexcept
        {
            const auto ends_with_ignoring_case = [path](const std::u8string_view suffix) {
                if (path.size() < suffix.size())
                    return false;
                for (std::size_t index { 0 }; index < suffix.size(); ++index)
                {
                    char8_t value { path[path.size() - suffix.size() + index] };
                    if (value >= u8'A' && value <= u8'Z')
                        value = static_cast<char8_t>(value - u8'A' + u8'a');
                    if (value != suffix[index])
                        return false;
                }
                return true;
            };
            return ends_with_ignoring_case(u8".cmd") || ends_with_ignoring_case(u8".bat");
        }

        std::vector<std::u8string> split_lines(const std::u8string_view text)
        {
            std::vector<std::u8string> lines {};
            std::size_t start { 0 };
            while (start < text.size())
            {
                const std::size_t end { text.find(u8'\n', start) };
                std::u8string_view line { text.substr(start, end == std::u8string_view::npos ? std::u8string_view::npos : end - start) };
                if (line.empty() == false && line.back() == u8'\r')
                    line.remove_suffix(1);
                if (line.empty() == false)
                    lines.emplace_back(line);
                if (end == std::u8string_view::npos)
                    break;
                start = end + 1;
            }
            return lines;
        }
    } // namespace

    commit_message_suggester::commit_message_suggester(process_runner& runner, std::u8string git_executable, commit_message_suggester_options options)
        : runner_ { &runner }
        , git_executable_ { std::move(git_executable) }
        , options_ { std::move(options) }
    {}

    commit_suggestion_result_intent commit_message_suggester::suggest(const commit_suggestion_job& job, const process_cancellation_token& token) const
    {
        commit_suggestion_result_intent result {};
        result.serial = job.serial;
        result.repository_id = job.repository_id;

        const auto fail = [&result](std::u8string reason) {
            result.state = commit_suggestion_state::failed;
            result.error = std::move(reason);
            return result;
        };
        const auto cancelled = [&result] {
            result.state = commit_suggestion_state::cancelled;
            return result;
        };
        const auto run_git = [this, &job, &token](git_arguments arguments, const std::size_t capture_limit) {
            captured output {};
            capture_sink sink {};
            git_request_options request_options {};
            request_options.raw_standard_output = true;
            request_options.maximum_captured_bytes_per_stream = capture_limit;
            request_options.environment = options_.git_environment;
            const process_request request { make_git_process_request(git_executable_, job.repository_path, std::move(arguments), git_command_class::local_query, request_options) };
            output.process = runner_->run(request, &sink, token);
            output.standard_output = std::move(sink.standard_output);
            output.standard_error = std::move(sink.standard_error);
            return output;
        };

        try
        {
            // 1. 무엇이 staged인가. 파일 목록과 줄 수는 패치가 잘려도 프롬프트에 온전히 실린다.
            const captured numstat { run_git(make_git_diff_numstat_args(diff_source::staged), patch_capture_limit) };
            if (numstat.process.completion == process_completion::cancelled)
                return cancelled();
            if (numstat.exited_with_zero() == false)
                return fail(tr(u8"commit_suggestion.diff_failed", { { u8"reason", first_line(numstat.standard_error) } }));

            std::vector<commit_suggestion_file> files {};
            std::vector<std::string> excluded {};
            std::size_t excluded_bytes { 0 };
            for (const numstat_entry& entry : parse_git_numstat(numstat.standard_output))
            {
                commit_suggestion_file file {};
                file.path = to_utf8(entry.path);
                file.original_path = to_utf8(entry.original_path);
                file.lines_added = entry.lines_added;
                file.lines_removed = entry.lines_removed;
                file.is_binary = entry.is_binary;
                // 내용을 보내지 않을 파일은 git이 패치에서 빼게 한다. 이름 변경은 양쪽 이름을 다 빼야 지운 쪽 내용이 나오지 않는다.
                if (commit_suggestion_omits_content(file.path) || commit_suggestion_omits_content(file.original_path))
                {
                    excluded.push_back(entry.path);
                    excluded_bytes += entry.path.size() + excluded_argument_overhead;
                    if (entry.original_path.empty() == false)
                    {
                        excluded.push_back(entry.original_path);
                        excluded_bytes += entry.original_path.size() + excluded_argument_overhead;
                    }
                }
                files.push_back(std::move(file));
            }
            if (files.empty())
                return fail(tr(u8"commit_suggestion.nothing_staged"));

            // 2. 패치다. 받아 둔 것이 상한을 넘으면 뒤가 잘린 채로 오고, 아래에서 파일 경계로 다시 줄인다.
            // 뺄 경로가 너무 많거나 길어 다 넘기지 못하면 패치를 읽지 않는다. 내용을 가려야 할 파일이 섞여 나가는 것보다 목록만 보내는 쪽이 낫다.
            // 파일 목록이 잘려 왔을 때도 같다 — 못 본 뒤쪽에 가릴 파일이 있을 수 있다.
            const bool patch_skipped {
                excluded.size() > commit_suggestion_excluded_path_limit || excluded_bytes > excluded_argument_byte_limit || numstat.process.standard_output_truncated
            };
            captured patch {};
            if (patch_skipped == false)
            {
                patch = run_git(make_git_staged_patch_args(excluded), patch_capture_limit);
                if (patch.process.completion == process_completion::cancelled)
                    return cancelled();
                if (patch.exited_with_zero() == false)
                    return fail(tr(u8"commit_suggestion.diff_failed", { { u8"reason", first_line(patch.standard_error) } }));
            }

            // 3. 최근 제목이다. 커밋이 없는 저장소에서는 실패하고, 그때는 화면 언어로 쓰게 한다.
            const captured subjects { run_git(make_git_recent_subjects_args(commit_suggestion_recent_subject_count), patch_capture_limit) };
            if (subjects.process.completion == process_completion::cancelled)
                return cancelled();
            const std::vector<std::u8string> recent_subjects { subjects.exited_with_zero() ? split_lines(to_utf8(subjects.standard_output)) : std::vector<std::u8string> {} };

            commit_suggestion_prompt_inputs inputs {};
            inputs.instruction = job.instruction;
            inputs.language = job.language;
            inputs.recent_subjects = recent_subjects;
            inputs.files = files;
            inputs.patch = shorten_commit_suggestion_patch(to_utf8(patch.standard_output));
            if (patch_skipped || patch.process.standard_output_truncated)
                inputs.patch.shortened = true;

            // 4. 명령을 찾는다.
            std::optional<std::vector<std::u8string>> arguments { split_commit_suggestion_arguments(job.arguments) };
            if (arguments.has_value() == false)
                return fail(tr(u8"commit_suggestion.arguments_invalid"));
            if (is_script_path(job.executable))
                return fail(tr(u8"commit_suggestion.script_not_supported", { { u8"command", job.executable } }));
            const std::optional<std::u8string> executable { options_.resolve_executable != nullptr ? options_.resolve_executable(job.executable) : std::optional<std::u8string> {} };
            if (executable.has_value() == false || is_absolute_windows_path(*executable) == false)
                return fail(tr(u8"commit_suggestion.command_not_found", { { u8"command", job.executable } }));
            if (is_script_path(*executable))
                return fail(tr(u8"commit_suggestion.script_not_supported", { { u8"command", job.executable } }));

            // 5. 띄운다. 프롬프트는 표준 입력으로만 간다 — 명령줄에 실으면 길이 제한에 걸리고 프로세스 목록에 보인다.
            process_request request {};
            request.executable = *executable;
            request.arguments = std::move(*arguments);
            request.working_directory = options_.working_directory;
            request.standard_input = to_bytes(build_commit_suggestion_prompt(inputs));
            request.timeout = options_.timeout;
            request.maximum_captured_bytes_per_stream = command_capture_limit;
            request.text_encoding = process_text_encoding::active_code_page_fallback;
            request.standard_output_mode = process_output_mode::raw;
            answer_sink sink {};
            const process_result process { runner_->run(request, &sink, token) };

            switch (process.completion)
            {
            case process_completion::cancelled:
                return cancelled();
            case process_completion::timed_out:
                return fail(tr(u8"commit_suggestion.timed_out", { { u8"command", job.executable } }));
            case process_completion::exited:
                break;
            case process_completion::start_failed:
            case process_completion::invalid_request:
            case process_completion::internal_error:
                return fail(tr(u8"commit_suggestion.start_failed", { { u8"command", job.executable } }));
            }

            const std::u8string answer { to_utf8(sink.answer) };
            if (process.exit_code.has_value() == false || *process.exit_code != 0)
            {
                // 이유는 보통 표준 오류의 마지막 줄에 있지만, 실패를 표준 출력으로 내는 명령도 있다.
                std::u8string reason { first_line(sink.last_error_line) };
                if (reason.empty())
                    reason = mask_secrets(first_line(answer));
                return fail(tr(u8"commit_suggestion.command_failed", { { u8"code", static_cast<std::int64_t>(process.exit_code.value_or(-1)) }, { u8"reason", reason } }));
            }
            if (sink.answer_too_long || process.standard_output_truncated)
                return fail(tr(u8"commit_suggestion.answer_too_long"));

            result.message = parse_suggested_commit_message(answer);
            if (result.message.subject.empty())
                return fail(tr(u8"commit_suggestion.empty_answer"));
            result.state = commit_suggestion_state::succeeded;
            return result;
        }
        catch (...)
        {
            // 인자 조립이나 할당이 던져도 결과는 돌려준다. 돌려주지 않으면 화면이 "제안 중"에 머문다.
            return fail(tr(u8"commit_suggestion.start_failed", { { u8"command", job.executable } }));
        }
    }
} // namespace cyella
