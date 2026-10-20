#pragma once

#include "domain/commit_signature.h"
#include "domain/signing_config.h"

#include <string>

namespace cyella {
    // 커밋 서명을 화면에 보이는 값이다. 배지의 색 느낌은 환경 설정 화면의 `tone`과 같은 네 가지다.
    enum class signature_tone
    {
        neutral,
        good,
        warning,
        bad,
    };

    [[nodiscard]] signature_tone signature_tone_of(signature_status status) noexcept;

    // 상세 패널의 서명 줄이다.
    struct signature_view
    {
        // 세션이 이 커밋의 서명을 읽어 두었다. 거짓이면 아직 확인 중이다.
        bool known { false };
        signature_status status { signature_status::none };
        signature_tone tone { signature_tone::neutral };
        // "서명 확인됨", "확인할 수 없음 (allowed signers 없음)" 같은 배지 글이다.
        std::u8string badge {};
        // 이유를 담은 한 문장이다. 서명이 없으면 비어 있다.
        std::u8string description {};
        // `%GS`와 `%GK`다. 모르면 비어 있다.
        std::u8string signer {};
        std::u8string key {};
    };

    // 읽은 값이 없으면(null) 확인 중인 줄이다.
    [[nodiscard]] signature_view make_signature_view(const commit_signature* signature);

    // 커밋 상자의 서명 토글 아래에 붙는 안내다. 서명이 켜져 있을 때만 의미가 있고, 서명할 수 없는 설정이면 이유를 알린다. 비어 있으면 할 말이 없다.
    [[nodiscard]] std::u8string signing_setup_hint(const signing_config& config, bool signing_enabled);
} // namespace cyella
