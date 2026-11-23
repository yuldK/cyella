#include "infrastructure/git_operation_context.h"

#include "domain/path_syntax.h"
#include "domain/text_catalog.h"
#include "infrastructure/git_blame_parser.h"
#include "infrastructure/git_diff_parser.h"
#include "infrastructure/git_extension_operations.h"
#include "infrastructure/git_file_log_parser.h"
#include "infrastructure/git_interactive_rebase.h"
#include "infrastructure/git_log_parser.h"
#include "infrastructure/git_query_parser.h"
#include "infrastructure/git_ref_parser.h"
#include "infrastructure/git_signing_operations.h"
#include "infrastructure/git_stash_parser.h"
#include "infrastructure/git_text.h"

#include <algorithm>
#include <chrono>
#include <limits>
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
            const std::size_t page_size { std::max(options.log_page_size, context.job().minimum_log_count) };
            log_options.max_count = static_cast<std::uint32_t>(page_size);
            log_options.skip = static_cast<std::uint32_t>(skip);
            log_options.include_head = include_head;

            git_command_spec spec {};
            spec.arguments = make_git_log_args(log_options);
            spec.on_standard_output = [&parser](const std::string_view bytes) { parser.feed(bytes); };
            const git_command_outcome outcome { context.run(std::move(spec)) };
            if (outcome.accepted == false)
                return context.failure_from(outcome);

            // 성공한 출력만 꼬리를 읽는다. 취소·시간 초과로 끊긴 출력의 꼬리는 잘린 레코드라 커밋으로 내면 안 된다.
            parser.finish();
            // 버려진 레코드도 git이 돌려준 한 건이다. 빼면 쪽이 찼는데도 이력이 끝난 것으로 판정해 더 불러오기가 멈춘다.
            batcher.flush(true, batcher.count() + parser.malformed_record_count() < page_size);
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
                return operation_outcome::failed(failure_kind::not_found, tr(u8"git.not_a_repository"));

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
            read_signing_snapshot(context, snapshot);

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
            // 전체 refresh(`read_extension_snapshot`)와 같이 LFS 표시를 붙인다. 빠뜨리면 파일 감시가 낸 가벼운
            // 새로 고침 뒤에 작업 영역의 LFS 배지가 사라진다.
            if (repository_uses_lfs(context))
            {
                mark_lfs_changes(context, status.staged);
                mark_lfs_changes(context, status.unstaged);
                mark_lfs_changes(context, status.conflicts);
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
                return operation_outcome::failed(failure_kind::not_found, tr(u8"git.commit_not_found"));

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

        // 비교의 왼쪽 리비전이다. 비었으면(루트 커밋부터의 범위) 이 저장소 객체 형식의 빈 트리 해시를 git에게 묻는다.
        // sha1과 sha256 저장소의 빈 트리 해시가 달라 상수로 둘 수 없다.
        bool resolve_base_revision(git_operation_context& context, const std::string& base, std::string& resolved, std::optional<operation_outcome>& stop)
        {
            if (base.empty() == false)
            {
                resolved = base;
                return true;
            }
            git_command_spec spec {};
            spec.arguments = make_git_empty_tree_args();
            git_command_outcome outcome {};
            if (context.run_required(std::move(spec), outcome, stop) == false)
                return false;
            resolved = trim_line(outcome.standard_output);
            if (resolved.empty())
            {
                stop = operation_outcome::failed(failure_kind::other, tr(u8"git.empty_tree_failed"), to_utf8(outcome.standard_output));
                return false;
            }
            return true;
        }

        operation_outcome show_range(git_operation_context& context, const show_range_request& request)
        {
            git_command_outcome outcome {};
            std::optional<operation_outcome> stop {};
            range_detail detail {};
            detail.request = request;
            if (resolve_base_revision(context, request.base, detail.base, stop) == false)
                return *stop;

            git_command_spec names {};
            names.arguments = make_git_diff_revisions_name_status_args(detail.base, request.tip);
            if (context.run_required(std::move(names), outcome, stop) == false)
                return *stop;
            detail.files = parse_git_name_status(outcome.standard_output);

            git_command_spec numstat {};
            numstat.arguments = make_git_diff_revisions_numstat_args(detail.base, request.tip);
            if (context.run_required(std::move(numstat), outcome, stop) == false)
                return *stop;
            apply_numstat(detail.files, parse_git_numstat(outcome.standard_output));
            if (repository_uses_lfs(context))
                mark_lfs_changes(context, detail.files);

            if (request.range)
            {
                git_command_spec count {};
                count.arguments = request.base.empty() ? make_git_count_reachable_args(request.tip) : make_git_count_commits_args(request.base, request.tip);
                if (context.run_required(std::move(count), outcome, stop) == false)
                    return *stop;
                detail.commit_count = git_text::parse_integer<std::uint32_t>(trim_line(outcome.standard_output));
            }
            context.emit(range_detail_loaded { std::move(detail) });
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
            else if (request.view == diff_view::range)
            {
                std::string base {};
                std::optional<operation_outcome> stop {};
                if (resolve_base_revision(context, request.base, base, stop) == false)
                    return *stop;
                spec.arguments = make_git_diff_revisions_file_args(base, request.sha, request.path, request.context_lines);
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
            // 상한을 넘은 큰 diff도 예전처럼 잘린 채 보이고, 줄 수 상한 안내가 붙는다.
            spec.fail_on_truncated_output = false;

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

        operation_outcome load_file_blame(git_operation_context& context, const blame_file_request& request)
        {
            // 큰 파일의 blame 출력은 수십 MiB다. 캡처 상한을 타지 않게 조각을 받는 대로 해석한다.
            git_blame_parser parser {};
            git_command_spec spec {};
            spec.arguments = make_git_blame_args(request.path, request.revision);
            spec.on_standard_output = [&parser](const std::string_view bytes) { parser.feed(bytes); };
            const git_command_outcome outcome { context.run(std::move(spec)) };
            // 없는 경로, 커밋 없는 저장소, submodule 경로는 git이 실패로 알린다.
            if (outcome.accepted == false)
                return context.failure_from(outcome);

            // 성공한 출력만 끝맺는다. 취소·시간 초과로 끊긴 출력은 앞부분만 있는 blame이라 내면 안 된다.
            git_blame_result result { parser.finish() };
            // 줄을 하나라도 버렸으면 줄 번호가 파일과 어긋난다. 틀린 주인을 보이느니 실패로 알린다.
            if (result.malformed_line_count != 0)
                return operation_outcome::failed(failure_kind::other, tr(u8"git.blame_parse_failed"));

            file_blame blame {};
            blame.request = request;
            blame.origins = std::move(result.origins);
            blame.lines = std::move(result.lines);
            blame.binary = result.binary;
            context.emit(file_blame_loaded { std::move(blame) });
            return operation_outcome::succeeded();
        }

        operation_outcome load_file_history(git_operation_context& context, const file_history_request& request)
        {
            file_history history {};
            history.request = request;
            const auto collect = [&history](git_file_log_entry&& entry) {
                history.entries.push_back(file_history_entry { std::move(entry.commit), std::move(entry.path), std::move(entry.previous_path), entry.status });
            };
            git_file_log_parser parser { collect, request.path };

            // 더 있는지 알려고 하나를 더 청한다. 0은 제한 없음이고, 최댓값은 더할 자리가 없어 그대로 둔다.
            const std::uint32_t limit { request.max_count };
            const std::uint32_t asked { limit == 0 || limit == std::numeric_limits<std::uint32_t>::max() ? limit : limit + 1 };
            git_command_spec spec {};
            spec.arguments = make_git_file_log_args(request.path, request.revision, asked);
            spec.on_standard_output = [&parser](const std::string_view bytes) { parser.feed(bytes); };
            const git_command_outcome outcome { context.run(std::move(spec)) };
            if (outcome.accepted == false)
                return context.failure_from(outcome);

            parser.finish();
            // 버려진 레코드도 git이 돌려준 한 건이다. 빼고 세면 더 있는데도 끝난 것으로 보인다.
            if (limit != 0 && history.entries.size() + parser.malformed_record_count() > limit)
            {
                history.truncated = true;
                if (history.entries.size() > limit)
                    history.entries.resize(limit);
            }
            context.emit(file_history_loaded { std::move(history) });
            return operation_outcome::succeeded();
        }

        std::string_view trim_blank(const std::string_view text) noexcept
        {
            constexpr std::string_view blank { " \t\r\n" };
            const std::size_t first { text.find_first_not_of(blank) };
            if (first == std::string_view::npos)
                return {};
            return text.substr(first, text.find_last_not_of(blank) - first + 1);
        }

        // `prefix`가 `text`의 앞부분인지를 ASCII 대소문자를 가리지 않고 본다. git은 SHA를 소문자로 내고 질의는 어느 쪽이든 된다.
        bool starts_with_hex(const std::string_view text, const std::string_view prefix) noexcept
        {
            const auto lower = [](const char value) { return value >= 'A' && value <= 'F' ? static_cast<char>(value - 'A' + 'a') : value; };
            return text.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), text.begin(), [&lower](const char left, const char right) { return lower(left) == lower(right); });
        }

        // 한 필드(메시지 또는 작성자)에서 질의가 든 커밋을 `found`에 더한다. git이 실패하면 끝맺음을 `stop`에 넣고 false다.
        // 상한보다 많이 맞았으면 `overflowed`를 세운다.
        bool collect_search_matches(git_operation_context& context, const std::string_view query, const commit_search_field field, const std::uint32_t limit, std::vector<commit_info>& found
            , bool& overflowed, std::optional<operation_outcome>& stop)
        {
            git_log_parser parser { [&found](commit_info&& commit) { found.push_back(std::move(commit)); } };
            // 더 있는지 알려고 하나를 더 청한다. 0은 제한 없음이고, 최댓값은 더할 자리가 없어 그대로 둔다.
            const std::uint32_t asked { limit == 0 || limit == std::numeric_limits<std::uint32_t>::max() ? limit : limit + 1 };
            // 맞는 커밋이 많으면 출력이 캡처 상한을 넘을 수 있어 조각을 받는 대로 해석한다.
            git_command_spec spec {};
            spec.arguments = make_git_search_log_args(query, field, context.job().log_include_head, asked);
            spec.on_standard_output = [&parser](const std::string_view bytes) { parser.feed(bytes); };
            const git_command_outcome outcome { context.run(std::move(spec)) };
            if (outcome.accepted == false)
            {
                stop = context.failure_from(outcome);
                return false;
            }

            // 성공한 출력만 꼬리를 읽는다. 취소로 끊긴 출력의 꼬리는 잘린 레코드다.
            parser.finish();
            // 버려진 레코드도 git이 돌려준 한 건이다. 빼고 세면 더 있는데도 다 찾은 것으로 보인다.
            if (limit != 0 && parser.commit_count() + parser.malformed_record_count() > limit)
                overflowed = true;
            return true;
        }

        // 질의가 SHA 앞부분이면 그 커밋을 `found`에 더한다. 그래프의 ref에서 닿지 않는 커밋은 화면이 그 행으로 갈 수 없어 넣지 않는다.
        // 그런 커밋이 없는 것은 실패가 아니다. git이 실패하면 끝맺음을 `stop`에 넣고 false다.
        bool collect_hash_match(git_operation_context& context, const std::string_view query, std::vector<commit_info>& found, std::optional<operation_outcome>& stop)
        {
            git_command_outcome outcome {};
            git_command_spec resolve {};
            resolve.arguments = make_git_resolve_commit_args(query);
            // 없거나 여러 커밋에 맞는 앞부분은 1이다.
            resolve.accepted_exit_codes = { 0, 1 };
            if (context.run_required(std::move(resolve), outcome, stop) == false)
                return false;
            const std::string sha { trim_line(outcome.standard_output) };
            // 같은 글자의 ref가 있으면 (`cafe`라는 브랜치) git은 ref를 먼저 풀어 다른 커밋을 준다. SHA가 맞은 것이 아니므로 버린다.
            if (outcome.exit_code() != 0 || is_commit_hash_prefix(sha) == false || starts_with_hex(sha, query) == false)
                return true;
            // 메시지나 작성자로 이미 찾은 커밋이다. 그래프의 ref에서 찾았으니 닿는지 물을 것도 없다.
            if (std::ranges::any_of(found, [&sha](const commit_info& commit) { return commit.sha == sha; }))
                return true;

            git_command_spec reachability {};
            reachability.arguments = make_git_graph_reachability_args(sha, context.job().log_include_head);
            if (context.run_required(std::move(reachability), outcome, stop) == false)
                return false;
            // 닿지 않으면 git이 그 SHA를 낸다.
            if (trim_line(outcome.standard_output).empty() == false)
                return true;

            git_command_spec info {};
            info.arguments = make_git_commit_info_args(sha);
            if (context.run_required(std::move(info), outcome, stop) == false)
                return false;
            std::vector<commit_info> commits { parse_git_log(outcome.standard_output, true) };
            if (commits.empty())
                return true;
            // 검색 결과는 본문을 싣지 않는다. 이 커밋만 본문을 달고 가면 같은 커밋이 찾은 길에 따라 다른 값이 된다.
            commits.front().body.reset();
            found.push_back(std::move(commits.front()));
            return true;
        }

        operation_outcome load_commit_search(git_operation_context& context, const search_commits_request& request)
        {
            const std::string_view query { trim_blank(request.query) };
            if (query.empty())
                return operation_outcome::failed(failure_kind::invalid_request, tr(u8"git.search_query_empty"));

            commit_search search {};
            // 요청은 받은 그대로 돌려준다. 화면이 자기가 낸 요청과 `==`로 견준다.
            search.request = request;
            const std::uint32_t limit { request.max_count };
            std::optional<operation_outcome> stop {};
            // `--grep`과 `--author`를 한 명령에 주면 둘 다 맞는 커밋만 나온다. "메시지 또는 작성자"라서 따로 돌려 합친다.
            for (const commit_search_field field : { commit_search_field::message, commit_search_field::author })
            {
                if (collect_search_matches(context, query, field, limit, search.commits, search.truncated, stop) == false)
                    return *stop;
            }
            if (is_commit_hash_prefix(query) && collect_hash_match(context, query, search.commits, stop) == false)
                return *stop;

            // 두 검색의 순서(`--topo-order`)는 합치면 뜻이 없다. 새 커밋이 앞에 오게 하고, 같은 시각은 SHA로 순서를 고정한다.
            // 같은 커밋은 시각도 같아 정렬하면 이웃한다.
            std::ranges::sort(search.commits, [](const commit_info& left, const commit_info& right) {
                if (left.committer_time != right.committer_time)
                    return left.committer_time > right.committer_time;
                return left.sha < right.sha;
            });
            const auto duplicates { std::ranges::unique(search.commits, {}, &commit_info::sha) };
            search.commits.erase(duplicates.begin(), duplicates.end());
            if (limit != 0 && search.commits.size() > limit)
            {
                search.truncated = true;
                search.commits.resize(limit);
            }
            context.emit(commit_search_loaded { std::move(search) });
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
                return operation_outcome::failed(failure_kind::other, tr(u8"git.commit_count_failed"), to_utf8(outcome.standard_output));
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
                else if constexpr (std::is_same_v<request_type, show_range_request>)
                    return show_range(context, request);
                else if constexpr (std::is_same_v<request_type, is_ancestor_request>)
                    return is_ancestor(context, request);
                else if constexpr (std::is_same_v<request_type, count_commits_request>)
                    return count_commits(context, request);
                else if constexpr (std::is_same_v<request_type, refresh_remote_tags_request>)
                    return refresh_remote_tags(context, request.remotes);
                else if constexpr (std::is_same_v<request_type, load_rebase_plan_request>)
                    return run_load_rebase_plan(context, request);
                else if constexpr (std::is_same_v<request_type, blame_file_request>)
                    return load_file_blame(context, request);
                else if constexpr (std::is_same_v<request_type, file_history_request>)
                    return load_file_history(context, request);
                else if constexpr (std::is_same_v<request_type, search_commits_request>)
                    return load_commit_search(context, request);
                else
                    return operation_outcome::failed(failure_kind::invalid_request, tr(u8"git.read_lane_unsupported"));
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
                        return operation_outcome::failed(failure_kind::invalid_request, tr(u8"git.absolute_path_required"));
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
                        return operation_outcome::failed(failure_kind::invalid_request, tr(u8"git.absolute_path_required"));
                    if (context.owner().files().probe(request.target_path) == path_kind::file)
                        return operation_outcome::failed(failure_kind::refused, tr(u8"git.file_exists"));
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
                    return operation_outcome::succeeded(tr(u8"git.cloned"));
                }
                else if constexpr (std::is_same_v<request_type, init_request>)
                {
                    if (is_absolute_windows_path(request.target_path) == false)
                        return operation_outcome::failed(failure_kind::invalid_request, tr(u8"git.absolute_path_required"));
                    git_command_spec spec {};
                    spec.arguments = make_git_init_args(to_bytes(request.target_path), request.initial_branch);
                    spec.command_class = git_command_class::local_write;
                    spec.working_directory = parent_directory(request.target_path);
                    git_command_outcome outcome {};
                    std::optional<operation_outcome> stop {};
                    if (context.run_required(std::move(spec), outcome, stop) == false)
                        return *stop;
                    return operation_outcome::succeeded(tr(u8"git.initialized"));
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
                    return operation_outcome::failed(failure_kind::invalid_request, tr(u8"git.not_setup_operation"));
                }
            },
            job.operation);
    }
} // namespace cyella
