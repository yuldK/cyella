#include "infrastructure/git_operation_context.h"

#include "domain/path_syntax.h"
#include "infrastructure/git_diff_parser.h"
#include "infrastructure/git_extension_operations.h"
#include "infrastructure/git_interactive_rebase.h"
#include "infrastructure/git_log_parser.h"
#include "infrastructure/git_query_parser.h"
#include "infrastructure/git_ref_parser.h"
#include "infrastructure/git_stash_parser.h"
#include "infrastructure/git_text.h"

#include <chrono>
#include <type_traits>
#include <utility>

namespace cyella {
    namespace {
        std::string trim_line(std::string text)
        {
            while (text.empty() == false && (text.back() == '\n' || text.back() == '\r'))
                text.pop_back();
            return text;
        }

        std::vector<std::string> split_lines(const std::string_view text)
        {
            std::vector<std::string> lines {};
            std::string_view rest { text };
            while (rest.empty() == false)
            {
                std::string_view line {};
                git_text::take_field(rest, '\n', line);
                if (line.empty() == false && line.back() == '\r')
                    line.remove_suffix(1);
                lines.emplace_back(line);
            }
            return lines;
        }

        // log 스트림을 묶음으로 끊어 logic thread에 넘긴다. 해석기 콜백은 runner의 읽기
        // 스레드에서 불리므로 이 객체도 그 스레드에서만 쓴다.
        class commit_batcher
        {
        public:
            commit_batcher(git_operation_context& context, const std::size_t first_index)
                : context_ { context }
                , next_index_ { first_index }
                , last_flush_ { std::chrono::steady_clock::now() }
            {}

            void add(commit_info&& commit)
            {
                pending_.push_back(std::move(commit));
                ++count_;
                const git_operation_runner_options& options { context_.owner().options() };
                if (pending_.size() >= options.log_batch_size || std::chrono::steady_clock::now() - last_flush_ >= options.log_flush_interval)
                    flush(false, false);
            }

            void flush(const bool page_complete, const bool history_complete)
            {
                if (pending_.empty() && page_complete == false)
                    return;
                commits_loaded batch {};
                batch.generation = context_.job().generation;
                batch.first_index = next_index_;
                batch.page_complete = page_complete;
                batch.history_complete = history_complete;
                next_index_ += pending_.size();
                batch.commits = std::move(pending_);
                pending_ = {};
                last_flush_ = std::chrono::steady_clock::now();
                context_.emit(std::move(batch));
            }

            [[nodiscard]] std::size_t count() const noexcept
            {
                return count_;
            }

        private:
            git_operation_context& context_;
            std::vector<commit_info> pending_ {};
            std::size_t next_index_ { 0 };
            std::size_t count_ { 0 };
            std::chrono::steady_clock::time_point last_flush_ {};
        };

        operation_outcome stream_log(git_operation_context& context, const std::size_t skip, const bool include_head)
        {
            const git_operation_runner_options& options { context.owner().options() };
            commit_batcher batcher { context, skip };
            git_log_parser parser { [&batcher](commit_info&& commit) { batcher.add(std::move(commit)); } };

            git_log_options log_options {};
            log_options.max_count = static_cast<std::uint32_t>(options.log_page_size);
            log_options.skip = static_cast<std::uint32_t>(skip);
            log_options.include_head = include_head;

            git_command_spec spec {};
            spec.arguments = make_git_log_args(log_options);
            spec.on_standard_output = [&parser](const std::string_view bytes) { parser.feed(bytes); };
            const git_command_outcome outcome { context.run(std::move(spec)) };
            parser.finish();
            if (outcome.accepted == false)
                return context.failure_from(outcome);

            batcher.flush(true, batcher.count() < options.log_page_size);
            return operation_outcome::succeeded();
        }

