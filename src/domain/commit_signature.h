#pragma once

#include <string>
#include <string_view>

namespace cyella {
    // 커밋 서명의 검증 결과다. git의 `%G?` 글자와 1:1이다 (G·B·U·X·Y·R·E·N).
    enum class signature_status
    {
        // N. 서명이 없다.
        none,
        // G. 올바른 서명이다.
        good,
        // B. 서명이 올바르지 않다 (내용이 바뀌었거나 깨졌다).
        bad,
        // U. 서명은 올바르지만 서명자를 알 수 없다. gpg는 신뢰 정도를 모르는 키, ssh는 allowed signers에 없는 키다.
        unknown_key,
        // X. 서명이 만료되었다.
        expired_signature,
        // Y. 서명한 키가 만료되었다.
        expired_key,
        // R. 서명한 키가 폐기되었다.
        revoked_key,
        // E. 서명을 확인할 수 없다 (공개 키가 없거나 gpg를 실행하지 못했다).
        cannot_check,
    };

    // 확인할 수 없는 이유 중 화면이 따로 알려 줄 것이다.
    enum class signature_note
    {
        none,
        // ssh 서명인데 `gpg.ssh.allowedSignersFile`이 없거나 가리키는 파일이 없다. 서명자를 대조할 목록이 없다.
        allowed_signers_missing,
    };

    // 커밋 하나의 서명 정보다. 서명이 없는 커밋도 `none`으로 담는다 (읽었다는 사실이 값이다).
    struct commit_signature
    {
        std::string sha {};
        signature_status status { signature_status::none };
        // `%GS`. gpg는 사용자 ID, ssh는 allowed signers의 principal이다. 모르면 비어 있다.
        std::u8string signer {};
        // `%GK`. gpg는 키 ID, ssh는 `SHA256:...` 지문이다.
        std::u8string key {};
        signature_note note { signature_note::none };

        [[nodiscard]] bool operator==(const commit_signature&) const noexcept = default;
    };

    // `%G?` 글자를 읽는다. 모르는 글자는 `cannot_check`다.
    [[nodiscard]] signature_status signature_status_from_code(char code) noexcept;
    [[nodiscard]] char signature_status_code(signature_status status) noexcept;
    [[nodiscard]] std::string_view signature_status_name(signature_status status) noexcept;

    // 서명이 있는 커밋인가 (`none`이 아니다).
    [[nodiscard]] bool is_signed(signature_status status) noexcept;

    // 배지에 쓰는 짧은 글이다 ("서명 확인됨", "알 수 없는 키").
    [[nodiscard]] std::u8string signature_badge_text(signature_status status);
    // 배지 아래에 붙는 설명 한 문장이다. `note`를 반영한다. 서명이 없으면 비어 있다. 서명자와 키는 따로 보인다(`commit_signature::signer`·`key`).
    [[nodiscard]] std::u8string signature_description(const commit_signature& signature);
    // `note`가 있으면 배지 글이 달라진다 ("확인할 수 없음 (allowed signers 없음)").
    [[nodiscard]] std::u8string signature_badge_label(const commit_signature& signature);
} // namespace cyella
