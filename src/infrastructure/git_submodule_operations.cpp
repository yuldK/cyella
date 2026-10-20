#include "infrastructure/git_extension_operations.h"
#include "infrastructure/git_submodule_commands.h"
#include "infrastructure/git_submodule_parser.h"

#include <optional>
#include <utility>

namespace cyella {
    namespace {
        // 종료 코드 128은 기록한 커밋이 submodule에 없다는 뜻이다. 어느 쪽이든 조상이 아니다.
        std::optional<bool> recorded_is_ancestor(git_operation_context& context, const submodule_info& info)
        {
            if (info.recorded_sha.empty() || info.checked_out_sha.empty())
                return std::nullopt;
            git_command_spec spec {};
            spec.arguments = make_git_is_ancestor_args(info.recorded_sha, info.checked_out_sha);
            spec.accepted_exit_codes = { 0, 1, 128 };
            spec.working_directory = join_path(context.job().repository_path, info.path);
            const git_command_outcome outcome { context.run(std::move(spec)) };
            if (outcome.accepted == false)
                return std::nullopt;
            return outcome.exit_code() == 0;
        }
    } // namespace

    std::vector<submodule_info> read_submodules(git_operation_context& context, const repository_snapshot& snapshot)
    {
        // `.gitmodules`가 없으면 submodule이 없다. git을 부르지 않는다.
        if (context.owner().files().probe(join_path(context.job().repository_path, ".gitmodules")) == path_kind::missing)
            return {};

        submodule_sources sources {};
        git_command_spec status_spec {};
        status_spec.arguments = make_git_submodule_status_args();
        const git_command_outcome listed { context.run(std::move(status_spec)) };
        if (listed.accepted == false)
            return {};
        sources.status = parse_git_submodule_status(listed.standard_output);
        if (sources.status.empty())
            return {};

        // 항목이 없으면 종료 코드 1이다.
        git_command_spec modules_spec {};
        modules_spec.arguments = make_git_gitmodules_config_args();
        modules_spec.accepted_exit_codes = { 0, 1 };
        const git_command_outcome modules { context.run(std::move(modules_spec)) };
        if (modules.accepted)
            sources.modules = parse_gitmodules_config(modules.standard_output);

        std::vector<std::string> paths {};
        for (const submodule_status_entry& entry : sources.status)
            paths.push_back(entry.path);
        git_command_spec link_spec {};
        link_spec.arguments = make_git_gitlinks_args(paths);
        const git_command_outcome links { context.run(std::move(link_spec)) };
        if (links.accepted)
            sources.gitlinks = parse_git_gitlinks(links.standard_output);

        for (const std::vector<file_change>* const group : { &snapshot.staged, &snapshot.unstaged })
            for (const file_change& change : *group)
                if (change.is_submodule)
                    sources.changes.push_back(change);

        return assemble_submodules(sources, [&context](const submodule_info& info) { return recorded_is_ancestor(context, info); });
    }

    operation_outcome run_submodule_update(git_operation_context& context, const submodule_update_request& request)
    {
        git_submodule_update_options options {};
        options.init = request.init;
        options.recursive = request.recursive;
        options.paths = request.paths;
        git_command_spec spec {};
        spec.arguments = make_git_submodule_update_args(options);
        // 처음 받는 submodule은 네트워크를 쓰고 인증을 물을 수 있다.
        spec.command_class = git_command_class::network;
        git_command_outcome outcome {};
        std::optional<operation_outcome> stop {};
        if (context.run_required(std::move(spec), outcome, stop) == false)
            return *stop;
        return operation_outcome::succeeded(u8"submodule을 기록된 커밋으로 맞췄습니다.");
    }

    operation_outcome run_submodule_sync(git_operation_context& context, const submodule_sync_request& request)
    {
        git_command_spec spec {};
        spec.arguments = make_git_submodule_sync_args(request.recursive, request.paths);
        spec.command_class = git_command_class::local_write;
        git_command_outcome outcome {};
        std::optional<operation_outcome> stop {};
        if (context.run_required(std::move(spec), outcome, stop) == false)
            return *stop;
        return operation_outcome::succeeded(u8"submodule의 url을 .gitmodules에 맞췄습니다.");
    }
} // namespace cyella
