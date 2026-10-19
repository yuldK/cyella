#pragma once

#include <string>
#include <string_view>

namespace cyella {
    inline constexpr std::u8string_view secret_mask { u8"***" };

    // 로그와 기록에 남기기 전에 자격 증명으로 보이는 값을 가린다.
    //
    // 앱은 애초에 자격 증명을 인자로 만들지 않지만, 사용자가 등록한 URL이나 외부
    // 도구의 출력에 비밀이 섞일 수 있으므로 방어 계층을 둔다. 대용량 출력에서
    // 성능과 스택이 문제 되는 `std::regex` 대신 직접 작성한 단일 통과 scanner를 쓴다.
    //
    // 이미 가려진 문자열을 다시 넣어도 결과가 같은 idempotent 연산이다.
    [[nodiscard]] std::u8string mask_secrets(std::u8string_view text);
} // namespace cyella
