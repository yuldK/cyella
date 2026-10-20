#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cyella {
    // 명령 팔레트의 퍼지 일치 규칙이다. 한글은 음절 부분 문자열과 초성(ㅂㄹㅊ → 브랜치)으로, 영문은 ASCII 대소문자를
    // 무시한 부분 문자열로 찾고, 연속으로 맞지 않으면 건너뛰며 맞는 것(부분 수열)도 낮은 점수로 받아들인다.
    // 순수 함수이고 UTF-8 입력만 읽는다.

    // 한글 음절(가~힣)인가.
    [[nodiscard]] bool is_hangul_syllable(char32_t value) noexcept;
    // 호환 자모의 초성 열아홉 자(ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ) 중 하나인가.
    [[nodiscard]] bool is_choseong_jamo(char32_t value) noexcept;
    // 음절의 초성을 호환 자모로 돌려준다. 음절이 아니면 값이 없다.
    [[nodiscard]] std::optional<char32_t> choseong_of(char32_t syllable) noexcept;
    // UTF-8을 코드 포인트로 풀어 쓴다. 잘못된 바이트는 U+FFFD가 된다.
    [[nodiscard]] std::u32string decode_utf8(std::u8string_view text);

    // 질의 한 토큰(공백 없음)이 글에서 맞는 정도다. 클수록 좋고 맞지 않으면 값이 없다.
    //  - 연속으로 맞는 것이 가장 높고 글머리·낱말 머리에서 맞으면 더한다.
    //  - 질의의 글자 하나는 같은 글자, 같은 초성의 음절(질의가 초성 자모일 때)과 맞는다.
    //  - 질의의 마지막 글자가 받침 없는 음절이면 같은 초성·중성의 음절은 받침과 관계없이 맞는다 (IME가 조합 중인 글자).
    [[nodiscard]] std::optional<int> fuzzy_token_score(std::u32string_view query, std::u32string_view text);

    // 공백으로 가른 질의의 모든 토큰이 제목이나 키워드에서 맞아야 한다. 제목에서 맞는 것이 키워드에서 맞는 것보다 높다.
    // 질의가 비어 있으면 0이다.
    [[nodiscard]] std::optional<int> fuzzy_match_score(std::u8string_view query, std::u8string_view title, std::u8string_view keywords);
} // namespace cyella
