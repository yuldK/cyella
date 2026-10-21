#include "infrastructure/git_operation_context.h"

#include "application/process_runner.h"
#include "domain/path_syntax.h"
#include "domain/text_catalog.h"
#include "infrastructure/git_progress_parser.h"
#include "infrastructure/git_query_parser.h"
#include "infrastructure/git_signing_failure.h"

#include <algorithm>
#include <utility>

namespace cyella {
    namespace {
        // 진행 중 표지 파일과 단계 파일은 작다. 큰 파일을 잘못 읽어 메모리를 쓰지 않게 막는다.
        constexpr std::size_t marker_file_limit { 64u * 1024u };
        // Windows 명령줄 한도는 32,767자다. 공통 인자와 인용을 넉넉히 빼고 stdin 판으로 넘어간다.
        constexpr std::size_t inline_path_budget { 24u * 1024u };

        failure_kind map_failure_kind(const git_failure_kind kind) noexcept
        {
            switch (kind)
            {
            case git_failure_kind::unclassified:
                return failure_kind::other;
            case git_failure_kind::authentication_required:
            case git_failure_kind::permission_denied_publickey:
                return failure_kind::auth_failed;
            case git_failure_kind::host_key_verification_failed:
                return failure_kind::host_key_refused;
            case git_failure_kind::host_key_changed:
                return failure_kind::host_key_changed;
            case git_failure_kind::could_not_resolve_host:
            case git_failure_kind::connection_failed:
                return failure_kind::offline;
            case git_failure_kind::repository_not_found:
                return failure_kind::not_found;
            case git_failure_kind::push_rejected_non_fast_forward:
                return failure_kind::rejected_non_ff;
            case git_failure_kind::push_rejected_fetch_first:
                return failure_kind::rejected_fetch_first;
            case git_failure_kind::push_rejected_stale_info:
                return failure_kind::stale_info;
            case git_failure_kind::push_rejected:
                return failure_kind::rejected;
            case git_failure_kind::conflict:
                return failure_kind::conflict;
            case git_failure_kind::index_lock_exists:
                return failure_kind::index_lock;
            case git_failure_kind::local_changes_would_be_overwritten:
                return failure_kind::dirty_worktree;
            case git_failure_kind::not_possible_to_fast_forward:
                return failure_kind::not_possible_to_fast_forward;
            case git_failure_kind::branch_checked_out_elsewhere:
                return failure_kind::branch_checked_out_elsewhere;
            case git_failure_kind::signing_failed:
                return failure_kind::signing_failed;
            case git_failure_kind::signing_key_missing:
                return failure_kind::signing_key_missing;
            case git_failure_kind::signing_passphrase_failed:
                return failure_kind::signing_passphrase_failed;
            }
            return failure_kind::other;
        }

        std::u8string join_lines(const std::vector<std::u8string>& lines)
        {
            std::u8string text {};
            for (const std::u8string& line : lines)
            {
                if (text.empty() == false)
                    text.push_back(u8'\n');
                text.append(line);
            }
            return text;
        }

        std::u8string to_decimal(const std::uint32_t value)
        {
            const std::string digits { std::to_string(value) };
            return std::u8string { digits.begin(), digits.end() };
        }
    } // namespace

    // 실행 문맥의 출력 수집기다. stdout 조각은 모으거나 스트림 해석기에 넘기고, stderr 줄은
    // 진행 표시면 진행률 사건으로, 아니면 실패 설명으로 모은다. runner가 호출을 직렬화한다.
    class git_operation_sink final : public process_output_sink
    {
    public:
        git_operation_sink(git_operation_context& context, git_command_outcome& outcome, const std::function<void(std::string_view)>& consumer) noexcept
            : context_ { context }
            , outcome_ { outcome }
            , consumer_ { consumer }
        {}

        void on_record(const process_output_record& record) override
        {
            if (record.stream != process_stream::standard_error)
                return;
            // `\r`로 끝난 진행 줄과, 단계가 끝날 때 `\n`으로 끝나는 마지막 진행 줄(`, done.`)이
            // 모두 진행 표시다. 둘 다 실패 설명에는 넣지 않는다.
            if (parse_git_progress(record.text).has_value())
            {
                context_.report_progress(record.text);
                return;
            }
            if (record.progress)
                return;
            outcome_.standard_error_lines.push_back(record.text);
        }

        void on_chunk(const process_output_chunk& chunk) override
        {
            if (chunk.stream != process_stream::standard_output)
                return;
            if (progress_on_standard_output_)
                feed_progress(chunk.bytes);
            if (consumer_ != nullptr)
                consumer_(chunk.bytes);
            else
                outcome_.standard_output.append(chunk.bytes);
        }

        void enable_standard_output_progress() noexcept
        {
            progress_on_standard_output_ = true;
        }

