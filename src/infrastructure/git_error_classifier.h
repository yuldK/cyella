#pragma once

#include <cstdint>
#include <optional>
#include <string>
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
        // push 거절 중 원격에 내게 없는 커밋이 있는 경우다(`(fetch first)`). 먼저 가져오면 된다.
        push_rejected_fetch_first,
        // `--force-with-lease`의 기대값이 원격과 달라 거절되었다(`(stale info)`).
        push_rejected_stale_info,
        // known_hosts의 키와 서버 키가 다르다. ssh는 묻지 않고 실패하므로 askpass를 거치지 않는다.
        host_key_changed,
        // 호스트 이름은 찾았지만 연결하지 못했다(거부, 시간 초과, 경로 없음).
        connection_failed,
        // `merge --ff-only`·`pull --ff-only`가 fast-forward로 옮길 수 없다.
        not_possible_to_fast_forward,
        // 브랜치가 다른 worktree에 체크아웃되어 있다(`'x' is already used by worktree at '<경로>'`).
        branch_checked_out_elsewhere,
        // 커밋·태그 서명이 실패했다. 아래 둘로 가르지 못한 경우(서명 프로그램을 실행하지 못했다 등)다.
        signing_failed,
        // 서명할 키가 없다 (gpg의 `No secret key`, ssh의 `Couldn't load public key`, `user.signingkey`가 비었다).
        signing_key_missing,
        // 키의 passphrase를 받지 못했다 (gpg의 `No pinentry`·`Bad passphrase`·`Operation cancelled`, ssh-keygen의 `incorrect passphrase`).
        signing_passphrase_failed,
    };

    // `@@@ WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED! @@@` 뒤의 `Offending ... key in
    // <path>:<line>` 줄이 가리키는 known_hosts 위치다. 대화 상자가 지울 줄을 보인다.
    struct known_hosts_location
    {
        std::string path {};
        std::uint32_t line { 0 };
    };

    // OpenSSH가 만든 문구라 번역되지 않는다. 바뀐 호스트 키 경고가 아니면 값이 없다. 경고는
    // 있는데 위치 줄을 읽지 못하면 빈 위치를 돌려준다.
    [[nodiscard]] std::optional<known_hosts_location> find_offending_known_hosts_line(std::string_view standard_error);

    // `branch_checked_out_elsewhere` 실패의 stderr에서 브랜치가 체크아웃된 worktree 폴더를 읽는다.
    // `at '<경로>'`(새 git) 또는 `checked out at '<경로>'`(옛 git)의 따옴표 안이다. 읽지 못하면 값이 없다.
    [[nodiscard]] std::optional<std::string> find_checked_out_worktree_path(std::string_view standard_error);

    // 서명 실패(`signing_*`)의 stderr가 gpg에서 온 것인가. 아니면 ssh-keygen(ssh 서명)이다. 안내 문구가 gpg-agent·pinentry와
    // ssh-agent·passphrase 중 어느 쪽을 가리킬지 정하는 데 쓴다. gpg는 git이 `--status-fd=2`로 부르므로 `[GNUPG:]` 줄이 있다.
    [[nodiscard]] bool signing_failure_is_from_gpg(std::string_view standard_error) noexcept;

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
    [[nodiscard]] std::u8string git_failure_message(git_failure_kind kind);
} // namespace cyella
