#include "application/git_operation.h"

#include <type_traits>

namespace cyella {
    namespace {
        template<typename request_type>
        constexpr operation_kind fixed_kind_of() noexcept
        {
            if constexpr (std::is_same_v<request_type, refresh_request>)
                return operation_kind::refresh;
            else if constexpr (std::is_same_v<request_type, load_more_log_request>)
                return operation_kind::load_more_log;
            else if constexpr (std::is_same_v<request_type, show_commit_request>)
                return operation_kind::show_commit;
            else if constexpr (std::is_same_v<request_type, diff_file_request>)
                return operation_kind::diff_file;
            else if constexpr (std::is_same_v<request_type, is_ancestor_request>)
                return operation_kind::is_ancestor;
            else if constexpr (std::is_same_v<request_type, count_commits_request>)
                return operation_kind::count_commits;
            else if constexpr (std::is_same_v<request_type, refresh_remote_tags_request>)
                return operation_kind::refresh_remote_tags;
            else if constexpr (std::is_same_v<request_type, stage_files_request>)
                return operation_kind::stage_files;
            else if constexpr (std::is_same_v<request_type, unstage_files_request>)
                return operation_kind::unstage_files;
            else if constexpr (std::is_same_v<request_type, discard_files_request>)
                return operation_kind::discard_files;
            else if constexpr (std::is_same_v<request_type, ignore_path_request>)
                return operation_kind::ignore_path;
            else if constexpr (std::is_same_v<request_type, commit_request>)
                return operation_kind::commit;
            else if constexpr (std::is_same_v<request_type, apply_patch_request>)
                return operation_kind::apply_patch;
            else if constexpr (std::is_same_v<request_type, checkout_branch_request>)
                return operation_kind::checkout_branch;
            else if constexpr (std::is_same_v<request_type, checkout_remote_branch_request>)
                return operation_kind::checkout_remote_branch;
            else if constexpr (std::is_same_v<request_type, checkout_detached_request>)
                return operation_kind::checkout_detached;
            else if constexpr (std::is_same_v<request_type, create_branch_request>)
                return operation_kind::create_branch;
            else if constexpr (std::is_same_v<request_type, rename_branch_request>)
                return operation_kind::rename_branch;
            else if constexpr (std::is_same_v<request_type, delete_branch_request>)
                return operation_kind::delete_branch;
            else if constexpr (std::is_same_v<request_type, fetch_request>)
                return operation_kind::fetch;
            else if constexpr (std::is_same_v<request_type, pull_request>)
                return operation_kind::pull;
            else if constexpr (std::is_same_v<request_type, push_execute_request>)
                return operation_kind::push_execute;
            else if constexpr (std::is_same_v<request_type, merge_request>)
                return operation_kind::merge;
            else if constexpr (std::is_same_v<request_type, rebase_request>)
                return operation_kind::rebase;
            else if constexpr (std::is_same_v<request_type, fast_forward_request>)
                return operation_kind::fast_forward;
            else if constexpr (std::is_same_v<request_type, reset_request>)
                return operation_kind::reset;
            else if constexpr (std::is_same_v<request_type, cherry_pick_request>)
                return operation_kind::cherry_pick;
            else if constexpr (std::is_same_v<request_type, revert_request>)
                return operation_kind::revert;
            else if constexpr (std::is_same_v<request_type, stash_push_request>)
                return operation_kind::stash_push;
            else if constexpr (std::is_same_v<request_type, stash_apply_request>)
                return operation_kind::stash_apply;
            else if constexpr (std::is_same_v<request_type, stash_pop_request>)
                return operation_kind::stash_pop;
            else if constexpr (std::is_same_v<request_type, stash_drop_request>)
                return operation_kind::stash_drop;
            else if constexpr (std::is_same_v<request_type, create_tag_request>)
                return operation_kind::create_tag;
            else if constexpr (std::is_same_v<request_type, push_tag_request>)
                return operation_kind::push_tag;
            else if constexpr (std::is_same_v<request_type, push_tags_request>)
                return operation_kind::push_tags;
            else if constexpr (std::is_same_v<request_type, delete_tag_request>)
                return operation_kind::delete_tag;
            else if constexpr (std::is_same_v<request_type, delete_remote_tag_request>)
                return operation_kind::delete_remote_tag;
            else if constexpr (std::is_same_v<request_type, resolve_file_request>)
                return operation_kind::resolve_file;
            else if constexpr (std::is_same_v<request_type, mark_resolved_request>)
                return operation_kind::mark_resolved;
            else if constexpr (std::is_same_v<request_type, remove_stale_index_lock_request>)
                return operation_kind::remove_stale_index_lock;
            else if constexpr (std::is_same_v<request_type, clone_request>)
                return operation_kind::clone;
            else if constexpr (std::is_same_v<request_type, init_request>)
                return operation_kind::init;
            else if constexpr (std::is_same_v<request_type, inspect_path_request>)
                return operation_kind::inspect_path;
            else
                return operation_kind::refresh;
        }
    } // namespace

    operation_kind operation_kind_of(const git_operation& operation) noexcept
    {
        return std::visit(
            [](const auto& request) noexcept {
                using request_type = std::decay_t<decltype(request)>;
                // continue·skip·abort는 한 요청 타입이 여러 종류를 나른다.
                if constexpr (std::is_same_v<request_type, sequence_control_request>)
                    return request.kind;
                else
                    return fixed_kind_of<request_type>();
            },
            operation);
    }

    operation_lane operation_lane_of(const operation_kind kind) noexcept
    {
        switch (kind)
        {
        case operation_kind::refresh:
        case operation_kind::load_more_log:
        case operation_kind::show_commit:
        case operation_kind::diff_file:
        case operation_kind::is_ancestor:
        case operation_kind::count_commits:
        case operation_kind::refresh_remote_tags:
        case operation_kind::inspect_path:
            return operation_lane::read;
        case operation_kind::clone:
        case operation_kind::init:
            return operation_lane::setup;
        default:
            return operation_lane::write;
        }
    }

    bool is_setup_operation(const operation_kind kind) noexcept
    {
        return kind == operation_kind::clone || kind == operation_kind::init || kind == operation_kind::inspect_path;
    }

    const temporary_ssh_trust* ssh_override_of(const git_operation& operation) noexcept
    {
        const auto trust_of = [](const auto& request) noexcept -> const temporary_ssh_trust* { return request.ssh_command_override.has_value() ? &*request.ssh_command_override : nullptr; };
        if (const auto* const fetch { std::get_if<fetch_request>(&operation) })
            return trust_of(*fetch);
        if (const auto* const pull { std::get_if<pull_request>(&operation) })
            return trust_of(*pull);
        if (const auto* const push { std::get_if<push_execute_request>(&operation) })
            return trust_of(*push);
        if (const auto* const clone { std::get_if<clone_request>(&operation) })
            return trust_of(*clone);
        if (const auto* const tags { std::get_if<refresh_remote_tags_request>(&operation) })
            return trust_of(*tags);
        return nullptr;
    }
} // namespace cyella
