#pragma once

#include "domain/process_execution.h"
#include "domain/repository_summary.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // UI가 operation을 요청할 때 붙는 번호다. application/operation_executor.h의 별칭과 같은
    // 타입이다. 도메인 값(operation_status)이 application 헤더에 기대지 않게 여기에도 둔다.
    using operation_id = std::uint64_t;

    // contract의 동작 이름과 1:1이다. 이름 문자열은 `operation_kind_name`이 준다.
    enum class operation_kind
    {
        // 읽기
        refresh,
        load_more_log,
        show_commit,
        diff_file,
        is_ancestor,
        count_commits,
        refresh_remote_tags,
        // 작업 영역
        stage_files,
        unstage_files,
        discard_files,
        ignore_path,
        commit,
        apply_patch,
        // 브랜치
        checkout_branch,
        checkout_remote_branch,
        checkout_detached,
        create_branch,
        rename_branch,
        delete_branch,
        // 원격
        fetch,
        pull,
        push_execute,
        // 이력
        merge,
        rebase,
        fast_forward,
        reset,
        cherry_pick,
        revert,
        // stash
        stash_push,
        stash_apply,
        stash_pop,
        stash_drop,
        // 태그
        create_tag,
        push_tag,
        push_tags,
        delete_tag,
        delete_remote_tag,
        // 충돌
        rebase_continue,
        rebase_skip,
        rebase_abort,
        merge_continue,
        merge_abort,
        cherry_pick_continue,
        cherry_pick_skip,
        cherry_pick_abort,
        revert_continue,
        revert_skip,
        revert_abort,
        resolve_file,
        mark_resolved,
        // 복구
        remove_stale_index_lock,
        // 저장소 준비. 저장소 id가 아니라 대상 경로로 묶인다.
        clone,
        init,
        inspect_path,
    };

    enum class operation_state
    {
        // 같은 저장소의 쓰기 lane에서 앞 작업을 기다린다.
        queued,
        running,
        // 사용자의 결정이 필요해 **끝난** 상태다. worker는 기다리지 않는다. UI는 고른 정책으로
        // 새 operation을 낸다. 인증(askpass)만은 git이 멈춰 있는 동안 이 상태로 바뀐다.
        needs_input,
        succeeded,
        failed,
        cancelled,
    };

    // 실패 배너가 조치 단추를 고르는 기준이다.
    enum class failure_kind
    {
        // 아래 어느 것도 아니다. stderr 원문을 그대로 보인다.
        other,
        // push 거절: 원격에 내게 없는 커밋이 있다(`fetch first`).
        rejected_fetch_first,
        // push 거절: 내 브랜치가 원격의 조상이 아니다(`non-fast-forward`).
        rejected_non_ff,
        // `--force-with-lease`가 기대한 원격 값이 바뀌었다(`stale info`).
        stale_info,
        // 그 밖의 원격 거절(hook, 이미 있는 태그).
        rejected,
        auth_cancelled,
        auth_failed,
        // 처음 보는 호스트 키를 거절했거나 확인하지 못했다.
        host_key_refused,
        // known_hosts의 키와 다르다. ssh는 묻지 않고 실패하므로 stderr에서 읽는다.
        host_key_changed,
        conflict,
        dirty_worktree,
        // `index.lock`이 남아 있다. 자동으로 지우지 않는다(`remove_stale_index_lock`).
        index_lock,
        offline,
        not_found,
        // fast-forward로 옮길 수 없다(`merge --ff-only`, `fetch . A:B`의 거절).
        not_possible_to_fast_forward,
        // 실행 전 확인에서 거절했다(다른 git이 도는 중 잠금 삭제 등).
        refused,
        // 요청 값이 잘못되어 git을 실행하지 않았다(빈 경로, 옵션처럼 보이는 이름).
        invalid_request,
        timed_out,
    };

    struct operation_failure
    {
        failure_kind kind { failure_kind::other };
        // 배너에 보일 한 줄이다.
        std::u8string summary {};
        // [자세히]가 보일 원문이다. 마지막으로 실패한 명령의 stderr(마스킹됨)와 충돌 문구가 든 stdout이다.
        std::u8string detail {};
    };

    struct operation_progress
    {
        // `Receiving objects`처럼 git `--progress`의 단계 이름이다. 비어 있으면 단계를 모른다.
        std::u8string phase_text {};
        // 0..1. 전체 크기를 모르는 단계는 값이 없다.
        std::optional<double> fraction {};
    };

    // operation 하나가 실행한 git 명령 하나다. 작업 로그가 순서대로 보인다.
    struct operation_command
    {
        // 마스킹된 명령줄이다. 사용자 정보가 든 URL은 가려져 있다.
        std::u8string command_line {};
        process_completion completion { process_completion::exited };
        // `exited`일 때만 값이 있다.
        std::optional<std::int32_t> exit_code {};
        std::chrono::milliseconds duration {};
    };

    enum class input_kind
    {
        // 체크아웃이 작업 트리의 변경 때문에 막혔다. 정책을 골라 다시 낸다.
        dirty_worktree,
        // merge·rebase·cherry-pick·revert가 충돌로 멈췄다. 해결 뒤 continue, 또는 abort한다.
        conflict,
        // 아래는 askpass 연결이 채운다.
        passphrase,
        host_key,
        credentials,
    };

    struct input_request
    {
        input_kind kind { input_kind::dirty_worktree };
        // dirty_worktree: 덮어써질 파일. conflict: 충돌 파일. 저장소 루트 기준 경로다.
        std::vector<std::string> files {};
        // dirty_worktree: 옮기려던 대상(브랜치 이름이나 SHA)이다.
        std::string target {};
        // dirty_worktree: 덮어써질 파일 중 추적하지 않는 파일이 있다. "변경 버리고 이동"으로도
        // 풀리지 않으므로 대화 상자가 알려야 한다.
        bool untracked_files { false };
        // conflict: 멈춘 작업의 종류다. 이어서 낼 continue·abort의 종류가 여기서 정해진다.
        repository_operation_state sequence { repository_operation_state::none };
    };

    // contract의 operation_status다. 저장소 세션이 정본을 갖고 frame은 이 값을 읽는다.
    struct operation_status
    {
        operation_id id { 0 };
        operation_kind kind { operation_kind::refresh };
        operation_state state { operation_state::queued };
        // 저장소 operation이면 저장소 id, clone·init·inspect_path면 비어 있다.
        std::string repository_id {};
        // clone·init·inspect_path의 대상 경로다. 실패는 이 경로를 연 대화 상자가 보인다.
        std::u8string target_path {};
        // 같은 쓰기 lane에서 이 작업 앞에 있던 operation이다. 상태 줄이 "무엇을 기다리는지" 보인다.
        std::optional<operation_id> queued_behind {};
        operation_progress progress {};
        std::vector<operation_command> commands {};
        std::optional<operation_failure> failure {};
        std::optional<input_request> needs_input {};
        // 성공 알림에 보일 한 줄이다.
        std::u8string result_summary {};
        // 사용자가 직접 낸 것이 아니라 쓰기 뒤 자동 갱신처럼 세션이 낸 작업이다. 작업 로그가 거를 수 있다.
        bool automatic { false };
        // 대기·실행 중이면 취소할 수 있다. 충돌 대기(needs_input)는 취소가 아니라 abort로만 빠져나간다.
        bool cancellable { true };
        bool cancel_requested { false };
        std::chrono::system_clock::time_point submitted_at {};
        std::chrono::system_clock::time_point started_at {};
        std::chrono::system_clock::time_point finished_at {};

        [[nodiscard]] bool finished() const noexcept;
    };

    [[nodiscard]] std::string_view operation_kind_name(operation_kind kind) noexcept;
    [[nodiscard]] std::string_view operation_state_name(operation_state state) noexcept;
    [[nodiscard]] std::string_view failure_kind_name(failure_kind kind) noexcept;
    [[nodiscard]] std::string_view input_kind_name(input_kind kind) noexcept;
    // 실패 배너의 기본 문구다. 실행기가 더 구체적인 문구를 만들 수 없을 때 쓴다.
    [[nodiscard]] std::u8string_view failure_kind_message(failure_kind kind) noexcept;
} // namespace cyella
