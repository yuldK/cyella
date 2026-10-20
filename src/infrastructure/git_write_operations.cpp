#include "infrastructure/git_operation_context.h"

#include "domain/path_syntax.h"
#include "infrastructure/git_interactive_rebase.h"
#include "infrastructure/git_query_parser.h"

#include <type_traits>
#include <utility>

namespace cyella {
    namespace {
        // 실행 하나로 끝나는 쓰기다. 실패는 분류해 돌려준다.
        operation_outcome run_single(git_operation_context& context, git_arguments arguments, const git_command_class command_class, std::u8string summary, std::string standard_input = {})
        {
            git_command_spec spec {};
            spec.arguments = std::move(arguments);
            spec.command_class = command_class;
            spec.standard_input = std::move(standard_input);
            git_command_outcome outcome {};
            std::optional<operation_outcome> stop {};
            if (context.run_required(std::move(spec), outcome, stop) == false)
                return *stop;
            return operation_outcome::succeeded(std::move(summary));
        }

        // 충돌로 멈출 수 있는 명령이다. 충돌이면 needs_input{conflict}로 끝난다.
        operation_outcome run_sequence(git_operation_context& context, git_arguments arguments, const git_command_class command_class, std::u8string summary)
        {
            git_command_spec spec {};
            spec.arguments = std::move(arguments);
            spec.command_class = command_class;
            const git_command_outcome outcome { context.run(std::move(spec)) };
            return context.finish_sequence(outcome, std::move(summary));
        }

        std::string with_trailing_newline(std::string text)
        {
            if (text.empty() == false && text.back() != '\n')
                text.push_back('\n');
            return text;
        }

        std::u8string name_of(const std::string_view value)
        {
            return to_utf8(value);
        }

        git_switch_mode switch_mode_of(const checkout_policy policy) noexcept
        {
            switch (policy)
            {
            case checkout_policy::discard:
                return git_switch_mode::discard_changes;
            case checkout_policy::carry:
                return git_switch_mode::merge;
            case checkout_policy::ask:
            case checkout_policy::stash_and_reapply:
                break;
            }
            return git_switch_mode::normal;
        }

        // 덮어써질 변경 때문에 막힌 체크아웃을 사용자의 결정으로 돌린다.
        operation_outcome dirty_worktree_input(const git_command_outcome& outcome, const std::string& target)
        {
            const overwritten_files blocked { parse_overwritten_files(outcome.standard_error_bytes()) };
            input_request request {};
            request.kind = input_kind::dirty_worktree;
            request.files = blocked.files;
            request.untracked_files = blocked.untracked;
            request.target = target;
            return operation_outcome::needs(std::move(request));
        }