        // 원격마다 태그 목록을 읽는다. 하나가 실패해도 나머지 원격은 계속 본다.
        operation_outcome refresh_remote_tags(git_operation_context& context, std::vector<std::string> remotes)
        {
            if (remotes.empty())
            {
                git_command_spec list {};
                list.arguments = make_git_remote_list_args();
                git_command_outcome outcome {};
                std::optional<operation_outcome> stop {};
                if (context.run_required(std::move(list), outcome, stop) == false)
                    return *stop;
                for (const remote_info& remote : parse_git_remotes(outcome.standard_output))
                    remotes.push_back(remote.name);
            }

            std::optional<operation_outcome> last_failure {};
            std::size_t succeeded { 0 };
            for (const std::string& remote : remotes)
            {
                git_command_spec spec {};
                spec.arguments = make_git_ls_remote_tags_args(remote);
                spec.command_class = git_command_class::network;
                const git_command_outcome outcome { context.run(std::move(spec)) };
                if (outcome.accepted == false)
                {
                    last_failure = context.failure_from(outcome);
                    if (last_failure->state == operation_state::cancelled)
                        return *last_failure;
                    continue;
                }
                ++succeeded;
                context.emit(remote_tags_loaded { remote, parse_ls_remote_tags(outcome.standard_output) });
            }
            if (succeeded == 0 && last_failure.has_value())
                return *last_failure;
            return operation_outcome::succeeded();
        }

        operation_outcome refresh(git_operation_context& context)
        {
            const std::string git_dir { context.read_git_dir() };
            if (git_dir.empty())
                return operation_outcome::failed(failure_kind::not_found, u8"git 저장소가 아닙니다.");

            git_command_outcome outcome {};
            std::optional<operation_outcome> stop {};
            git_command_spec status_spec {};
            status_spec.arguments = make_git_status_args();
            if (context.run_required(std::move(status_spec), outcome, stop) == false)
                return *stop;
            git_status_result status { parse_git_status(outcome.standard_output) };

            // 줄 수는 있으면 좋은 값이라 실패해도 refresh를 멈추지 않는다.
            for (const diff_source source : { diff_source::unstaged, diff_source::staged })
            {
                git_command_spec numstat {};
                numstat.arguments = make_git_diff_numstat_args(source);
                const git_command_outcome counted { context.run(std::move(numstat)) };
                if (counted.accepted)
                    apply_numstat(source == diff_source::staged ? std::span<file_change> { status.staged } : std::span<file_change> { status.unstaged }, parse_git_numstat(counted.standard_output));
            }

            repository_snapshot snapshot {};
            snapshot.summary.path = to_bytes(context.job().repository_path);
            // 탭 이름은 폴더 이름이다. 끝 구분자는 경로 정규화와 같은 이유로 무시한다.
            std::string_view folder { snapshot.summary.path };
            while (folder.size() > 1 && (folder.back() == '/' || folder.back() == '\\'))
                folder.remove_suffix(1);
            snapshot.summary.display_name = std::string { folder.substr(folder.find_last_of("/\\") == std::string_view::npos ? 0 : folder.find_last_of("/\\") + 1) };
            apply_git_status(snapshot.summary, status);
            snapshot.summary.git_dir = git_dir;

            git_command_spec refs_spec {};
            refs_spec.arguments = make_git_for_each_ref_args();
            if (context.run_required(std::move(refs_spec), outcome, stop) == false)
                return *stop;
            snapshot.refs = parse_git_refs(outcome.standard_output).refs;

            git_command_spec stash_spec {};
            stash_spec.arguments = make_git_stash_list_args();
            if (context.run_required(std::move(stash_spec), outcome, stop) == false)
                return *stop;
            snapshot.stashes = parse_git_stash_list(outcome.standard_output).entries;

            git_command_spec remote_spec {};
            remote_spec.arguments = make_git_remote_list_args();
            if (context.run_required(std::move(remote_spec), outcome, stop) == false)
                return *stop;
            snapshot.summary.remotes = parse_git_remotes(outcome.standard_output);

            snapshot.summary.in_progress = context.read_in_progress(git_dir);
            snapshot.summary.operation_state = snapshot.summary.in_progress.kind;

            if (snapshot.summary.has_upstream && snapshot.summary.head_sha.empty() == false)
            {
                git_command_spec pushed {};
                pushed.arguments = make_git_is_ancestor_args("HEAD", snapshot.summary.upstream);
                pushed.accepted_exit_codes = { 0, 1 };
                const git_command_outcome answer { context.run(std::move(pushed)) };
                snapshot.summary.head_pushed = answer.accepted && answer.exit_code() == 0;
            }

            snapshot.staged = std::move(status.staged);
            snapshot.unstaged = std::move(status.unstaged);
            snapshot.conflicts = std::move(status.conflicts);
            read_extension_snapshot(context, snapshot);

            const bool has_head { snapshot.summary.head_sha.empty() == false };
            const bool has_refs { snapshot.refs.empty() == false };
            context.emit(snapshot_loaded { context.job().generation, std::move(snapshot) });

            // 첫 커밋 전이고 다른 ref도 없으면 읽을 이력이 없다. `log HEAD`는 실패하므로 부르지 않는다.
            if (has_head == false && has_refs == false)
            {
                commits_loaded empty {};
                empty.generation = context.job().generation;
                empty.page_complete = true;
                empty.history_complete = true;
                context.emit(std::move(empty));
                return operation_outcome::succeeded();
            }
            return stream_log(context, 0, has_head);
        }