    private:
        // stdout을 줄바꿈(CR·LF)으로 끊어 진행 줄이면 진행률로 보낸다. 조각 경계에 걸린 줄은 다음 조각과 이어 붙인다.
        void feed_progress(const std::string_view bytes)
        {
            for (const char value : bytes)
            {
                if (value == '\r' || value == '\n')
                {
                    flush_progress_line();
                    continue;
                }
                if (progress_tail_.size() < 4096)
                    progress_tail_.push_back(static_cast<char8_t>(value));
            }
        }

        void flush_progress_line()
        {
            if (progress_tail_.empty())
                return;
            if (parse_git_progress(progress_tail_).has_value())
                context_.report_progress(progress_tail_);
            progress_tail_.clear();
        }

        git_operation_context& context_;
        git_command_outcome& outcome_;
        const std::function<void(std::string_view)>& consumer_;
        bool progress_on_standard_output_ { false };
        std::u8string progress_tail_ {};
    };

    operation_outcome operation_outcome::succeeded(std::u8string summary)
    {
        operation_outcome outcome {};
        outcome.state = operation_state::succeeded;
        outcome.summary = std::move(summary);
        return outcome;
    }

    operation_outcome operation_outcome::failed(const failure_kind kind, std::u8string summary, std::u8string detail)
    {
        operation_outcome outcome {};
        outcome.state = operation_state::failed;
        operation_failure failure {};
        failure.kind = kind;
        failure.summary = summary.empty() ? std::u8string { failure_kind_message(kind) } : std::move(summary);
        failure.detail = std::move(detail);
        outcome.failure = std::move(failure);
        return outcome;
    }

    operation_outcome operation_outcome::cancelled()
    {
        operation_outcome outcome {};
        outcome.state = operation_state::cancelled;
        return outcome;
    }

    operation_outcome operation_outcome::needs(input_request request)
    {
        operation_outcome outcome {};
        outcome.state = operation_state::needs_input;
        outcome.needs_input = std::move(request);
        return outcome;
    }

    std::u8string git_command_outcome::standard_error_text() const
    {
        return join_lines(standard_error_lines);
    }

    std::string git_command_outcome::standard_error_bytes() const
    {
        return to_bytes(standard_error_text());
    }

    git_operation_context::git_operation_context(const git_operation_runner& owner, const git_operation_job& job, const operation_event_sink& emit)
        : owner_ { owner }
        , job_ { job }
        , emit_ { emit }
    {}

    void git_operation_context::emit(operation_event_payload payload) const
    {
        if (emit_ == nullptr)
            return;
        operation_event event {};
        event.id = job_.id;
        event.repository_id = job_.repository_id;
        event.payload = std::move(payload);
        emit_(std::move(event));
    }

    git_command_outcome git_operation_context::run(git_command_spec spec)
    {
        git_command_outcome outcome {};
        const std::u8string& directory { spec.working_directory.empty() ? job_.repository_path : spec.working_directory };

        git_request_options options {};
        options.askpass_helper = owner_.options().askpass_helper;
        if (options.askpass_helper.empty() == false)
        {
            // 도우미가 질문을 이 operation에 묶어 보내고, 취소할 때도 이 id로 대기 질문을 찾는다.
            // 명령마다 같은 값이라 한 operation의 여러 git 명령이 같은 id로 묻는다.
            const std::string operation_digits { std::to_string(job_.id) };
            options.askpass.pipe_name = owner_.options().askpass.pipe_name;
            options.askpass.token = owner_.options().askpass.token;
            options.askpass.operation_id = std::u8string { operation_digits.begin(), operation_digits.end() };
        }
        if (const temporary_ssh_trust* const trust { ssh_override_of(job_.operation) }; trust != nullptr)
            options.ssh_command = trust->command;
        options.standard_input = std::move(spec.standard_input);
        options.environment = std::move(spec.environment);
        options.raw_standard_output = true;
        if (spec.on_standard_output != nullptr)
            options.maximum_captured_bytes_per_stream = unlimited_process_capture_bytes;
        const process_request request { make_git_process_request(owner_.git_executable(), directory, std::move(spec.arguments), spec.command_class, options) };

        {
            const git_operation_runner::activity_scope activity { owner_, directory };
            git_operation_sink sink { *this, outcome, spec.on_standard_output };
            if (spec.progress_on_standard_output)
                sink.enable_standard_output_progress();
            outcome.process = owner_.runner().run(request, &sink, job_.cancellation);
        }

        // 사용자가 인증 대화 상자를 답 없이 닫았다. 이 명령의 인증 실패는 `auth_cancelled`로 보고한다.
        if (job_.prompt_cancelled != nullptr && job_.prompt_cancelled->load())
            note_authentication_cancelled();

        const std::optional<std::int32_t> code { outcome.process.exit_code };
        outcome.accepted = outcome.process.completion == process_completion::exited && code.has_value()
            && std::find(spec.accepted_exit_codes.begin(), spec.accepted_exit_codes.end(), *code) != spec.accepted_exit_codes.end();

        operation_command command {};
        command.command_line = outcome.process.masked_command_line;
        command.completion = outcome.process.completion;
        command.exit_code = outcome.process.exit_code;
        command.duration = outcome.process.duration;
        emit(operation_command_finished { std::move(command) });
        return outcome;
    }

