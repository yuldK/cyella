#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 서명과 관련된 git 설정이다. 환경 설정의 "서명" 화면과 커밋 상자의 기본값이 읽는다.

    enum class signing_key_id : std::uint8_t
    {
        commit_gpgsign,
        tag_gpgsign,
        gpg_format,
        user_signingkey,
        gpg_program,
        gpg_ssh_program,
        gpg_ssh_allowed_signers,
    };

    inline constexpr std::size_t signing_key_count { 7 };

    // `git config`의 키 이름이다 (`commit.gpgsign`).
    [[nodiscard]] std::string_view signing_key_name(signing_key_id key) noexcept;

    // 값이 어느 설정 파일에서 왔는가. `git config --show-scope`의 낱말이다.
    enum class config_origin : std::uint8_t
    {
        unset,
        system,
        global,
        local,
        worktree,
        command,
    };

    [[nodiscard]] std::u8string config_origin_label(config_origin origin);

    enum class signing_format : std::uint8_t
    {
        openpgp,
        ssh,
    };

    [[nodiscard]] std::string_view signing_format_name(signing_format format) noexcept;
    // `openpgp`·`ssh`만 알아본다. 다른 값(`x509`)이나 빈 값은 값이 없다.
    [[nodiscard]] bool parse_signing_format(std::string_view text, signing_format& format) noexcept;

    // 설정 하나의 값이다. 값이 없는 설정(`[tag] gpgsign`처럼 `=`가 없는 불리언)은 `valueless`가 참이고 `value`는 비어 있다.
    struct signing_entry
    {
        std::string value {};
        config_origin origin { config_origin::unset };
        bool valueless { false };

        [[nodiscard]] bool is_set() const noexcept
        {
            return origin != config_origin::unset;
        }

        [[nodiscard]] bool operator==(const signing_entry&) const noexcept = default;
    };

    // 한 층(실효 값, 지역, 전역)의 서명 설정이다.
    struct signing_config
    {
        std::array<signing_entry, signing_key_count> entries {};

        [[nodiscard]] signing_entry& at(const signing_key_id key) noexcept
        {
            return entries[static_cast<std::size_t>(key)];
        }

        [[nodiscard]] const signing_entry& at(const signing_key_id key) const noexcept
        {
            return entries[static_cast<std::size_t>(key)];
        }

        // `commit.gpgsign`·`tag.gpgsign`을 불리언으로 읽는다. 없거나 알아볼 수 없으면 거짓이다.
        [[nodiscard]] bool commit_signing() const noexcept;
        [[nodiscard]] bool tag_signing() const noexcept;
        // `gpg.format`이다. 없거나 `openpgp`·`ssh`가 아니면 `openpgp`다 (git의 기본값).
        [[nodiscard]] signing_format format() const noexcept;
        // `user.signingkey`가 있다.
        [[nodiscard]] bool has_signing_key() const noexcept;

        [[nodiscard]] bool operator==(const signing_config&) const noexcept = default;
    };

    // git이 쓰는 불리언 낱말이다 (`true`·`yes`·`on`·`1` / `false`·`no`·`off`·`0`). 알아보지 못하면 false를 돌려주고 값은 건드리지 않는다.
    [[nodiscard]] bool parse_config_bool(std::string_view text, bool& value) noexcept;

    // 같은 키 모음을 세 번 읽은 결과다. 실효 값은 git이 정하고, 지역·전역은 각 파일에서 읽는다.
    struct signing_config_layers
    {
        signing_config effective {};
        signing_config local {};
        signing_config global {};

        [[nodiscard]] bool operator==(const signing_config_layers&) const noexcept = default;
    };
} // namespace cyella
