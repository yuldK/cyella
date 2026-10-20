#pragma once

namespace cyella {
    // 커밋 서명의 화면 메시지다. 환경 설정 "서명" 화면의 메시지는 `presentation/settings/signing_intents.h`다.

    // 커밋 상자의 "서명" 토글을 눌렀다. 켜면 `commit -S`, 설정(`commit.gpgsign`)이 켜져 있는데 끄면 `--no-gpg-sign`이다.
    struct toggle_sign_intent
    {};
} // namespace cyella
