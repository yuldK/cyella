#pragma once

#include "domain/user_profile.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 저장소에서 git이 실제로 쓰는 작성자 신원(`user.name`·`user.email`·`user.signingkey`)과 그 값이 어느 설정 파일에서 왔는지다.
    // `git config --show-scope --show-origin --get-regexp '^user\.(name|email|signingkey)$'`의 출력을 읽는다. 전역 설정은 건드리지 않고 읽기만 한다.

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

        // 커밋하려면 이름과 이메일이 모두 있어야 한다.
        [[nodiscard]] bool complete() const noexcept
        {
            return name.has_value() && email.has_value() && name->value.empty() == false && email->value.empty() == false;
        }

        [[nodiscard]] bool operator==(const git_identity&) const noexcept = default;
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
    // 프로필의 이름·이메일과 같은 신원인가.
    [[nodiscard]] bool identity_matches_profile(const git_identity& identity, const user_profile& profile) noexcept;
    // `홍길동 <hong@example.com> · 로컬`처럼 신원과 출처를 한 줄로 말한다. 이름과 이메일이 서로 다른 파일에서 왔으면 각각 말한다
    // (`… · 이름 전역 · 이메일 includeIf`).
    [[nodiscard]] std::u8string describe_identity_with_source(const git_identity& identity);
} // namespace cyella
