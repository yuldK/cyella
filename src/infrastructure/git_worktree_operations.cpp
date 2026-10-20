#include "domain/path_syntax.h"
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
            return operation_outcome::failed(failure_kind::invalid_request, u8"작업 트리 폴더는 절대 경로여야 합니다.");
        if (request.new_branch.empty() && request.detach == false && request.branch.empty())
            return operation_outcome::failed(failure_kind::invalid_request, u8"체크아웃할 브랜치나 새 브랜치 이름이 필요합니다.");

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
        return operation_outcome::succeeded(u8"작업 트리를 만들었습니다: " + request.path);
    }

    operation_outcome run_worktree_remove(git_operation_context& context, const worktree_remove_request& request)
    {
        if (request.path.empty())
            return operation_outcome::failed(failure_kind::invalid_request, u8"지울 작업 트리 경로가 필요합니다.");

        // 목록에서 찾아 주 작업 트리와 잠금을 먼저 가린다. 경로는 대소문자와 구분자를 무시하고 비교한다.
        const std::vector<worktree_info> worktrees { read_worktrees(context) };
        const std::u8string key { normalize_path_key(request.path) };
        const auto found { std::ranges::find_if(worktrees, [&key](const worktree_info& worktree) { return normalize_path_key(to_utf8(worktree.path)) == key; }) };
        if (found == worktrees.end())
            return operation_outcome::failed(failure_kind::not_found, u8"이 저장소의 작업 트리가 아닙니다: " + request.path);
        if (found->is_main)
            return operation_outcome::failed(failure_kind::refused, u8"주 작업 트리는 지울 수 없습니다.");
        if (found->is_current)
            return operation_outcome::failed(failure_kind::refused, u8"지금 열어 둔 작업 트리는 이 탭에서 지울 수 없습니다. 주 작업 트리 탭에서 지우세요.");
        if (found->locked && request.force == false)
            return operation_outcome::failed(failure_kind::refused,
                u8"잠긴 작업 트리입니다" + (found->lock_reason.empty() ? std::u8string {} : u8" (" + name_of(found->lock_reason) + u8")") + u8". 강제로 지우려면 확인이 필요합니다.");

        git_command_spec spec {};
        spec.arguments = make_git_worktree_remove_args(found->path, request.force, found->locked);
        spec.command_class = git_command_class::local_write;
        git_command_outcome outcome {};
        std::optional<operation_outcome> stop {};
        if (context.run_required(std::move(spec), outcome, stop) == false)
            return *stop;
        return operation_outcome::succeeded(u8"작업 트리를 지웠습니다.");
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
        return operation_outcome::succeeded(u8"사라진 작업 트리의 기록을 정리했습니다.");
    }
} // namespace cyella
