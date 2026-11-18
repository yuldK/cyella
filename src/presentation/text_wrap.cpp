#include "presentation/text_wrap.h"

#include <algorithm>
#include <string_view>

namespace cyella {
    namespace {
        // UTF-8 한 글자의 byte 수다. 깨진 첫 byte는 한 byte로 본다.
        std::size_t sequence_length(const char8_t lead) noexcept
        {
            if (lead < 0x80u)
                return 1;
            if ((lead & 0xE0u) == 0xC0u)
                return 2;
            if ((lead & 0xF0u) == 0xE0u)
                return 3;
            if ((lead & 0xF8u) == 0xF0u)
                return 4;
            return 1;
        }

        char32_t decode(const std::u8string_view text, const std::size_t at, const std::size_t length) noexcept
        {
            const auto lead { static_cast<char32_t>(text[at]) };
            if (length == 1)
                return lead;
            char32_t value { lead & (0x7Fu >> length) };
            for (std::size_t index { 1 }; index < length && at + index < text.size(); ++index)
                value = (value << 6) | (static_cast<char32_t>(text[at + index]) & 0x3Fu);
            return value;
        }

        constexpr char32_t zero_width_joiner { 0x200Du };

        // 앞 글자에 붙어 자리를 더 차지하지 않는 코드 포인트다: 결합 부호, 한글 중성·종성 자모(NFD), 폭 없는 공백·이음,
        // 변이 선택자, 가나의 탁점, 이모지 피부색 수정자.
        bool is_combining(const char32_t value) noexcept
        {
            return (value >= 0x0300u && value <= 0x036Fu) || (value >= 0x1160u && value <= 0x11FFu) || (value >= 0x200Bu && value <= 0x200Du) || (value >= 0x3099u && value <= 0x309Au)
                || (value >= 0xFE00u && value <= 0xFE0Fu) || (value >= 0x1F3FBu && value <= 0x1F3FFu);
        }

        // `at`에서 시작하는 글자 덩어리의 끝이다. 기본 글자 하나에 결합 코드 포인트가 붙고, ZWJ 뒤의 글자도 한 덩어리다.
        // 덩어리 가운데서 자르면 기본 글자는 줄 끝에, 결합 부분은 다음 줄 맨 앞에 그려진다.
        std::size_t cluster_end(const std::u8string_view text, const std::size_t at, const std::size_t end) noexcept
        {
            std::size_t position { std::min(at + sequence_length(text[at]), end) };
            char32_t last { decode(text, at, position - at) };
            while (position < end)
            {
                const std::size_t length { std::min(sequence_length(text[position]), end - position) };
                const char32_t value { decode(text, position, length) };
                if (is_combining(value) == false && last != zero_width_joiner)
                    break;
                position += length;
                last = value;
            }
            return position;
        }

        // 글자 하나의 짐작 폭이다 (em). 맑은 고딕·Segoe UI보다 조금 넓다.
        float advance_of(const char32_t value) noexcept
        {
            if (is_combining(value))
                return 0.0f;
            if (value >= 0x1100u)
                return 1.0f;
            if (value >= 0x80u)
                return 0.62f;
            switch (value)
            {
            case U' ':
            case U'i':
            case U'l':
            case U'.':
            case U',':
            case U':':
            case U';':
            case U'\'':
            case U'|':
            case U'!':
            case U'`':
                return 0.32f;
            case U'f':
            case U'j':
            case U'r':
            case U't':
            case U'I':
            case U'(':
            case U')':
            case U'[':
            case U']':
            case U'-':
                return 0.42f;
            case U'm':
            case U'w':
            case U'M':
            case U'W':
            case U'@':
                return 0.92f;
            default:
                break;
            }
            if (value >= U'A' && value <= U'Z')
                return 0.68f;
            return 0.58f;
        }

        // 줄 하나를 정한다. 다음 줄이 시작하는 자리를 `next`로 돌려준다.
        text_line next_line(const std::u8string_view text, const std::size_t begin, const std::size_t end, const float width, const text_measure& measure, std::size_t& next)
        {
            std::size_t fit_end { std::u8string_view::npos };
            std::size_t fit_next { std::u8string_view::npos };
            std::size_t position { begin };
            while (position < end)
            {
                std::size_t word_end { position };
                while (word_end < end && text[word_end] != u8' ')
                    ++word_end;
                std::size_t after { word_end };
                while (after < end && text[after] == u8' ')
                    ++after;
                if (measure(text.substr(begin, word_end - begin)) > width)
                    break;
                fit_end = word_end;
                fit_next = after;
                position = after;
            }
            if (fit_end != std::u8string_view::npos)
            {
                next = fit_next;
                return { begin, fit_end };
            }
            // 첫 단어부터 넘친다. 그 단어를 글자 단위로 자른다 (적어도 한 글자는 담는다). 결합 문자는 기본 글자와 한 덩어리다.
            std::size_t cut { cluster_end(text, begin, end) };
            while (cut < end)
            {
                const std::size_t step { cluster_end(text, cut, end) - cut };
                if (text[cut] == u8' ' || measure(text.substr(begin, cut + step - begin)) > width)
                    break;
                cut += step;
            }
            cut = std::min(cut, end);
            next = cut;
            return { begin, cut };
        }
    } // namespace

    std::vector<text_line> wrap_text_lines(const std::u8string_view text, const float width, const text_measure& measure)
    {
        std::vector<text_line> lines {};
        std::size_t start { 0 };
        while (start <= text.size())
        {
            std::size_t stop { text.find(u8'\n', start) };
            const bool last { stop == std::u8string_view::npos };
            if (last)
                stop = text.size();
            std::size_t paragraph_end { stop };
            if (paragraph_end > start && text[paragraph_end - 1] == u8'\r')
                --paragraph_end;
            if (paragraph_end == start || width <= 0.0f)
                lines.push_back({ start, paragraph_end });
            else
            {
                std::size_t begin { start };
                while (begin < paragraph_end)
                {
                    std::size_t next { paragraph_end };
                    lines.push_back(next_line(text, begin, paragraph_end, width, measure, next));
                    begin = next;
                }
            }
            if (last)
                break;
            start = stop + 1;
        }
        return lines;
    }

    float estimate_text_width(const std::u8string_view text, const float size, const bool bold) noexcept
    {
        float em { 0.0f };
        for (std::size_t at { 0 }; at < text.size();)
        {
            const std::size_t length { sequence_length(text[at]) };
            em += advance_of(decode(text, at, length));
            at += length;
        }
        return em * size * (bold ? 1.06f : 1.0f);
    }

    std::size_t estimate_wrapped_line_count(const std::u8string_view text, const float width, const float size, const bool bold)
    {
        if (text.empty())
            return 0;
        return wrap_text_lines(text, width, [size, bold](const std::u8string_view part) { return estimate_text_width(part, size, bold); }).size();
    }
} // namespace cyella