        // 작업 트리 변경만 다시 읽는다. refresh의 status·줄 수 부분이고, 그래프·ref·stash·원격은 읽지 않는다.
        operation_outcome refresh_status(git_operation_context& context)
        {
            git_command_outcome outcome {};
            std::optional<operation_outcome> stop {};
            git_command_spec status_spec {};
            status_spec.arguments = make_git_status_args();
            if (context.run_required(std::move(status_spec), outcome, stop) == false)
                return *stop;
            git_status_result status { parse_git_status(outcome.standard_output) };

            for (const diff_source source : { diff_source::unstaged, diff_source::staged })
            {
                git_command_spec numstat {};
                numstat.arguments = make_git_diff_numstat_args(source);
                const git_command_outcome counted { context.run(std::move(numstat)) };
                if (counted.accepted)
                    apply_numstat(source == diff_source::staged ? std::span<file_change> { status.staged } : std::span<file_change> { status.unstaged }, parse_git_numstat(counted.standard_output));
            }

            status_loaded loaded {};
            apply_git_status(loaded.summary, status);
            loaded.staged = std::move(status.staged);
            loaded.unstaged = std::move(status.unstaged);
            loaded.conflicts = std::move(status.conflicts);
            context.emit(std::move(loaded));
            return operation_outcome::succeeded();
        }

        operation_outcome show_commit(git_operation_context& context, const show_commit_request& request)
        {
            git_command_outcome outcome {};
            std::optional<operation_outcome> stop {};
            git_command_spec info {};
            info.arguments = make_git_commit_info_args(request.sha);
            if (context.run_required(std::move(info), outcome, stop) == false)
                return *stop;
            std::vector<commit_info> commits { parse_git_log(outcome.standard_output, true) };
            if (commits.empty())
                return operation_outcome::failed(failure_kind::not_found, u8"커밋을 찾을 수 없습니다.");

            commit_detail detail {};
            detail.commit = std::move(commits.front());
            const bool is_merge { detail.commit.parents.size() > 1 };

            git_command_spec names {};
            names.arguments = make_git_commit_name_status_args(detail.commit.sha, is_merge);
            if (context.run_required(std::move(names), outcome, stop) == false)
                return *stop;
            detail.files = parse_git_name_status(outcome.standard_output);

            git_command_spec numstat {};
            numstat.arguments = make_git_show_numstat_args(detail.commit.sha, is_merge);
            if (context.run_required(std::move(numstat), outcome, stop) == false)
                return *stop;
            apply_numstat(detail.files, parse_git_numstat(outcome.standard_output));
            if (repository_uses_lfs(context))
                mark_lfs_changes(context, detail.files);

            context.emit(commit_detail_loaded { std::move(detail) });
            return operation_outcome::succeeded();
        }

        // `.gitattributes`가 이 파일에 textconv가 있는 diff 드라이버를 지정했으면 그 이름이다. 알아내지 못하면(git 실패 등) 빈 값이다.
        // 배지를 위한 부가 정보라 실패해도 diff를 막지 않는다.
        std::string detect_textconv_driver(git_operation_context& context, const std::string& path)
        {
            git_command_spec attribute {};
            attribute.arguments = make_git_check_attr_diff_args(path);
            const git_command_outcome attributed { context.run(std::move(attribute)) };
            if (attributed.accepted == false)
                return {};
            const std::optional<std::string> driver { parse_git_diff_driver(attributed.standard_output) };
            if (driver.has_value() == false || driver->find_first_of("\r\n") != std::string::npos)
                return {};

            git_command_spec config {};
            config.arguments = make_git_config_get_args("diff." + *driver + ".textconv");
            // 값이 없으면 종료 코드 1이다.
            config.accepted_exit_codes = { 0, 1 };
            const git_command_outcome configured { context.run(std::move(config)) };
            if (configured.accepted == false || configured.exit_code() != 0 || parse_git_config_value(configured.standard_output).has_value() == false)
                return {};
            return *driver;
        }

