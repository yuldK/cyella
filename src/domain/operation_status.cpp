#include "domain/operation_status.h"

#include "domain/text_catalog.h"

namespace cyella {
    bool operation_status::finished() const noexcept
    {
        // 체크아웃·충돌의 needs_input은 worker가 끝난 뒤의 상태다. 인증(askpass)의 needs_input은
        // git이 도우미의 답을 기다리는 동안이라 아직 끝나지 않았다.
        if (state == operation_state::needs_input)
            return needs_input.has_value() && (needs_input->kind == input_kind::dirty_worktree || needs_input->kind == input_kind::conflict);
        return state == operation_state::succeeded || state == operation_state::failed || state == operation_state::cancelled;
    }

    std::string_view operation_kind_name(const operation_kind value) noexcept
    {
        switch (value)
        {
        case operation_kind::refresh:
            return "refresh";
        case operation_kind::load_more_log:
            return "load_more_log";
        case operation_kind::show_commit:
            return "show_commit";
        case operation_kind::diff_file:
            return "diff_file";
        case operation_kind::is_ancestor:
            return "is_ancestor";
        case operation_kind::count_commits:
            return "count_commits";
        case operation_kind::refresh_remote_tags:
            return "refresh_remote_tags";
        case operation_kind::stage_files:
            return "stage_files";
        case operation_kind::unstage_files:
            return "unstage_files";
        case operation_kind::discard_files:
            return "discard_files";
        case operation_kind::ignore_path:
            return "ignore_path";
        case operation_kind::commit:
            return "commit";
        case operation_kind::apply_patch:
            return "apply_patch";
        case operation_kind::checkout_branch:
            return "checkout_branch";
        case operation_kind::checkout_remote_branch:
            return "checkout_remote_branch";
        case operation_kind::checkout_detached:
            return "checkout_detached";
        case operation_kind::create_branch:
            return "create_branch";
        case operation_kind::rename_branch:
            return "rename_branch";
        case operation_kind::delete_branch:
            return "delete_branch";
        case operation_kind::fetch:
            return "fetch";
        case operation_kind::pull:
            return "pull";
        case operation_kind::push_execute:
            return "push_execute";
        case operation_kind::merge:
            return "merge";
        case operation_kind::rebase:
            return "rebase";
        case operation_kind::fast_forward:
            return "fast_forward";
        case operation_kind::reset:
            return "reset";
        case operation_kind::cherry_pick:
            return "cherry_pick";
        case operation_kind::revert:
            return "revert";
        case operation_kind::stash_push:
            return "stash_push";
        case operation_kind::stash_apply:
            return "stash_apply";
        case operation_kind::stash_pop:
            return "stash_pop";
        case operation_kind::stash_drop:
            return "stash_drop";
        case operation_kind::create_tag:
            return "create_tag";
        case operation_kind::push_tag:
            return "push_tag";
        case operation_kind::push_tags:
            return "push_tags";
        case operation_kind::delete_tag:
            return "delete_tag";
        case operation_kind::delete_remote_tag:
            return "delete_remote_tag";
        case operation_kind::rebase_continue:
            return "rebase_continue";
        case operation_kind::rebase_skip:
            return "rebase_skip";
        case operation_kind::rebase_abort:
            return "rebase_abort";
        case operation_kind::merge_continue:
            return "merge_continue";
        case operation_kind::merge_abort:
            return "merge_abort";
        case operation_kind::cherry_pick_continue:
            return "cherry_pick_continue";
        case operation_kind::cherry_pick_skip:
            return "cherry_pick_skip";
        case operation_kind::cherry_pick_abort:
            return "cherry_pick_abort";
        case operation_kind::revert_continue:
            return "revert_continue";
        case operation_kind::revert_skip:
            return "revert_skip";
        case operation_kind::revert_abort:
            return "revert_abort";
        case operation_kind::resolve_file:
            return "resolve_file";
        case operation_kind::mark_resolved:
            return "mark_resolved";
        case operation_kind::remove_stale_index_lock:
            return "remove_stale_index_lock";
        case operation_kind::clone:
            return "clone";
        case operation_kind::init:
            return "init";
        case operation_kind::inspect_path:
            return "inspect_path";
        case operation_kind::ssh_keygen:
            return "ssh_keygen";
        case operation_kind::ssh_connection_test:
            return "ssh_connection_test";
        case operation_kind::submodule_update:
            return "submodule_update";
        case operation_kind::submodule_sync:
            return "submodule_sync";
        case operation_kind::worktree_add:
            return "worktree_add";
        case operation_kind::worktree_remove:
            return "worktree_remove";
        case operation_kind::worktree_prune:
            return "worktree_prune";
        case operation_kind::lfs_pull:
            return "lfs_pull";
        case operation_kind::lfs_fetch:
            return "lfs_fetch";
        case operation_kind::lfs_track:
            return "lfs_track";
        case operation_kind::load_rebase_plan:
            return "load_rebase_plan";
        case operation_kind::interactive_rebase:
            return "interactive_rebase";
        case operation_kind::refresh_status:
            return "refresh_status";
        case operation_kind::set_upstream:
            return "set_upstream";
        case operation_kind::unset_upstream:
            return "unset_upstream";
        case operation_kind::trash_files:
            return "trash_files";
        case operation_kind::verify_signatures:
            return "verify_signatures";
        }
        return "unknown";
    }

