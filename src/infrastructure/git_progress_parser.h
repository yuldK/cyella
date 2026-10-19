#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cyella {
    // `--progress`로 켠 git의 stderr 한 줄에서 읽은 진행 상태다.
    struct git_progress
    {
        // `Receiving objects`처럼 단계 이름만 담는다. `remote: ` 접두어와 퍼센트·속도는 뺀다.
        std::u8string phase_text {};
        // 0..1이다. 전체 크기를 알 수 없는 단계(`Enumerating objects: 5`)는 값이 없다.
        std::optional<double> fraction {};
        // 서버가 sideband로 보낸 줄(`remote: ` 접두어)인지 여부다.
        bool remote { false };
    };

    // 순수 함수다. 진행 줄이 아니면 값이 없다. 줄 끝의 `\r`·`\n`은 있어도 무시한다.
    //
    // 오류·안내 문구(`fatal: ...`, `From ...`)를 진행으로 오인하지 않도록 구조로 판정한다.
    // 단계 이름은 대문자로 시작하고 글자와 공백만 있어야 하며, 뒤에는 `N%`로 시작하거나
    // 숫자만(선택적으로 `, done.`) 있어야 한다. 로캘에 따라 단계 이름이 번역되더라도
    // 이 구조는 같다.
    [[nodiscard]] std::optional<git_progress> parse_git_progress(std::u8string_view line);
} // namespace cyella