        operation_outcome load_file_diff(git_operation_context& context, const diff_file_request& request)
        {
            git_command_spec spec {};
            if (request.view == diff_view::commit)
            {
                git_command_outcome info {};
                std::optional<operation_outcome> stop {};
                git_command_spec parents {};
                parents.arguments = make_git_commit_info_args(request.sha);
                if (context.run_required(std::move(parents), info, stop) == false)
                    return *stop;
                const std::vector<commit_info> commits { parse_git_log(info.standard_output, true) };
                const bool is_merge { commits.empty() == false && commits.front().parents.size() > 1 };
                spec.arguments = make_git_show_file_args(request.sha, request.path, is_merge, request.context_lines);
            }
            else if (request.untracked)
            {
                spec.arguments = make_git_diff_untracked_args(request.path, request.context_lines);
                // 차이가 있으면 1이다.
                spec.accepted_exit_codes = { 0, 1 };
            }
            else
            {
                spec.arguments = make_git_diff_args(request.path, request.view == diff_view::staged ? diff_source::staged : diff_source::unstaged, request.context_lines);
            }

            git_command_outcome outcome {};
            std::optional<operation_outcome> stop {};
            if (context.run_required(std::move(spec), outcome, stop) == false)
                return *stop;
            file_diff diff {};
            diff.request = request;
            diff.files = parse_git_diff(outcome.standard_output);
            annotate_lfs_diff(context, request, diff.files);
            // 추적하지 않는 파일은 `--no-index`라 속성을 따르지 않는다.
            if (request.untracked == false)
                diff.textconv_driver = detect_textconv_driver(context, request.path);
            context.emit(file_diff_loaded { std::move(diff) });
            return operation_outcome::succeeded();
        }

        operation_outcome is_ancestor(git_operation_context& context, const is_ancestor_request& request)
        {
            git_command_spec spec {};
            spec.arguments = make_git_is_ancestor_args(request.ancestor, request.descendant);
            spec.accepted_exit_codes = { 0, 1 };
            git_command_outcome outcome {};
            std::optional<operation_outcome> stop {};
            if (context.run_required(std::move(spec), outcome, stop) == false)
                return *stop;
            context.emit(ancestry_answered { request.ancestor, request.descendant, outcome.exit_code() == 0 });
            return operation_outcome::succeeded();
        }

        operation_outcome count_commits(git_operation_context& context, const count_commits_request& request)
        {
            git_command_spec spec {};
            spec.arguments = make_git_count_commits_args(request.from, request.to);
            git_command_outcome outcome {};
            std::optional<operation_outcome> stop {};
            if (context.run_required(std::move(spec), outcome, stop) == false)
                return *stop;
            const std::optional<std::uint32_t> count { git_text::parse_integer<std::uint32_t>(trim_line(outcome.standard_output)) };
            if (count.has_value() == false)
                return operation_outcome::failed(failure_kind::other, u8"커밋 수를 읽지 못했습니다.", to_utf8(outcome.standard_output));
            context.emit(commit_count_answered { request.from, request.to, *count });
            return operation_outcome::succeeded();
        }
    } // namespace

    operation_outcome run_read_operation(git_operation_context& context)
    {
        const git_operation_job& job { context.job() };
        return std::visit(
            [&context, &job](const auto& request) -> operation_outcome {
                using request_type = std::decay_t<decltype(request)>;
                if constexpr (std::is_same_v<request_type, refresh_request>)
                    return refresh(context);
                else if constexpr (std::is_same_v<request_type, refresh_status_request>)
                    return refresh_status(context);
                else if constexpr (std::is_same_v<request_type, load_more_log_request>)
                    return stream_log(context, job.log_skip, job.log_include_head);
                else if constexpr (std::is_same_v<request_type, show_commit_request>)
                    return show_commit(context, request);
                else if constexpr (std::is_same_v<request_type, diff_file_request>)
                    return load_file_diff(context, request);
                else if constexpr (std::is_same_v<request_type, is_ancestor_request>)
                    return is_ancestor(context, request);
                else if constexpr (std::is_same_v<request_type, count_commits_request>)
                    return count_commits(context, request);
                else if constexpr (std::is_same_v<request_type, refresh_remote_tags_request>)
                    return refresh_remote_tags(context, request.remotes);
                else if constexpr (std::is_same_v<request_type, load_rebase_plan_request>)
                    return run_load_rebase_plan(context, request);
                else
                    return operation_outcome::failed(failure_kind::invalid_request, u8"읽기 lane에서 실행할 수 없는 작업입니다.");
            },
            job.operation);
    }