        // 체크아웃 정책의 공통 흐름이다. `make_switch`는 모드를 받아 switch 명령줄을 만든다.
        template<typename switch_builder>
        operation_outcome checkout_with_policy(git_operation_context& context, const checkout_policy policy, const std::string& target, const switch_builder& make_switch, std::u8string summary)
        {
            if (policy != checkout_policy::stash_and_reapply)
            {
                git_command_spec spec {};
                spec.arguments = make_switch(switch_mode_of(policy));
                spec.command_class = git_command_class::local_write;
                const git_command_outcome outcome { context.run(std::move(spec)) };
                if (outcome.accepted)
                    return operation_outcome::succeeded(std::move(summary));
                operation_outcome failure { context.failure_from(outcome) };
                // 처음 시도에서 막혔을 때만 묻는다. 정책을 고른 뒤에도 막히면(추적하지 않는 파일)
                // 같은 질문을 되풀이하지 않고 실패로 알린다.
                if (policy == checkout_policy::ask && failure.failure.has_value() && failure.failure->kind == failure_kind::dirty_worktree)
                    return dirty_worktree_input(outcome, target);
                return failure;
            }

            git_command_outcome outcome {};
            std::optional<operation_outcome> stop {};
            git_stash_push_options stash_options {};
            stash_options.include_untracked = true;
            stash_options.message = "cyella: " + target + " 체크아웃 전에 보관";
            git_command_spec stash {};
            stash.arguments = make_git_stash_push_args(stash_options);
            stash.command_class = git_command_class::local_write;
            if (context.run_required(std::move(stash), outcome, stop) == false)
                return *stop;
            // 바꿀 것이 없으면 stash가 생기지 않는다. 그때 pop하면 남의 stash를 꺼낸다.
            const bool stashed { outcome.standard_output.find("No local changes to save") == std::string::npos };

            git_command_spec move {};
            move.arguments = make_switch(git_switch_mode::normal);
            move.command_class = git_command_class::local_write;
            const git_command_outcome moved { context.run(std::move(move)) };
            if (moved.accepted == false)
            {
                operation_outcome failure { context.failure_from(moved) };
                // 옮기지 못했으면 원래 자리에서 변경을 되돌려 놓는다.
                if (stashed)
                {
                    git_command_spec restore {};
                    restore.arguments = make_git_stash_pop_args(0);
                    restore.command_class = git_command_class::local_write;
                    static_cast<void>(context.run(std::move(restore)));
                }
                return failure;
            }
            if (stashed == false)
                return operation_outcome::succeeded(std::move(summary));

            git_command_spec pop {};
            pop.arguments = make_git_stash_pop_args(0);
            pop.command_class = git_command_class::local_write;
            const git_command_outcome popped { context.run(std::move(pop)) };
            if (popped.accepted)
                return operation_outcome::succeeded(std::move(summary));
            if (popped.process.completion != process_completion::exited)
                return context.failure_from(popped);
            // 충돌이 나도 git은 stash를 지우지 않는다. 체크아웃은 이미 됐으므로 그 사실과 함께 알린다.
            operation_outcome failure { context.failure_from(popped) };
            if (failure.failure.has_value())
                failure.failure->summary = name_of(target) + u8"로 옮겼지만 보관한 변경을 다시 꺼내다 충돌했습니다. stash@{0}을 남겼습니다.";
            return failure;
        }

        operation_outcome checkout_branch(git_operation_context& context, const checkout_branch_request& request)
        {
            return checkout_with_policy(
                context, request.policy, request.name, [&request](const git_switch_mode mode) { return make_git_switch_args(request.name, mode); }, name_of(request.name) + u8"로 체크아웃했습니다.");
        }

        operation_outcome checkout_remote_branch(git_operation_context& context, const checkout_remote_branch_request& request)
        {
            const std::size_t slash { request.remote_branch.find('/') };
            if (slash == std::string::npos || slash + 1 >= request.remote_branch.size())
                return operation_outcome::failed(failure_kind::invalid_request, u8"원격 브랜치 이름은 `<remote>/<branch>` 모양이어야 합니다.");
            const std::string local { request.remote_branch.substr(slash + 1) };

            git_command_spec exists {};
            exists.arguments = make_git_ref_exists_args("refs/heads/" + local);
            exists.accepted_exit_codes = { 0, 1 };
            const git_command_outcome found { context.run(std::move(exists)) };
            if (found.accepted == false)
                return context.failure_from(found);

            if (found.exit_code() == 0)
                return checkout_with_policy(
                    context, request.policy, local, [&local](const git_switch_mode mode) { return make_git_switch_args(local, mode); }, name_of(local) + u8"로 체크아웃했습니다.");
            return checkout_with_policy(
                context, request.policy, local, [&local, &request](const git_switch_mode mode) { return make_git_switch_create_args(local, request.remote_branch, true, mode); },
                name_of(local) + u8" 추적 브랜치를 만들고 체크아웃했습니다.");
        }

        operation_outcome checkout_detached(git_operation_context& context, const checkout_detached_request& request)
        {
            return checkout_with_policy(
                context, request.policy, request.revision, [&request](const git_switch_mode mode) { return make_git_switch_detach_args(request.revision, mode); },
                u8"커밋을 체크아웃했습니다(detached HEAD).");
        }

        operation_outcome commit(git_operation_context& context, const commit_request& request)
        {
            if (request.stage_all)
            {
                git_command_spec add {};
                add.arguments = make_git_add_all_args();
                add.command_class = git_command_class::local_write;
                git_command_outcome outcome {};
                std::optional<operation_outcome> stop {};
                if (context.run_required(std::move(add), outcome, stop) == false)
                    return *stop;
            }
            git_commit_options options {};
            options.amend = request.amend;
            // amend에서 메시지를 비우면 기존 메시지를 쓴다. 새 커밋의 빈 메시지는 git이 거절한다.
            options.keep_message = request.amend && request.message.empty();
            options.sign = request.sign;
            const std::string message { options.keep_message ? std::string {} : with_trailing_newline(request.message) };
            // 서명하면 gpg의 pinentry나 ssh 키 passphrase 대화 상자를 사용자가 열 때까지 기다린다. 그동안 시간 제한(300초)으로 끊지 않고 취소만 받는다.
            const git_command_class command_class { request.sign == true ? git_command_class::network : git_command_class::local_write };
            return run_single(context, make_git_commit_args(options), command_class, request.amend ? u8"직전 커밋을 고쳤습니다." : u8"커밋했습니다.", message);
        }

