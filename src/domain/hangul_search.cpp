#include "domain/hangul_search.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace cyella {
    namespace {
        constexpr char32_t hangul_syllable_first { 0xAC00 };
        constexpr char32_t hangul_syllable_last { 0xD7A3 };
        constexpr char32_t medial_count { 21 };
        constexpr char32_t final_count { 28 };
        constexpr char32_t syllables_per_initial { medial_count * final_count };

        // 초성 순서대로 호환 자모의 코드 포인트다.
        constexpr std::u32string_view choseong_jamo { U"ㄱㄲㄴㄷㄸㄹㅁㅂㅃㅅㅆㅇㅈㅉㅊㅋㅌㅍㅎ" };

        constexpr int contiguous_base { 1000 };
        constexpr int prefix_bonus { 300 };
        constexpr int boundary_bonus { 150 };
        constexpr int subsequence_base { 400 };
        // 키워드(영문 동의어)에서 맞은 점수에서 뺀다. 제목에서 맞은 것이 늘 위에 선다.
        constexpr int keyword_penalty { 400 };

        char32_t fold(const char32_t value) noexcept
        {
            return value >= U'A' && value <= U'Z' ? value - U'A' + U'a' : value;
        }

        bool is_word_boundary(const char32_t value) noexcept
        {
            switch (value)
            {
            case U' ':
            case U'(':
            case U')':
            case U'/':
            case U'\\':
            case U'-':
            case U'_':
            case U'.':
            case U':':
            case U',':
            case U'…':
                return true;
            default:
                return false;
            }
        }

        bool is_space(const char32_t value) noexcept
        {
            return value == U' ' || value == U'\t' || value == U'\r' || value == U'\n' || value == 0x3000;
        }

        // 질의 글자 하나가 글의 글자 하나와 맞는가. `last`는 질의의 마지막 글자(IME가 조합 중일 수 있다)다.
        bool character_matches(const char32_t wanted, const char32_t actual, const bool last) noexcept
        {
            if (fold(wanted) == fold(actual))
                return true;
            if (is_choseong_jamo(wanted))
            {
                const std::optional<char32_t> initial { choseong_of(actual) };
                return initial.has_value() && *initial == wanted;
            }
            if (last && is_hangul_syllable(wanted) && is_hangul_syllable(actual))
            {
                const char32_t wanted_index { wanted - hangul_syllable_first };
                const char32_t actual_index { actual - hangul_syllable_first };
                // 받침이 없는 글자는 받침 앞까지(초성·중성)만 같으면 된다.
                return wanted_index % final_count == 0 && wanted_index / final_count == actual_index / final_count;
            }
            return false;
        }

        std::optional<int> contiguous_score(const std::u32string_view query, const std::u32string_view text)
        {
            std::optional<int> best {};
            for (std::size_t start { 0 }; start + query.size() <= text.size(); ++start)
            {
                bool matched { true };
                for (std::size_t offset { 0 }; offset < query.size() && matched; ++offset)
                    matched = character_matches(query[offset], text[start + offset], offset + 1 == query.size());
                if (matched == false)
                    continue;
                int score { contiguous_base - static_cast<int>(start) * 3 - static_cast<int>(text.size() - query.size()) };
                if (start == 0)
                    score += prefix_bonus;
                else if (is_word_boundary(text[start - 1]))
                    score += boundary_bonus;
                if (best.has_value() == false || score > *best)
                    best = score;
            }
            return best;
        }

        std::optional<int> subsequence_score(const std::u32string_view query, const std::u32string_view text)
        {
            if (query.size() < 2)
                return std::nullopt;
            std::size_t matched { 0 };
            std::size_t first { 0 };
            std::size_t last { 0 };
            for (std::size_t index { 0 }; index < text.size() && matched < query.size(); ++index)
            {
                if (character_matches(query[matched], text[index], matched + 1 == query.size()) == false)
                    continue;
                if (matched == 0)
                    first = index;
                last = index;
                ++matched;
            }
            if (matched != query.size())
                return std::nullopt;
            const int gaps { static_cast<int>(last - first + 1 - query.size()) };
            return std::max(1, subsequence_base - gaps * 5 - static_cast<int>(first));
        }

        std::vector<std::u32string> split_tokens(const std::u32string& query)
        {
            std::vector<std::u32string> tokens {};
            std::u32string current {};
            for (const char32_t value : query)
            {
                if (is_space(value))
                {
                    if (current.empty() == false)
                        tokens.push_back(std::move(current));
                    current.clear();
                    continue;
                }
                current.push_back(value);
            }
            if (current.empty() == false)
                tokens.push_back(std::move(current));
            return tokens;
        }
    } // namespace

    bool is_hangul_syllable(const char32_t value) noexcept
    {
        return value >= hangul_syllable_first && value <= hangul_syllable_last;
    }

    bool is_choseong_jamo(const char32_t value) noexcept
    {
        return std::ranges::find(choseong_jamo, value) != choseong_jamo.end();
    }

    std::optional<char32_t> choseong_of(const char32_t syllable) noexcept
    {
        if (is_hangul_syllable(syllable) == false)
            return std::nullopt;
        return choseong_jamo[(syllable - hangul_syllable_first) / syllables_per_initial];
    }

    std::u32string decode_utf8(const std::u8string_view text)
    {
        std::u32string result {};
        result.reserve(text.size());
        std::size_t index { 0 };
        while (index < text.size())
        {
            const auto lead { static_cast<unsigned char>(text[index]) };
            std::size_t length { 1 };
            char32_t value { lead };
            if (lead >= 0xF0 && lead <= 0xF4)
            {
                length = 4;
                value = lead & 0x07u;
            }
            else if (lead >= 0xE0)
            {
                length = 3;
                value = lead & 0x0Fu;
            }
            else if (lead >= 0xC2 && lead <= 0xDF)
            {
                length = 2;
                value = lead & 0x1Fu;
            }
            else if (lead >= 0x80)
            {
                result.push_back(U'�');
                ++index;
                continue;
            }
            bool valid { index + length <= text.size() };
            for (std::size_t offset { 1 }; valid && offset < length; ++offset)
            {
                const auto next { static_cast<unsigned char>(text[index + offset]) };
                valid = (next & 0xC0u) == 0x80u;
                value = (value << 6) | (next & 0x3Fu);
            }
            if (valid == false || (length == 1 && lead >= 0x80))
            {
                result.push_back(U'�');
                ++index;
                continue;
            }
            result.push_back(value);
            index += length;
        }
        return result;
    }

    std::optional<int> fuzzy_token_score(const std::u32string_view query, const std::u32string_view text)
    {
        if (query.empty())
            return 0;
        if (query.size() > text.size())
            return std::nullopt;
        if (const std::optional<int> contiguous { contiguous_score(query, text) }; contiguous.has_value())
            return contiguous;
        return subsequence_score(query, text);
    }

    std::optional<int> fuzzy_match_score(const std::u8string_view query, const std::u8string_view title, const std::u8string_view keywords)
    {
        const std::vector<std::u32string> tokens { split_tokens(decode_utf8(query)) };
        if (tokens.empty())
            return 0;
        const std::u32string title_text { decode_utf8(title) };
        const std::u32string keyword_text { decode_utf8(keywords) };
        int total { 0 };
        for (const std::u32string& token : tokens)
        {
            std::optional<int> best { fuzzy_token_score(token, title_text) };
            if (const std::optional<int> keyword { fuzzy_token_score(token, keyword_text) }; keyword.has_value())
            {
                const int reduced { std::max(1, *keyword - keyword_penalty) };
                if (best.has_value() == false || reduced > *best)
                    best = reduced;
            }
            if (best.has_value() == false)
                return std::nullopt;
            total += *best;
        }
        return total;
    }
} // namespace cyella