    operation_outcome run_setup_operation(git_operation_context& context)
    {
        const git_operation_job& job { context.job() };
        return std::visit(
            [&context](const auto& request) -> operation_outcome {
                using request_type = std::decay_t<decltype(request)>;
                if constexpr (std::is_same_v<request_type, inspect_path_request>)
                {
                    path_inspected result {};
                    result.path = request.path;
                    if (is_absolute_windows_path(request.path) == false)
                        return operation_outcome::failed(failure_kind::invalid_request, u8"절대 경로가 필요합니다.");
                    if (context.owner().files().probe(request.path) != path_kind::directory)
                    {
                        context.emit(std::move(result));
                        return operation_outcome::succeeded();
                    }
                    git_command_spec spec {};
                    spec.arguments = make_git_rev_parse_args();
                    spec.working_directory = request.path;
                    const git_command_outcome outcome { context.run(std::move(spec)) };
                    const std::vector<std::string> lines { split_lines(outcome.standard_output) };
                    result.kind = path_inspection_kind::directory;
                    if (outcome.accepted && lines.size() >= 2 && lines[1].empty() == false)
                    {
                        result.root = to_utf8(lines[1]);
                        result.kind = normalize_path_key(result.root) == normalize_path_key(request.path) ? path_inspection_kind::repository : path_inspection_kind::inside_repository;
                    }
                    context.emit(std::move(result));
                    return operation_outcome::succeeded();
                }
                else if constexpr (std::is_same_v<request_type, clone_request>)
                {
                    if (is_absolute_windows_path(request.target_path) == false)
                        return operation_outcome::failed(failure_kind::invalid_request, u8"절대 경로가 필요합니다.");
                    if (context.owner().files().probe(request.target_path) == path_kind::file)
                        return operation_outcome::failed(failure_kind::refused, u8"같은 이름의 파일이 있습니다.");
                    git_clone_options options {};
                    options.url = request.url;
                    options.directory = to_bytes(request.target_path);
                    git_command_spec spec {};
                    spec.arguments = make_git_clone_args(options);
                    spec.command_class = git_command_class::network;
                    // 대상 폴더는 git이 만든다. 실행 위치는 이미 있는 부모 폴더여야 한다.
                    spec.working_directory = parent_directory(request.target_path);
                    git_command_outcome outcome {};
                    std::optional<operation_outcome> stop {};
                    if (context.run_required(std::move(spec), outcome, stop) == false)
                        return *stop;
                    return operation_outcome::succeeded(u8"저장소를 복제했습니다.");
                }
                else if constexpr (std::is_same_v<request_type, init_request>)
                {
                    if (is_absolute_windows_path(request.target_path) == false)
                        return operation_outcome::failed(failure_kind::invalid_request, u8"절대 경로가 필요합니다.");
                    git_command_spec spec {};
                    spec.arguments = make_git_init_args(to_bytes(request.target_path), request.initial_branch);
                    spec.command_class = git_command_class::local_write;
                    spec.working_directory = parent_directory(request.target_path);
                    git_command_outcome outcome {};
                    std::optional<operation_outcome> stop {};
                    if (context.run_required(std::move(spec), outcome, stop) == false)
                        return *stop;
                    return operation_outcome::succeeded(u8"저장소를 만들었습니다.");
                }
                else if constexpr (std::is_same_v<request_type, ssh_keygen_request>)
                {
                    return run_ssh_keygen(context, request);
                }
                else if constexpr (std::is_same_v<request_type, ssh_connection_test_request>)
                {
                    return run_ssh_connection_test(context, request);
                }
                else
                {
                    return operation_outcome::failed(failure_kind::invalid_request, u8"저장소 준비 작업이 아닙니다.");
                }
            },
            job.operation);
    }
} // namespace cyella