        // 추적하지 않는 파일을 휴지통으로 보낸다. git을 실행하지 않는 파일 연산이다.
        operation_outcome trash_files(git_operation_context& context, const trash_files_request& request)
        {
            if (request.paths.empty())
                return operation_outcome::failed(failure_kind::invalid_request, u8"휴지통으로 보낼 파일이 없습니다.");
            std::vector<std::u8string> targets {};
            targets.reserve(request.paths.size());
            for (const std::string& path : request.paths)
            {
                const std::optional<std::u8string> absolute { resolve_workspace_child(context.job().repository_path, path) };
                if (absolute.has_value() == false)
                    return operation_outcome::failed(failure_kind::invalid_request, u8"저장소 안의 파일 경로가 아닙니다: " + to_utf8(path));
                targets.push_back(*absolute);
            }
            // 이미 없는 파일은 보낼 것이 없다. 일부만 없을 때는 남은 것만 보낸다.
            workspace_files& files { context.owner().files() };
            std::erase_if(targets, [&files](const std::u8string& path) { return files.probe(path) == path_kind::missing; });
            if (targets.empty())
                return operation_outcome::succeeded(u8"이미 없는 파일입니다.");
            if (files.move_to_recycle_bin(targets) == false)
                return operation_outcome::failed(failure_kind::other, u8"휴지통으로 보내지 못했습니다.");
            return operation_outcome::succeeded(u8"휴지통으로 보냈습니다.");
        }

        operation_outcome ignore_path(git_operation_context& context, const ignore_path_request& request)
        {
            if (request.pattern.empty() || request.pattern.find_first_of("\r\n") != std::string::npos)
                return operation_outcome::failed(failure_kind::invalid_request, u8"한 줄짜리 패턴이 필요합니다.");
            const std::u8string path { join_path(context.job().repository_path, ".gitignore") };
            workspace_files& files { context.owner().files() };
            std::string line {};
            // 마지막 줄이 줄바꿈 없이 끝났으면 이어 붙지 않게 먼저 줄을 바꾼다.
            if (const auto existing { files.read_text(path, 16u * 1024u * 1024u) }; existing.has_value() && existing->empty() == false && existing->back() != '\n')
                line.push_back('\n');
            line.append(request.pattern).push_back('\n');
            if (files.append_text(path, line) == false)
                return operation_outcome::failed(failure_kind::other, u8".gitignore에 쓰지 못했습니다.");
            return operation_outcome::succeeded(u8".gitignore에 더했습니다.");
        }

        operation_outcome push(git_operation_context& context, const push_execute_request& request)
        {
            git_push_options options {};
            options.remote = request.remote;
            options.set_upstream = request.set_upstream;
            options.follow_tags = request.follow_tags;
            options.force_with_lease = request.force_with_lease;
            if (request.branch.empty() == false)
            {
                if (request.remote.empty())
                    return operation_outcome::failed(failure_kind::invalid_request, u8"브랜치를 고르면 원격도 골라야 합니다.");
                const std::string& remote_branch { request.remote_branch.empty() ? request.branch : request.remote_branch };
                options.refspec = "refs/heads/" + request.branch + ":refs/heads/" + remote_branch;
            }
            return run_single(context, make_git_push_args(options), git_command_class::network, u8"push했습니다.");
        }

        operation_outcome merge(git_operation_context& context, const merge_request& request)
        {
            if (request.into.empty() == false && request.into != context.read_current_branch())
            {
                git_command_spec move {};
                move.arguments = make_git_switch_args(request.into);
                move.command_class = git_command_class::local_write;
                git_command_outcome outcome {};
                std::optional<operation_outcome> stop {};
                if (context.run_required(std::move(move), outcome, stop) == false)
                    return *stop;
            }
            return run_sequence(context, make_git_merge_args(request.from), git_command_class::local_write, name_of(request.from) + u8"를 병합했습니다.");
        }

