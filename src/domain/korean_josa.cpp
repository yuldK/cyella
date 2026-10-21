#include "domain/korean_josa.h"

#include <cstdint>

namespace cyella {
    namespace {
        // 끝의 UTF-8 글자 하나를 코드 포인트로 푼다. 못 풀면 0이다.
        char32_t last_code_point(const std::u8string_view text) noexcept
        {
            if (text.empty())
                return 0;
            std::size_t start { text.size() - 1 };
            while (start > 0 && (static_cast<std::uint8_t>(text[start]) & 0xC0u) == 0x80u)
                --start;
            const auto lead { static_cast<std::uint8_t>(text[start]) };
            const std::size_t length { text.size() - start };
            if (length == 1)
                return lead;
            char32_t value {};
            if (length == 2)
                value = lead & 0x1Fu;
            else if (length == 3)
                value = lead & 0x0Fu;
            else
                value = lead & 0x07u;
            for (std::size_t index { start + 1 }; index < text.size(); ++index)
                value = (value << 6) | (static_cast<std::uint8_t>(text[index]) & 0x3Fu);
            return value;
        }

        bool is_hangul_syllable(const char32_t value) noexcept
        {
            return value >= 0xAC00 && value <= 0xD7A3;
        }
    } // namespace

    bool ends_with_batchim(const std::u8string_view text) noexcept
    {
        const char32_t last { last_code_point(text) };
        if (is_hangul_syllable(last))
            return (last - 0xAC00) % 28 != 0;
        // 숫자는 일·삼·육·칠·팔·영이 받침으로 끝난다.
        if (last == U'0' || last == U'1' || last == U'3' || (last >= U'6' && last <= U'8'))
            return true;
        return last == U'n' || last == U'm' || last == U'l' || last == U'N' || last == U'M' || last == U'L';
    }

    bool ends_with_rieul(const std::u8string_view text) noexcept
    {
        const char32_t last { last_code_point(text) };
        if (is_hangul_syllable(last))
            return (last - 0xAC00) % 28 == 8;
        return last == U'l' || last == U'L';
    }
} // namespace cyella
