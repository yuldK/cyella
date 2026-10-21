#include "domain/path_syntax.h"
#include "domain/text_catalog.h"
#include "infrastructure/git_extension_operations.h"
#include "infrastructure/git_worktree_commands.h"
#include "infrastructure/git_worktree_parser.h"

#include <algorithm>
#include <utility>

namespace cyella {
    namespace {
        std::u8string name_of(const std::string_view value)
        {
            return to_utf8(value);
        }
    } // namespace

    std::vector<worktree_info> read_worktrees(git_operation_context& context)
    {
        git_command_spec spec {};
        spec.arguments = make_git_worktree_list_args();
        const git_command_outcome outcome { context.run(std::move(spec)) };
        if (outcome.accepted == false)
            return {};
        std::vector<worktree_info> worktrees { parse_git_worktree_list(outcome.standard_output) };
        const std::u8string current { normalize_path_key(context.job().repository_path) };
        for (worktree_info& worktree : worktrees)
            worktree.is_current = normalize_path_key(to_utf8(worktree.path)) == current;
        return worktrees;
    }

    operation_outcome run_worktree_add(git_operation_context& context, const worktree_add_request& request)
    {
        if (is_absolute_windows_path(request.path) == false)
            return operation_outcome::failed(failure_kind::invalid_request, tr(u8"worktree.path_must_be_absolute"));
        if (request.new_branch.empty() && request.detach == false && request.branch.empty())
            return operation_outcome::failed(failure_kind::invalid_request, tr(u8"worktree.branch_required"));

        git_worktree_add_options options {};
        options.path = to_bytes(request.path);
        options.branch = request.branch;
        options.new_branch = request.new_branch;
        options.start_point = request.start_point;
        options.detach = request.detach;

        git_command_spec spec {};
        spec.arguments = make_git_worktree_add_args(options);
        spec.command_class = git_command_class::local_write;
        git_command_outcome outcome {};
        std::optional<operation_outcome> stop {};
        if (context.run_required(std::move(spec), outcome, stop) == false)
            return *stop;
        return operation_outcome::succeeded(tr(u8"worktree.created", { { u8"path", request.path } }));
    }

    operation_outcome run_worktree_remove(git_operation_context& context, const worktree_remove_request& request)
    {
        if (request.path.empty())
            return operation_outcome::failed(failure_kind::invalid_request, tr(u8"worktree.remove_path_required"));

        // 목록에서 찾아 주 작업 트리와 잠금을 먼저 가린다. 경로는 대소문자와 구분자를 무시하고 비교한다.
        const std::vector<worktree_info> worktrees { read_worktrees(context) };
        const std::u8string key { normalize_path_key(request.path) };
        const auto found { std::ranges::find_if(worktrees, [&key](const worktree_info& worktree) { return normalize_path_key(to_utf8(worktree.path)) == key; }) };
        if (found == worktrees.end())
            return operation_outcome::failed(failure_kind::not_found, tr(u8"worktree.not_in_repository", { { u8"path", request.path } }));
        if (found->is_main)
            return operation_outcome::failed(failure_kind::refused, tr(u8"worktree.cannot_remove_main"));
        if (found->is_current)
            return operation_outcome::failed(failure_kind::refused, tr(u8"worktree.cannot_remove_current"));
        if (found->locked && request.force == false)
            return operation_outcome::failed(
                failure_kind::refused, found->lock_reason.empty() ? tr(u8"worktree.remove_locked") : tr(u8"worktree.remove_locked_reason", { { u8"reason", name_of(found->lock_reason) } }));

        git_command_spec spec {};
        spec.arguments = make_git_worktree_remove_args(found->path, request.force, found->locked);
        spec.command_class = git_command_class::local_write;
        git_command_outcome outcome {};
        std::optional<operation_outcome> stop {};
        if (context.run_required(std::move(spec), outcome, stop) == false)
            return *stop;
        return operation_outcome::succeeded(tr(u8"worktree.removed"));
    }

    operation_outcome run_worktree_prune(git_operation_context& context)
    {
        git_command_spec spec {};
        spec.arguments = make_git_worktree_prune_args();
        spec.command_class = git_command_class::local_write;
        git_command_outcome outcome {};
        std::optional<operation_outcome> stop {};
        if (context.run_required(std::move(spec), outcome, stop) == false)
            return *stop;
        return operation_outcome::succeeded(tr(u8"worktree.pruned"));
    }
} // namespace cyella
