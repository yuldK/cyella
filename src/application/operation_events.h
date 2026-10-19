#pragma once

#include "application/askpass_service.h"
#include "application/git_operation.h"
#include "domain/commit_info.h"
#include "domain/credential_prompt.h"
#include "domain/diff_file.h"
#include "domain/file_change.h"
#include "domain/operation_status.h"
#include "domain/ref.h"
#include "domain/repository_summary.h"
#include "domain/stash_entry.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cyella {
    // worker → logic thread로 가는 사건이다. 모두 `operation_event`에 담겨 relay를 건너고,
    // logic thread의 `git_workspace::apply`가 상태에 반영한다. worker는 상태를 직접 고치지
    // 않는다. 한 operation의 사건은 한 worker가 순서대로 내므로 도착 순서가 곧 발생 순서다.

    // job이 worker에서 실행을 시작했다(queued → running).
    struct operation_started
    {};

    struct operation_progressed
    {
        operation_progress progress {};
    };

    // git 명령 하나가 끝났다. 작업 로그의 한 줄이다.
    struct operation_command_finished
    {
        operation_command command {};
    };

    // git(또는 ssh)이 askpass 도우미로 사용자에게 물었다. 도우미가 답을 기다리는 동안 git도 멈춰
    // 있으므로 operation은 끝나지 않고 `needs_input{passphrase|host_key|credentials}`가 된다.
    // 이 사건은 askpass 서버의 연결 스레드가 relay로 보낸다. `id`는 도우미가 환경 변수로 받은
    // operation id다.
    //
    // `reply`는 서버가 만든 일회용 답 callback이다. 처음 부른 답만 효과가 있고 서버가 끝났거나
    // 도우미가 사라졌으면 조용히 무시된다. 사건을 받은 쪽은 반드시 한 번은 불러야 한다. 부르지
    // 않으면 도우미가 operation이 끝날 때까지 매달린다.
    struct operation_input_requested
    {
        std::uint64_t prompt_id { 0 };
        credential_prompt prompt {};
        askpass_reply_callback reply {};
    };

    // refresh가 읽은 저장소 상태 전체다. 같은 generation의 log 묶음보다 먼저 온다.
    struct repository_snapshot
    {
        repository_summary summary {};
        std::vector<ref> refs {};
        std::vector<stash_entry> stashes {};
        std::vector<file_change> staged {};
        std::vector<file_change> unstaged {};
        std::vector<file_change> conflicts {};
    };

    struct snapshot_loaded
    {
        std::uint64_t generation { 0 };
        repository_snapshot snapshot {};
    };

    // log 스트림의 한 묶음이다. 약 2000개나 50 ms마다 끊어 보낸다.
    struct commits_loaded
    {
        std::uint64_t generation { 0 };
        // 이 묶음의 첫 커밋이 전체 log에서 몇 번째인지다(WIP 행은 세지 않는다). 세션이 받은 수와
        // 다르면 쪽이 어긋난 것이라 버린다.
        std::size_t first_index { 0 };
        std::vector<commit_info> commits {};
        // 이 쪽의 마지막 묶음이다.
        bool page_complete { false };
        // log의 끝에 닿았다. 더 읽을 쪽이 없다.
        bool history_complete { false };
    };

    // 원격 하나의 `ls-remote --tags` 결과다.
    struct remote_tags_loaded
    {
        std::string remote {};
        std::vector<remote_tag> tags {};
    };

    struct commit_detail
    {
        commit_info commit {};
        // 첫 부모와 비교한 변경 파일이다. 줄 수는 numstat으로 채운다.
        std::vector<file_change> files {};
    };

    struct commit_detail_loaded
    {
        commit_detail detail {};
    };

    struct file_diff
    {
        diff_file_request request {};
        std::vector<diff_file> files {};
    };

    struct file_diff_loaded
    {
        file_diff diff {};
    };

    struct ancestry_answered
    {
        std::string ancestor {};
        std::string descendant {};
        bool is_ancestor { false };
    };

    struct commit_count_answered
    {
        std::string from {};
        std::string to {};
        std::uint32_t count { 0 };
    };

    enum class path_inspection_kind
    {
        missing,
        // 일반 폴더다. init이나 clone 대상이 될 수 있다.
        directory,
        // 저장소 루트다.
        repository,
        // 저장소 안쪽 폴더다. UI는 루트로 바꿔 열자고 제안한다.
        inside_repository,
    };

    struct path_inspected
    {
        std::u8string path {};
        path_inspection_kind kind { path_inspection_kind::missing };
        // repository·inside_repository일 때 작업 트리 루트다.
        std::u8string root {};
    };

    // 마지막 사건이다. 반드시 한 번 온다. state는 succeeded·failed·cancelled·needs_input이다.
    struct operation_finished
    {
        operation_state state { operation_state::succeeded };
        std::optional<operation_failure> failure {};
        std::optional<input_request> needs_input {};
        std::u8string summary {};
    };

    using operation_event_payload = std::variant<operation_started, operation_progressed, operation_command_finished, operation_input_requested, snapshot_loaded, commits_loaded, remote_tags_loaded,
        commit_detail_loaded, file_diff_loaded, ancestry_answered, commit_count_answered, path_inspected, operation_finished>;

    struct operation_event
    {
        operation_id id { 0 };
        // 저장소 작업이면 저장소 id, clone·init·inspect_path면 비어 있다.
        std::string repository_id {};
        operation_event_payload payload {};
    };

    // worker가 사건을 내보내는 길이다. 어느 스레드에서나 불릴 수 있어야 한다(relay의 post).
    using operation_event_sink = std::function<void(operation_event)>;
} // namespace cyella