    bool git_operation_context::run_required(git_command_spec spec, git_command_outcome& outcome, std::optional<operation_outcome>& stop)
    {
        outcome = run(std::move(spec));
        if (outcome.accepted)
            return true;
        stop = failure_from(outcome);
        return false;
    }

    operation_outcome git_operation_context::failure_from(const git_command_outcome& outcome) const
    {
        switch (outcome.process.completion)
        {
        case process_completion::cancelled:
            return operation_outcome::cancelled();
        case process_completion::timed_out:
            return operation_outcome::failed(failure_kind::timed_out);
        case process_completion::start_failed:
        case process_completion::invalid_request:
        case process_completion::internal_error: {
            std::u8string detail {};
            for (const diagnostic& entry : outcome.process.diagnostics)
            {
                if (detail.empty() == false)
                    detail.push_back(u8'\n');
                detail.append(entry.message);
            }
            return operation_outcome::failed(failure_kind::other, tr(u8"git.run_failed"), std::move(detail));
        }
        case process_completion::exited:
            break;
        }

        const std::string standard_error { outcome.standard_error_bytes() };
        const git_failure_kind classified { classify_git_failure(standard_error, outcome.standard_output) };
        failure_kind kind { map_failure_kind(classified) };
        // 호스트 키 대화 상자를 닫은 것도 취소다. ssh는 답이 없으면 "Host key verification failed."로 끝나지만,
        // 사용자가 `no`라고 답한 것(`host_key_refused`)과는 다르다.
        // 서명 키의 passphrase 대화 상자를 닫은 것도 같다. ssh-keygen이 빈 답으로 "incorrect passphrase"를 내므로 사용자가 취소한 것과 구별한다.
        if ((kind == failure_kind::auth_failed || kind == failure_kind::host_key_refused || kind == failure_kind::signing_passphrase_failed) && authentication_cancelled_)
            kind = failure_kind::auth_cancelled;

        std::u8string detail { outcome.standard_error_text() };
        // 충돌 문구(`CONFLICT (content): ...`)는 stdout으로 나온다. 원문을 함께 보여야 이유가 보인다.
        if (kind == failure_kind::conflict && outcome.standard_output.empty() == false)
        {
            if (detail.empty() == false)
                detail.push_back(u8'\n');
            detail.append(to_utf8(outcome.standard_output));
        }
        std::u8string summary {};
        if (kind == failure_kind::host_key_changed)
        {
            if (const auto location { find_offending_known_hosts_line(standard_error) }; location.has_value() && location->line != 0)
                summary = std::u8string { failure_kind_message(kind) } + u8" (" + to_utf8(location->path) + u8":" + to_decimal(location->line) + u8")";
        }
        if (kind == failure_kind::branch_checked_out_elsewhere)
        {
            summary = std::u8string { failure_kind_message(kind) };
            if (const auto path { find_checked_out_worktree_path(standard_error) }; path.has_value())
                summary += u8" (" + to_utf8(*path) + u8")";
        }
        if (kind == failure_kind::signing_failed || kind == failure_kind::signing_key_missing || kind == failure_kind::signing_passphrase_failed)
            summary = signing_failure_summary(kind, standard_error);
        return operation_outcome::failed(kind, std::move(summary), std::move(detail));
    }

    std::optional<git_status_result> git_operation_context::read_status()
    {
        git_command_spec spec {};
        spec.arguments = make_git_status_args();
        const git_command_outcome outcome { run(std::move(spec)) };
        if (outcome.accepted == false)
            return std::nullopt;
        return parse_git_status(outcome.standard_output);
    }

    std::string git_operation_context::read_git_dir()
    {
        git_command_spec spec {};
        spec.arguments = make_git_rev_parse_args();
        const git_command_outcome outcome { run(std::move(spec)) };
        // bare 저장소에서는 `--show-toplevel`이 실패하지만 앞의 git 디렉터리는 이미 나왔다.
        const std::string_view text { outcome.standard_output };
        std::string_view first { text.substr(0, text.find('\n')) };
        if (first.empty() == false && first.back() == '\r')
            first.remove_suffix(1);
        if (outcome.process.completion != process_completion::exited)
            return {};
        return std::string { first };
    }

