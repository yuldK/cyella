#include "infrastructure/git_progress_parser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace cyella {
    namespace {
        constexpr std::u8string_view remote_prefix { u8"remote: " };
        constexpr std::size_t maximum_phase_length { 64 };
        // 자릿수 상한이다. 이보다 길면 진행 숫자가 아니라 다른 텍스트다.
        constexpr std::size_t maximum_digits { 18 };

        bool is_ascii_digit(const char8_t value) noexcept
        {
            return value >= u8'0' && value <= u8'9';
        }

        bool is_ascii_letter(const char8_t value) noexcept
        {
            return (value >= u8'a' && value <= u8'z') || (value >= u8'A' && value <= u8'Z');
        }

        std::u8string_view trim(std::u8string_view value) noexcept
        {
            constexpr std::u8string_view whitespace { u8" \t\r\n" };
            const std::size_t begin { value.find_first_not_of(whitespace) };
            if (begin == std::u8string_view::npos)
                return {};
            value.remove_prefix(begin);
            value.remove_suffix(value.size() - 1 - value.find_last_not_of(whitespace));
            return value;
        }

        // `text` 앞쪽의 십진수를 읽고 소비한 만큼 잘라 낸다.
        std::optional<std::uint64_t> take_number(std::u8string_view& text) noexcept
        {
            std::size_t length { 0 };
            while (length < text.size() && is_ascii_digit(text[length]))
                ++length;
            if (length == 0 || length > maximum_digits)
                return std::nullopt;

            std::uint64_t value { 0 };
            for (std::size_t index = 0; index < length; ++index)
                value = (value * 10u) + static_cast<std::uint64_t>(text[index] - u8'0');
            text.remove_prefix(length);
            return value;
        }

        // 단계 이름 규칙이다. 오류 접두어(`fatal`, `error`)는 소문자라 여기서 걸러진다.
        bool is_phase_name(const std::u8string_view name) noexcept
        {
            if (name.empty() || name.size() > maximum_phase_length)
                return false;
            if (name.front() < u8'A' || name.front() > u8'Z')
                return false;
            return std::ranges::all_of(name, [](const char8_t value) { return is_ascii_letter(value) || value == u8' '; });
        }

        // `N% (a/b)` 형태에서 비율을 구한다. 퍼센트는 반올림되어 있으므로 괄호 안의 개수가
        // 있으면 그쪽이 더 정확하다.
        std::optional<double> percent_fraction(std::u8string_view rest) noexcept
        {
            const std::optional<std::uint64_t> percent { take_number(rest) };
            if (percent.has_value() == false || rest.empty() || rest.front() != u8'%')
                return std::nullopt;
            rest.remove_prefix(1);

            double fraction { std::min(static_cast<double>(*percent) / 100.0, 1.0) };
            rest = trim(rest);
            if (rest.empty() == false && rest.front() == u8'(')
            {
                rest.remove_prefix(1);
                const std::optional<std::uint64_t> done { take_number(rest) };
                if (done.has_value() && rest.empty() == false && rest.front() == u8'/')
                {
                    rest.remove_prefix(1);
                    const std::optional<std::uint64_t> total { take_number(rest) };
                    if (total.has_value() && *total != 0 && rest.empty() == false && rest.front() == u8')')
                        fraction = std::min(static_cast<double>(*done) / static_cast<double>(*total), 1.0);
                }
            }
            return fraction;
        }

        // 숫자만 있는 단계(`5`, `5, done.`)다. 전체를 모르므로 끝났다는 표시가 있을 때만
        // 비율이 정해진다. 숫자 뒤에 다른 단어가 오면 진행 줄이 아니다.
        std::optional<git_progress> counted_phase(const std::u8string_view phase, std::u8string_view rest, const bool remote)
        {
            if (take_number(rest).has_value() == false)
                return std::nullopt;
            if (rest.empty())
                return git_progress { std::u8string { phase }, std::nullopt, remote };
            if (rest.front() != u8',')
                return std::nullopt;

            const bool finished { rest.find(u8"done") != std::u8string_view::npos };
            return git_progress { std::u8string { phase }, finished ? std::optional<double> { 1.0 } : std::nullopt, remote };
        }
    } // namespace

    std::optional<git_progress> parse_git_progress(std::u8string_view line)
    {
        line = trim(line);
        bool remote { false };
        if (line.starts_with(remote_prefix))
        {
            remote = true;
            line = trim(line.substr(remote_prefix.size()));
        }

        const std::size_t separator { line.find(u8": ") };
        if (separator == std::u8string_view::npos)
            return std::nullopt;
        const std::u8string_view phase { line.substr(0, separator) };
        if (is_phase_name(phase) == false)
            return std::nullopt;

        // 퍼센트가 앞에 공백으로 맞춰져 있으므로(`Receiving objects:  45%`) 먼저 다듬는다.
        const std::u8string_view rest { trim(line.substr(separator + 2)) };
        if (rest.empty())
            return std::nullopt;

        if (const std::optional<double> fraction { percent_fraction(rest) }; fraction.has_value())
            return git_progress { std::u8string { phase }, fraction, remote };
        return counted_phase(phase, rest, remote);
    }
} // namespace cyella
