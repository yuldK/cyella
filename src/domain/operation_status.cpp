#include "domain/operation_status.h"

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

    std::u8string_view failure_kind_message(const failure_kind value) noexcept
    {
        switch (value)
        {
        case failure_kind::other:
            return u8"명령이 실패했습니다.";
        case failure_kind::rejected_fetch_first:
            return u8"원격에 내게 없는 커밋이 있습니다. 먼저 Pull 하세요.";
        case failure_kind::rejected_non_ff:
            return u8"원격 브랜치가 내 브랜치의 조상이 아니어서 push가 거절되었습니다.";
        case failure_kind::stale_info:
            return u8"원격 브랜치가 마지막으로 가져온 뒤 바뀌어 강제 push가 거절되었습니다.";
        case failure_kind::rejected:
            return u8"원격이 변경을 거절했습니다.";
        case failure_kind::auth_cancelled:
            return u8"인증을 취소했습니다.";
        case failure_kind::auth_failed:
            return u8"인증에 실패했습니다.";
        case failure_kind::host_key_refused:
            return u8"SSH 호스트 키를 확인하지 못해 연결하지 않았습니다.";
        case failure_kind::host_key_changed:
            return u8"SSH 호스트 키가 known_hosts에 기록된 키와 다릅니다.";
        case failure_kind::conflict:
            return u8"충돌이 발생했습니다. 해결한 뒤 계속하세요.";
        case failure_kind::dirty_worktree:
            return u8"작업 트리의 변경이 덮어써질 수 있어 중단했습니다.";
        case failure_kind::index_lock:
            return u8"index.lock이 남아 있어 저장소를 바꿀 수 없습니다.";
        case failure_kind::offline:
            return u8"원격에 연결하지 못했습니다.";
        case failure_kind::not_found:
            return u8"저장소나 대상을 찾을 수 없습니다.";
        case failure_kind::not_possible_to_fast_forward:
            return u8"fast-forward로 옮길 수 없습니다.";
        case failure_kind::refused:
            return u8"지금은 이 작업을 할 수 없습니다.";
        case failure_kind::invalid_request:
            return u8"요청 값이 올바르지 않습니다.";
        case failure_kind::timed_out:
            return u8"명령이 제한 시간 안에 끝나지 않았습니다.";
        case failure_kind::branch_checked_out_elsewhere:
            return u8"이 브랜치는 다른 worktree에서 체크아웃되어 있어 옮길 수 없습니다.";
        case failure_kind::signing_failed:
            return u8"서명하지 못했습니다. 환경 설정의 서명에서 서명 프로그램(gpg·ssh-keygen)과 키를 확인하세요.";
        case failure_kind::signing_key_missing:
            return u8"서명할 키를 찾지 못했습니다. 환경 설정의 서명에서 키를 고르세요.";
        case failure_kind::signing_passphrase_failed:
            return u8"서명 키의 passphrase를 받지 못했습니다.";
        }
        return u8"명령이 실패했습니다.";
    }
} // namespace cyella