    in_progress_operation git_operation_context::read_in_progress(const std::string_view git_dir) const
    {
        if (git_dir.empty())
            return {};
        const std::u8string base { to_utf8(git_dir) };
        std::vector<std::string> existing {};
        for (const std::string_view marker : repository_operation_marker_names)
            if (owner_.files().probe(join_path(base, marker)) != path_kind::missing)
                existing.emplace_back(marker);

        const repository_operation_state kind { detect_repository_operation_state(existing) };
        workspace_files& files { owner_.files() };
        return read_in_progress_operation(kind, [&files, &base](const std::string_view relative) { return files.read_text(join_path(base, relative), marker_file_limit); });
    }

    std::string git_operation_context::read_current_branch()
    {
        git_command_spec spec {};
        spec.arguments = make_git_current_branch_args();
        spec.accepted_exit_codes = { 0, 1 };
        const git_command_outcome outcome { run(std::move(spec)) };
        if (outcome.accepted == false || outcome.exit_code() != 0)
            return {};
        std::string name { outcome.standard_output };
        while (name.empty() == false && (name.back() == '\n' || name.back() == '\r'))
            name.pop_back();
        return name;
    }

    operation_outcome git_operation_context::finish_sequence(const git_command_outcome& outcome, std::u8string success_summary)
    {
        if (outcome.accepted)
            return operation_outcome::succeeded(std::move(success_summary));
        if (outcome.process.completion != process_completion::exited)
            return failure_from(outcome);

        const in_progress_operation progress { read_in_progress(read_git_dir()) };
        if (progress.kind != repository_operation_state::none)
        {
            const std::optional<git_status_result> status { read_status() };
            if (status.has_value() && status->conflicts.empty() == false)
            {
                input_request request {};
                request.kind = input_kind::conflict;
                request.sequence = progress.kind;
                for (const file_change& change : status->conflicts)
                    request.files.push_back(change.path);
                return operation_outcome::needs(std::move(request));
            }
        }
        return failure_from(outcome);
    }

    git_command_spec git_operation_context::path_command(
        const std::vector<std::string>& paths, git_arguments (*const inline_builder)(std::span<const std::string>), git_arguments (*const stdin_builder)(), const git_command_class command_class) const
    {
        std::size_t length { 0 };
        for (const std::string& path : paths)
            length += path.size() + 3;

        git_command_spec spec {};
        spec.command_class = command_class;
        if (length < inline_path_budget)
        {
            spec.arguments = inline_builder(paths);
            return spec;
        }
        spec.arguments = stdin_builder();
        for (const std::string& path : paths)
        {
            spec.standard_input.append(path);
            spec.standard_input.push_back('\0');
        }
        return spec;
    }

    void git_operation_context::report_progress(const std::u8string& line)
    {
        const std::optional<git_progress> parsed { parse_git_progress(line) };
        if (parsed.has_value() == false)
            return;
        // 단계가 바뀌거나 끝났으면 바로, 아니면 간격을 두고 보낸다.
        const auto now { std::chrono::steady_clock::now() };
        const bool phase_changed { parsed->phase_text != last_phase_ };
        const bool completed { parsed->fraction.has_value() && *parsed->fraction >= 1.0 };
        if (phase_changed == false && completed == false && now - last_progress_ < owner_.options().progress_interval)
            return;
        last_progress_ = now;
        last_phase_ = parsed->phase_text;
        emit(operation_progressed { operation_progress { parsed->phase_text, parsed->fraction } });
    }

    std::string to_bytes(const std::u8string_view text)
    {
        return std::string { text.begin(), text.end() };
    }

    std::u8string to_utf8(const std::string_view bytes)
    {
        return std::u8string { bytes.begin(), bytes.end() };
    }

    std::u8string join_path(const std::u8string_view base, const std::string_view relative)
    {
        std::u8string path { base };
        if (path.empty() == false && path.back() != u8'/' && path.back() != u8'\\')
            path.push_back(u8'/');
        path.append(relative.begin(), relative.end());
        return path;
    }

    std::u8string parent_directory(const std::u8string_view path)
    {
        std::u8string_view trimmed { path };
        while (trimmed.size() > 3 && (trimmed.back() == u8'/' || trimmed.back() == u8'\\'))
            trimmed.remove_suffix(1);
        const std::size_t separator { trimmed.find_last_of(u8"/\\") };
        if (separator == std::u8string_view::npos)
            return std::u8string { trimmed };
        // `C:/x`의 부모는 `C:/`다. 구분자를 남겨야 드라이브 기준 상대 경로가 되지 않는다.
        if (separator == 2 && trimmed.size() > 1 && trimmed[1] == u8':')
            return std::u8string { trimmed.substr(0, 3) };
        return std::u8string { trimmed.substr(0, separator) };
    }
} // namespace cyella