    std::string_view operation_state_name(const operation_state value) noexcept
    {
        switch (value)
        {
        case operation_state::queued:
            return "queued";
        case operation_state::running:
            return "running";
        case operation_state::needs_input:
            return "needs_input";
        case operation_state::succeeded:
            return "succeeded";
        case operation_state::failed:
            return "failed";
        case operation_state::cancelled:
            return "cancelled";
        }
        return "unknown";
    }

    std::string_view failure_kind_name(const failure_kind value) noexcept
    {
        switch (value)
        {
        case failure_kind::other:
            return "other";
        case failure_kind::rejected_fetch_first:
            return "rejected_fetch_first";
        case failure_kind::rejected_non_ff:
            return "rejected_non_ff";
        case failure_kind::stale_info:
            return "stale_info";
        case failure_kind::rejected:
            return "rejected";
        case failure_kind::auth_cancelled:
            return "auth_cancelled";
        case failure_kind::auth_failed:
            return "auth_failed";
        case failure_kind::host_key_refused:
            return "host_key_refused";
        case failure_kind::host_key_changed:
            return "host_key_changed";
        case failure_kind::conflict:
            return "conflict";
        case failure_kind::dirty_worktree:
            return "dirty_worktree";
        case failure_kind::index_lock:
            return "index_lock";
        case failure_kind::offline:
            return "offline";
        case failure_kind::not_found:
            return "not_found";
        case failure_kind::not_possible_to_fast_forward:
            return "not_possible_to_fast_forward";
        case failure_kind::refused:
            return "refused";
        case failure_kind::invalid_request:
            return "invalid_request";
        case failure_kind::timed_out:
            return "timed_out";
        case failure_kind::branch_checked_out_elsewhere:
            return "branch_checked_out_elsewhere";
        case failure_kind::signing_failed:
            return "signing_failed";
        case failure_kind::signing_key_missing:
            return "signing_key_missing";
        case failure_kind::signing_passphrase_failed:
            return "signing_passphrase_failed";
        }
        return "other";
    }

    std::string_view input_kind_name(const input_kind value) noexcept
    {
        switch (value)
        {
        case input_kind::dirty_worktree:
            return "dirty_worktree";
        case input_kind::conflict:
            return "conflict";
        case input_kind::passphrase:
            return "passphrase";
        case input_kind::host_key:
            return "host_key";
        case input_kind::credentials:
            return "credentials";
        }
        return "unknown";
    }

    input_kind input_kind_for_prompt(const credential_prompt_kind kind) noexcept
    {
        switch (kind)
        {
        case credential_prompt_kind::ssh_key_passphrase:
        case credential_prompt_kind::ssh_new_key_passphrase:
        case credential_prompt_kind::ssh_new_key_passphrase_confirm:
            return input_kind::passphrase;
        case credential_prompt_kind::ssh_host_key_confirm:
            return input_kind::host_key;
        case credential_prompt_kind::ssh_password:
        case credential_prompt_kind::https_username:
        case credential_prompt_kind::https_password:
        case credential_prompt_kind::unknown:
            return input_kind::credentials;
        }
        return input_kind::credentials;
    }

    std::u8string failure_kind_message(const failure_kind value)
    {
        switch (value)
        {
        case failure_kind::other:
            return tr(u8"operation.failure.other");
        case failure_kind::rejected_fetch_first:
            return tr(u8"operation.failure.rejected_fetch_first");
        case failure_kind::rejected_non_ff:
            return tr(u8"operation.failure.rejected_non_ff");
        case failure_kind::stale_info:
            return tr(u8"operation.failure.stale_info");
        case failure_kind::rejected:
            return tr(u8"operation.failure.rejected");
        case failure_kind::auth_cancelled:
            return tr(u8"operation.failure.auth_cancelled");
        case failure_kind::auth_failed:
            return tr(u8"operation.failure.auth_failed");
        case failure_kind::host_key_refused:
            return tr(u8"operation.failure.host_key_refused");
        case failure_kind::host_key_changed:
            return tr(u8"operation.failure.host_key_changed");
        case failure_kind::conflict:
            return tr(u8"operation.failure.conflict");
        case failure_kind::dirty_worktree:
            return tr(u8"operation.failure.dirty_worktree");
        case failure_kind::index_lock:
            return tr(u8"operation.failure.index_lock");
        case failure_kind::offline:
            return tr(u8"operation.failure.offline");
        case failure_kind::not_found:
            return tr(u8"operation.failure.not_found");
        case failure_kind::not_possible_to_fast_forward:
            return tr(u8"operation.failure.not_possible_to_fast_forward");
        case failure_kind::refused:
            return tr(u8"operation.failure.refused");
        case failure_kind::invalid_request:
            return tr(u8"operation.failure.invalid_request");
        case failure_kind::timed_out:
            return tr(u8"operation.failure.timed_out");
        case failure_kind::branch_checked_out_elsewhere:
            return tr(u8"operation.failure.branch_checked_out_elsewhere");
        case failure_kind::signing_failed:
            return tr(u8"operation.failure.signing_failed");
        case failure_kind::signing_key_missing:
            return tr(u8"operation.failure.signing_key_missing");
        case failure_kind::signing_passphrase_failed:
            return tr(u8"operation.failure.signing_passphrase_failed");
        }
        return tr(u8"operation.failure.other");
    }
} // namespace cyella
