#pragma once

#include <string_view>

namespace cyella {
    enum class git_failure_kind
    {
        // 아래 어느 신호에도 맞지 않는다. 추측하지 않고 원문을 그대로 보고한다.
        unclassified,
        authentication_required,
        host_key_verification_failed,
        could_not_resolve_host,
        repository_not_found,
        permission_denied_publickey,
        push_rejected_non_fast_forward,
        // non-fast-forward 말고 원격이 push를 거절했다(stale info, 이미 있는 태그 등).
        push_rejected,
        // 병합·rebase·cherry-pick·revert가 충돌로 멈췄다.
        conflict,
        // 다른 git 프로세스가 index(또는 ref)를 잠그고 있다.
        index_lock_exists,
        // 작업 트리의 변경이 체크아웃·병합으로 덮어써질 뻔했다.
        local_changes_would_be_overwritten,
    };

    // git 실패 출력을 분류한다. 입력은 종료 코드가 0이 아닌 명령의 stderr이며, 병합
    // 충돌처럼 stdout으로 나오는 문구(`CONFLICT (...)`)를 위해 stdout도 받는다.
    //
    // 분류는 best-effort다. git의 메시지는 지역화될 수 있어서, 번역되지 않는 근거를
    // 먼저 쓴다.
    //   - OpenSSH가 만드는 문구(`Host key verification failed.`, `Permission denied
    //     (publickey`)와 libcurl이 만드는 문구(`Could not resolve host`)
    //   - 서버가 돌려주는 문구(`Repository not found`)와 HTTP 상태 번호
    //   - push 결과 표의 `[rejected]` / `[remote rejected]` 표지
    //   - 파일 이름 `index.lock`
    // 번역 대상인 문구(`would be overwritten`, `CONFLICT`, `non-fast-forward` 등)는
    // 영어 출력에서만 보조 신호로 쓴다. 프로세스는 LC_ALL=C로 실행하는 것을 권한다.
    // 그렇게 하면 stderr가 항상 영어가 되어 분류가 안정적이다.
    [[nodiscard]] git_failure_kind classify_git_failure(std::string_view standard_error, std::string_view standard_output = {}) noexcept;

    [[nodiscard]] std::string_view git_failure_kind_name(git_failure_kind kind) noexcept;

    // 사용자에게 보일 한 줄 설명이다.
    [[nodiscard]] std::u8string_view git_failure_message(git_failure_kind kind) noexcept;
} // namespace cyella
