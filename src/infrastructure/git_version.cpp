#include "infrastructure/git_version.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace cyella {
    namespace {
        constexpr std::uint32_t maximum_component { std::numeric_limits<std::uint32_t>::max() / 10u };

        bool is_ascii_digit(const char8_t value) noexcept
        {
            return value >= u8'0' && value <= u8'9';
        }

        // 숫자를 하나 읽고 위치를 옮긴다. 값이 표현 범위를 넘으면 실패로 본다.
        bool read_component(const std::u8string_view text, std::size_t& index, std::uint32_t& value) noexcept
        {
            if (index >= text.size() || is_ascii_digit(text[index]) == false)
                return false;

            std::uint32_t parsed { 0 };
            while (index < text.size() && is_ascii_digit(text[index]))
            {
                if (parsed > maximum_component)
                    return false;
                parsed = (parsed * 10u) + static_cast<std::uint32_t>(text[index] - u8'0');
                ++index;
            }
            value = parsed;
            return true;
        }

        // 첫 줄에서 `숫자.숫자` 형태가 시작되는 위치를 찾는다. 접두어를 고정하지 않고
        // 첫 버전 토큰을 찾는 편이 배포판 표기 차이에 안정적이다.
        std::optional<std::size_t> find_version_start(const std::u8string_view line) noexcept
        {
            for (std::size_t index = 0; index < line.size(); ++index)
            {
                if (is_ascii_digit(line[index]) == false)
                    continue;
                if (index > 0 && (is_ascii_digit(line[index - 1]) || line[index - 1] == u8'.'))
                    continue;

                std::size_t cursor { index };
                std::uint32_t ignored { 0 };
                if (read_component(line, cursor, ignored) == false)
                    continue;
                if (cursor < line.size() && line[cursor] == u8'.')
                    return index;
            }
            return std::nullopt;
        }
    } // namespace

    std::optional<git_tool_version> parse_git_tool_version(const std::u8string_view first_line) noexcept
    {
        const std::optional<std::size_t> start { find_version_start(first_line) };
        if (start.has_value() == false)
            return std::nullopt;

        std::size_t index { *start };
        git_tool_version version {};
        if (read_component(first_line, index, version.major) == false)
            return std::nullopt;
        if (index >= first_line.size() || first_line[index] != u8'.')
            return std::nullopt;
        ++index;
        if (read_component(first_line, index, version.minor) == false)
            return std::nullopt;

        // patch가 없는 표기(`2.43`)도 받아 준다. 이때 patch는 0이다.
        if (index < first_line.size() && first_line[index] == u8'.')
        {
            ++index;
            if (read_component(first_line, index, version.patch) == false)
                return std::nullopt;
        }
        return version;
    }

    bool meets_minimum_git_version(const git_tool_version& version) noexcept
    {
        return version >= minimum_supported_git_version();
    }
} // namespace cyella