        operation_outcome fast_forward(git_operation_context& context, const fast_forward_request& request)
        {
            if (request.target == context.read_current_branch())
            {
                git_command_spec spec {};
                spec.arguments = make_git_merge_args(request.to, git_merge_mode::fast_forward_only);
                spec.command_class = git_command_class::local_write;
                const git_command_outcome outcome { context.run(std::move(spec)) };
                if (outcome.accepted)
                    return operation_outcome::succeeded(name_of(request.target) + u8"를 fast-forward했습니다.");
                return context.failure_from(outcome);
            }

            // `fetch .`의 refspec은 전체 ref 이름이어야 모호하지 않다. SHA면 그대로 쓴다.
            git_command_spec resolve {};
            resolve.arguments = make_git_symbolic_full_name_args(request.to);
            git_command_outcome resolved {};
            std::optional<operation_outcome> stop {};
            if (context.run_required(std::move(resolve), resolved, stop) == false)
                return *stop;
            std::string source { resolved.standard_output.substr(0, resolved.standard_output.find_first_of("\r\n")) };
            if (source.empty())
                source = request.to;

            git_command_spec spec {};
            spec.arguments = make_git_fetch_fast_forward_args(source, "refs/heads/" + request.target);
            spec.command_class = git_command_class::local_write;
            const git_command_outcome outcome { context.run(std::move(spec)) };
            if (outcome.accepted)
                return operation_outcome::succeeded(name_of(request.target) + u8"를 fast-forward했습니다.");
            operation_outcome failure { context.failure_from(outcome) };
            // `fetch .`의 거절은 push 거절과 같은 표지(`[rejected] ... (non-fast-forward)`)다.
            if (failure.failure.has_value() && (failure.failure->kind == failure_kind::rejected_non_ff || failure.failure->kind == failure_kind::rejected))
            {
                failure.failure->kind = failure_kind::not_possible_to_fast_forward;
                failure.failure->summary = std::u8string { failure_kind_message(failure_kind::not_possible_to_fast_forward) };
            }
            return failure;
        }

        git_pull_mode pull_mode_of(const pull_strategy strategy) noexcept
        {
            switch (strategy)
            {
            case pull_strategy::rebase:
                return git_pull_mode::rebase;
            case pull_strategy::merge:
                return git_pull_mode::merge;
            case pull_strategy::ff_only:
                break;
            }
            return git_pull_mode::fast_forward_only;
        }

        git_reset_mode reset_mode_of(const reset_mode mode) noexcept
        {
            switch (mode)
            {
            case reset_mode::soft:
                return git_reset_mode::soft;
            case reset_mode::hard:
                return git_reset_mode::hard;
            case reset_mode::mixed:
                break;
            }
            return git_reset_mode::mixed;
        }

        operation_outcome stash_restore(git_operation_context& context, git_arguments arguments, std::u8string summary)
        {
            git_command_spec spec {};
            spec.arguments = std::move(arguments);
            spec.command_class = git_command_class::local_write;
            const git_command_outcome outcome { context.run(std::move(spec)) };
            if (outcome.accepted)
                return operation_outcome::succeeded(std::move(summary));
            operation_outcome failure { context.failure_from(outcome) };
            // 충돌한 stash는 git이 지우지 않는다. 진행 중 상태가 없으므로 continue가 아니라 파일을 직접 고친다.
            const std::optional<git_status_result> status { context.read_status() };
            if (failure.failure.has_value() && status.has_value() && status->conflicts.empty() == false)
            {
                failure.failure->kind = failure_kind::conflict;
                failure.failure->summary = u8"stash를 꺼내다 충돌했습니다. stash는 남아 있습니다.";
            }
            return failure;
        }

