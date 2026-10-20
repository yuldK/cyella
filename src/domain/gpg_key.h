#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cyella {
    // `gpg --list-secret-keys --with-colons`가 알려 주는 비밀 키 하나다. 서명 키를 고르는 목록이 읽는다.
    struct gpg_secret_key
    {
        // 16자리 16진수 긴 키 ID다.
        std::string key_id {};
        // 40자리(또는 그 이상) 16진수 지문이다. `user.signingkey`에 이 값을 쓴다.
        std::string fingerprint {};
        // `ed25519`, `rsa3072`처럼 사람이 읽는 알고리즘 이름이다.
        std::string algorithm {};
        // 유닉스 초다. 모르면 0이다.
        std::int64_t created { 0 };
        // 만료 시각이다. 0이면 만료가 없다.
        std::int64_t expires { 0 };
        bool expired { false };
        bool revoked { false };
        bool disabled { false };
        // 서명할 수 있는 키(주 키 또는 하위 키)다.
        bool can_sign { false };
        std::vector<std::u8string> user_ids {};

        [[nodiscard]] bool operator==(const gpg_secret_key&) const noexcept = default;
    };
} // namespace cyella
