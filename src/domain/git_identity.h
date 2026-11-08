#pragma once

#include "domain/user_profile.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 저장소에서 git이 실제로 쓰는 작성자 신원(`user.name`·`user.email`·`user.signingkey`)과 접속 계정(`credential.username`·`core.sshCommand`),
    // 그리고 그 값이 어느 설정 파일에서 왔는지다. `git config --show-scope --show-origin --get-regexp`의 출력을 읽는다. 전역 설정은 건드리지 않고 읽기만 한다.
    // URL을 붙인 `credential.<url>.username`은 읽지 않는다 (프로필이 쓰는 것은 URL 없는 키다).

    enum class identity_origin
    {
        // 저장소의 `.git/config`다.
        local,
        // `.git/config.worktree`다 (worktree별 설정).
        worktree,
        // 사용자 전역 설정(`~/.gitconfig`, `$XDG_CONFIG_HOME/git/config`, `GIT_CONFIG_GLOBAL`)이다.
        global,
        // 전역 설정이 `include`·`includeIf`로 끌어온 다른 파일이다 (예: 폴더별 `.gitconfig-work`).
        include,
        system,
        // `-c`나 환경 변수로 준 값이다.
        command_line,
        unknown,
    };

    struct identity_value
    {
        std::u8string value {};
        identity_origin origin { identity_origin::unknown };
        // 값이 든 설정 파일이다 (`file:` 접두를 뗀 git의 표기 그대로). 저장소 설정은 상대 경로(`.git/config`)로 올 수 있다.
        std::u8string file {};

        [[nodiscard]] bool operator==(const identity_value&) const noexcept = default;
    };

    // 읽은 신원이다. 값이 여러 파일에 있으면 git이 실제로 쓰는 마지막 값(저장소 설정이 전역을 이긴다)이다.
    struct git_identity
    {
        std::optional<identity_value> name {};
        std::optional<identity_value> email {};
        std::optional<identity_value> signing_key {};
        // HTTPS 접속에 자격 증명 도우미에 넘길 계정 이름이다. 원격 URL에 사용자(`A@host`)가 있으면 그쪽이 이긴다.
        std::optional<identity_value> credential_username {};
        // git이 ssh 대신 부르는 명령이다. 프로필이 쓴 꼴이면 `profile_ssh_key_of`가 키 경로를 돌려준다.
        std::optional<identity_value> ssh_command {};

        // 커밋하려면 이름과 이메일이 모두 있어야 한다.
        [[nodiscard]] bool complete() const noexcept
        {
            return name.has_value() && email.has_value() && name->value.empty() == false && email->value.empty() == false;
        }

        [[nodiscard]] bool operator==(const git_identity&) const noexcept = default;
    };

    // 저장소의 로컬 설정(`git config --local`)에 할 일이다. 프로필 적용과 "로컬 설정 지우기"가 만든다.
    struct local_identity_change
    {
        // 참이면 앱이 쓰는 키를 저장소에서 지워 전역 신원과 계정으로 돌아간다. 나머지 필드는 보지 않는다.
        bool clear { false };
        std::u8string user_name {};
        std::u8string user_email {};
        // 비어 있으면 저장소의 `credential.username`을 지운다.
        std::u8string credential_username {};
        // 비어 있으면 저장소의 `core.sshCommand`가 프로필이 쓴 꼴일 때만 지운다 (손으로 쓴 명령은 둔다).
        std::u8string ssh_command {};

        [[nodiscard]] bool operator==(const local_identity_change&) const noexcept = default;
    };

    // 전역 설정 파일이 어느 경로인지 알려 `include` 파일과 가른다. 비어 있으면 `global` 범위를 모두 전역으로 본다.
    struct identity_origin_hints
    {
        std::vector<std::u8string> global_files {};
    };

    [[nodiscard]] git_identity parse_git_identity(std::u8string_view output, const identity_origin_hints& hints = {});

    [[nodiscard]] std::u8string identity_origin_label(identity_origin origin);
    // `홍길동 <hong@example.com>` 꼴이다. 둘 다 없으면 "설정되지 않음"이다.
    [[nodiscard]] std::u8string describe_identity(const git_identity& identity);
    // 프로필과 같은 신원인가. 이름·이메일은 같아야 하고, 접속 계정(HTTPS 사용자·SSH 키)은 프로필에 적혀 있을 때만 맞춰 본다.
    [[nodiscard]] bool identity_matches_profile(const git_identity& identity, const user_profile& profile);

    // 프로필의 SSH 키로 접속하는 `core.sshCommand` 값이다: `'<ssh>' -i '<키>' -o IdentitiesOnly=yes`.
    // git이 sh로 해석하므로 경로는 슬래시로 바꿔 작은따옴표로 감싼다. `IdentitiesOnly`는 ssh-agent에 올린 다른 계정의 키를 먼저 내밀지 않게 한다.
    [[nodiscard]] std::u8string make_profile_ssh_command(std::u8string_view ssh_executable, std::u8string_view key_path);
    // `make_profile_ssh_command`가 만든 꼴이면 키 경로다. 사용자가 손으로 쓴 다른 명령이면 값이 없다 (앱이 지우지 않는다).
    [[nodiscard]] std::optional<std::u8string> profile_ssh_key_of(std::u8string_view ssh_command);
    // 신원의 `core.sshCommand`가 프로필이 쓴 꼴이면 그 키 경로다.
    [[nodiscard]] std::optional<std::u8string> profile_ssh_key_of(const git_identity& identity);
    // 같은 키 파일인가 (구분자·대소문자를 가리지 않는다).
    [[nodiscard]] bool same_key_path(std::u8string_view left, std::u8string_view right);
    // 접속 계정을 짧게 말한다: `접속 B` / `키 id_work` / 둘 다. 저장소에 아무것도 없으면 빈 글이다.
    [[nodiscard]] std::u8string describe_access(const git_identity& identity);
    // `describe_identity_with_source` 뒤에 접속 계정을 잇는다 (`홍길동 <a@b> · 로컬 · 접속 B`). 상태 줄과 환경 설정이 쓴다.
    [[nodiscard]] std::u8string describe_identity_with_access(const git_identity& identity);
    // `홍길동 <hong@example.com> · 로컬`처럼 신원과 출처를 한 줄로 말한다. 이름과 이메일이 서로 다른 파일에서 왔으면 각각 말한다
    // (`… · 이름 전역 · 이메일 includeIf`).
    [[nodiscard]] std::u8string describe_identity_with_source(const git_identity& identity);
} // namespace cyella