        operation_outcome create_tag(git_operation_context& context, const create_tag_request& request)
        {
            const bool signed_tag { request.sign == true };
            const bool no_sign { request.sign == false };
            // 서명한 태그는 항상 주석 태그다 (`tag -s`). passphrase를 기다릴 수 있어 시간 제한 없이 돈다.
            if (request.annotated || signed_tag)
            {
                // annotated 태그는 메시지가 필요하다. 비우면 이름을 메시지로 쓴다.
                const std::string message { with_trailing_newline(request.message.empty() ? request.name : request.message) };
                git_arguments arguments { signed_tag ? make_git_signed_tag_args(request.name, request.sha) : make_git_annotated_tag_args(request.name, request.sha, no_sign) };
                return run_single(context, std::move(arguments), signed_tag ? git_command_class::network : git_command_class::local_write, name_of(request.name) + u8" 태그를 만들었습니다.", message);
            }
            return run_single(context, make_git_tag_args(request.name, request.sha, no_sign), git_command_class::local_write, name_of(request.name) + u8" 태그를 만들었습니다.");
        }

        git_arguments sequence_arguments(const operation_kind kind)
        {
            switch (kind)
            {
            case operation_kind::rebase_continue:
                return make_git_rebase_continue_args();
            case operation_kind::rebase_skip:
                return make_git_rebase_skip_args();
            case operation_kind::rebase_abort:
                return make_git_rebase_abort_args();
            case operation_kind::merge_continue:
                return make_git_merge_continue_args();
            case operation_kind::merge_abort:
                return make_git_merge_abort_args();
            case operation_kind::cherry_pick_continue:
                return make_git_cherry_pick_continue_args();
            case operation_kind::cherry_pick_skip:
                return make_git_cherry_pick_skip_args();
            case operation_kind::cherry_pick_abort:
                return make_git_cherry_pick_abort_args();
            case operation_kind::revert_continue:
                return make_git_revert_continue_args();
            case operation_kind::revert_skip:
                return make_git_revert_skip_args();
            case operation_kind::revert_abort:
                return make_git_revert_abort_args();
            default:
                return {};
            }
        }

        operation_outcome sequence_control(git_operation_context& context, const sequence_control_request& request)
        {
            git_arguments arguments { sequence_arguments(request.kind) };
            if (arguments.empty())
                return operation_outcome::failed(failure_kind::invalid_request, u8"continue·skip·abort 종류가 아닙니다.");
            git_command_spec spec {};
            spec.arguments = std::move(arguments);
            spec.command_class = git_command_class::local_write;
            // 인터랙티브 rebase가 맡긴 reword·squash 메시지는 충돌 뒤 이어지는 줄에서도 써야 한다 (ADR-005).
            if (request.kind == operation_kind::rebase_continue || request.kind == operation_kind::rebase_skip)
                spec.environment = interactive_rebase_continue_environment(context);
            const git_command_outcome outcome { context.run(std::move(spec)) };
            return context.finish_sequence(outcome, u8"진행 중 작업을 이어서 처리했습니다.");
        }

        operation_outcome resolve_file(git_operation_context& context, const resolve_file_request& request)
        {
            git_command_spec take {};
            take.arguments = make_git_checkout_conflict_side_args(request.path, request.side == conflict_side::ours);
            take.command_class = git_command_class::local_write;
            git_command_outcome outcome {};
            std::optional<operation_outcome> stop {};
            if (context.run_required(std::move(take), outcome, stop) == false)
                return *stop;
            const std::vector<std::string> paths { request.path };
            return run_single(context, make_git_add_args(paths), git_command_class::local_write, name_of(request.path) + u8" 충돌을 해결했습니다.");
        }

        operation_outcome remove_stale_index_lock(git_operation_context& context)
        {
            const std::string git_dir { context.read_git_dir() };
            if (git_dir.empty())
                return operation_outcome::failed(failure_kind::not_found, u8"git 저장소가 아닙니다.");
            // 우리가 띄운 git이 이 저장소에서 돌고 있으면 그 잠금은 버려진 것이 아니다.
            if (context.owner().active_process_count(context.job().repository_path) != 0)
                return operation_outcome::failed(failure_kind::refused, u8"이 저장소에서 git이 실행 중이라 index.lock을 지우지 않았습니다.");
            const std::u8string lock { join_path(to_utf8(git_dir), "index.lock") };
            workspace_files& files { context.owner().files() };
            if (files.probe(lock) == path_kind::missing)
                return operation_outcome::succeeded(u8"index.lock이 이미 없습니다.");
            if (files.remove_file(lock) == false)
                return operation_outcome::failed(failure_kind::other, u8"index.lock을 지우지 못했습니다.");
            return operation_outcome::succeeded(u8"남은 index.lock을 지웠습니다.");
        }
    } // namespace

