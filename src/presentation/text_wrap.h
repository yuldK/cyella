#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 글 한 구간의 폭을 재는 함수다. 그릴 때는 실제 글꼴로, 배치할 때는 `estimate_text_width`로 잰다.
    using text_measure = std::function<float(std::u8string_view)>;

    // 원문 안의 한 줄이다 (`[begin, end)` byte). 줄 끝의 공백과 줄바꿈은 들지 않는다.
    struct text_line
    {
        std::size_t begin { 0 };
        std::size_t end { 0 };

        [[nodiscard]] bool operator==(const text_line&) const noexcept = default;
    };

    // 글을 `width` 안에 들도록 줄로 나눈다. 앞에서부터 한 줄에 들어가는 만큼 담는다.
    //  - `\n`은 언제나 줄을 바꾼다 (`\r`은 버린다). 빈 줄도 한 줄이다.
    //  - 공백에서만 나눈다 (CSS `word-break: keep-all`과 같다). 한국어 문장도 어절이 깨지지 않는다.
    //  - 한 단어가 한 줄보다 길면(URL, 긴 경로) 그 단어만 글자 단위로 나눈다.
    //  - 줄 앞의 공백은 남긴다. 본문의 `  - 항목` 같은 들여쓰기가 그대로 보인다.
    //  - `width`가 0 이하면 나누지 않는다 (문단마다 한 줄).
    [[nodiscard]] std::vector<text_line> wrap_text_lines(std::u8string_view text, float width, const text_measure& measure);

    // 배치용 폭 짐작이다 (논리 픽셀). 측정 단계가 없어 글꼴 없이 글자 종류로 잰다.
    //  - 실제보다 **조금 넓게** 잰다. 넓게 재면 줄이 하나 남아 아래가 조금 빌 뿐이지만, 좁게 재면 마지막 줄이 잘린다.
    [[nodiscard]] float estimate_text_width(std::u8string_view text, float size, bool bold) noexcept;

    // 배치할 때 잡을 줄 수다. 그리는 쪽과 같은 나누기 규칙을 짐작 폭으로 돌린다. 빈 글은 0이다.
    [[nodiscard]] std::size_t estimate_wrapped_line_count(std::u8string_view text, float width, float size, bool bold);
} // namespace cyella