    operation_outcome run_write_operation(git_operation_context& context)
    {
        return std::visit(
            [&context](const auto& request) -> operation_outcome {
                using request_type = std::decay_t<decltype(request)>;
                constexpr git_command_class write { git_command_class::local_write };
                constexpr git_command_class network { git_command_class::network };
                if constexpr (std::is_same_v<request_type, stage_files_request>)
                {
                    git_command_spec spec { context.path_command(request.paths, &make_git_add_args, &make_git_add_from_stdin_args, write) };
                    return run_single(context, std::move(spec.arguments), write, u8"stage했습니다.", std::move(spec.standard_input));
                }
                else if constexpr (std::is_same_v<request_type, unstage_files_request>)
                {
                    git_command_spec spec { context.path_command(request.paths, &make_git_restore_staged_args, &make_git_restore_staged_from_stdin_args, write) };
                    return run_single(context, std::move(spec.arguments), write, u8"unstage했습니다.", std::move(spec.standard_input));
                }
                else if constexpr (std::is_same_v<request_type, discard_files_request>)
                {
                    git_command_spec spec { context.path_command(request.paths, &make_git_restore_worktree_args, &make_git_restore_worktree_from_stdin_args, write) };
                    return run_single(context, std::move(spec.arguments), write, u8"변경을 버렸습니다.", std::move(spec.standard_input));
                }
                else if constexpr (std::is_same_v<request_type, ignore_path_request>)
                    return ignore_path(context, request);
                else if constexpr (std::is_same_v<request_type, trash_files_request>)
                    return trash_files(context, request);
                else if constexpr (std::is_same_v<request_type, commit_request>)
                    return commit(context, request);
                else if constexpr (std::is_same_v<request_type, apply_patch_request>)
                {
                    git_apply_options options {};
                    options.cached = request.cached;
                    options.reverse = request.reverse;
                    options.recount = request.recount;
                    options.unidiff_zero = request.unidiff_zero;
                    return run_single(context, make_git_apply_args(options), write, u8"patch를 적용했습니다.", request.patch);
                }
                else if constexpr (std::is_same_v<request_type, checkout_branch_request>)
                    return checkout_branch(context, request);
                else if constexpr (std::is_same_v<request_type, checkout_remote_branch_request>)
                    return checkout_remote_branch(context, request);
                else if constexpr (std::is_same_v<request_type, checkout_detached_request>)
                    return checkout_detached(context, request);
                else if constexpr (std::is_same_v<request_type, create_branch_request>)
                {
                    if (request.checkout)
                        return run_single(context, make_git_switch_create_args(request.name, request.at), write, name_of(request.name) + u8" 브랜치를 만들고 체크아웃했습니다.");
                    return run_single(context, make_git_branch_create_args(request.name, request.at), write, name_of(request.name) + u8" 브랜치를 만들었습니다.");
                }
                else if constexpr (std::is_same_v<request_type, rename_branch_request>)
                    return run_single(context, make_git_branch_rename_args(request.old_name, request.new_name), write, u8"브랜치 이름을 바꿨습니다.");
                else if constexpr (std::is_same_v<request_type, delete_branch_request>)
                    return run_single(context, make_git_branch_delete_args(request.name, request.force), write, name_of(request.name) + u8" 브랜치를 지웠습니다.");
                else if constexpr (std::is_same_v<request_type, set_upstream_request>)
                    return run_single(context, make_git_branch_set_upstream_args(request.branch, request.remote, request.remote_branch), write,
                        name_of(request.branch) + u8"의 upstream을 " + name_of(request.remote) + u8"/" + name_of(request.remote_branch) + u8"로 정했습니다.");
                else if constexpr (std::is_same_v<request_type, unset_upstream_request>)
                    return run_single(context, make_git_branch_unset_upstream_args(request.branch), write, name_of(request.branch) + u8"의 upstream을 해제했습니다.");
                else if constexpr (std::is_same_v<request_type, fetch_request>)
                {
                    git_fetch_options options {};
                    options.remote = request.remote;
                    options.all = request.all;
                    options.prune = request.prune;
                    options.refspec = request.refspec;
                    return run_single(context, make_git_fetch_args(options), network, u8"가져왔습니다.");
                }
                else if constexpr (std::is_same_v<request_type, pull_request>)
                {
                    return run_sequence(context, make_git_pull_args(pull_mode_of(request.strategy)), network, u8"pull했습니다.");
                }
                else if constexpr (std::is_same_v<request_type, push_execute_request>)
                    return push(context, request);
                else if constexpr (std::is_same_v<request_type, merge_request>)
                    return merge(context, request);
                else if constexpr (std::is_same_v<request_type, rebase_request>)
                    return run_sequence(
                        context, make_git_rebase_args(request.onto, request.branch, request.autostash), write, name_of(request.branch) + u8"를 " + name_of(request.onto) + u8" 위로 rebase했습니다.");
                else if constexpr (std::is_same_v<request_type, fast_forward_request>)
                    return fast_forward(context, request);
                else if constexpr (std::is_same_v<request_type, reset_request>)
                    return run_single(context, make_git_reset_args(reset_mode_of(request.mode), request.to), write, u8"reset했습니다.");
                else if constexpr (std::is_same_v<request_type, cherry_pick_request>)
                {
                    const std::vector<std::string> revisions { request.sha };
                    return run_sequence(context, make_git_cherry_pick_args(revisions), write, u8"cherry-pick했습니다.");
                }
                else if constexpr (std::is_same_v<request_type, revert_request>)
                {
                    const std::vector<std::string> revisions { request.sha };
                    return run_sequence(context, make_git_revert_args(revisions), write, u8"되돌리는 커밋을 만들었습니다.");
                }
                else if constexpr (std::is_same_v<request_type, stash_push_request>)
                {
                    git_stash_push_options options {};
                    options.message = request.message;
                    options.include_untracked = request.include_untracked;
                    return run_single(context, make_git_stash_push_args(options), write, u8"stash에 보관했습니다.");
                }
                else if constexpr (std::is_same_v<request_type, stash_apply_request>)
                    return stash_restore(context, make_git_stash_apply_args(request.index), u8"stash를 적용했습니다.");
                else if constexpr (std::is_same_v<request_type, stash_pop_request>)
                    return stash_restore(context, make_git_stash_pop_args(request.index), u8"stash를 꺼냈습니다.");
                else if constexpr (std::is_same_v<request_type, stash_drop_request>)
                    return run_single(context, make_git_stash_drop_args(request.index), write, u8"stash를 지웠습니다.");
                else if constexpr (std::is_same_v<request_type, create_tag_request>)
                    return create_tag(context, request);
                else if constexpr (std::is_same_v<request_type, push_tag_request>)
                    return run_single(context, make_git_push_tag_args(request.remote, request.name), network, name_of(request.name) + u8" 태그를 push했습니다.");
                else if constexpr (std::is_same_v<request_type, push_tags_request>)
                    return run_single(context, make_git_push_all_tags_args(request.remote), network, u8"모든 태그를 push했습니다.");
                else if constexpr (std::is_same_v<request_type, delete_tag_request>)
                    return run_single(context, make_git_delete_tag_args(request.name), write, name_of(request.name) + u8" 태그를 지웠습니다.");
                else if constexpr (std::is_same_v<request_type, delete_remote_tag_request>)
                    return run_single(context, make_git_delete_remote_tag_args(request.remote, request.name), network, name_of(request.name) + u8" 태그를 원격에서 지웠습니다.");
                else if constexpr (std::is_same_v<request_type, sequence_control_request>)
                    return sequence_control(context, request);
                else if constexpr (std::is_same_v<request_type, resolve_file_request>)
                    return resolve_file(context, request);
                else if constexpr (std::is_same_v<request_type, mark_resolved_request>)
                {
                    git_command_spec spec { context.path_command(request.paths, &make_git_add_args, &make_git_add_from_stdin_args, write) };
                    return run_single(context, std::move(spec.arguments), write, u8"해결됨으로 표시했습니다.", std::move(spec.standard_input));
                }
                else if constexpr (std::is_same_v<request_type, remove_stale_index_lock_request>)
                    return remove_stale_index_lock(context);
                else if constexpr (std::is_same_v<request_type, interactive_rebase_request>)
                    return run_interactive_rebase(context, request);
                else
                    return operation_outcome::failed(failure_kind::invalid_request, u8"쓰기 lane에서 실행할 수 없는 작업입니다.");
            },
            context.job().operation);
    }
} // namespace cyella
