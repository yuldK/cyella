#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

namespace cyella {
    // git 출력 해석기가 함께 쓰는 작은 도구다. 모두 입력 바이트를 복사하지 않고
    // string_view로 자른다.
    namespace git_text {
        // `separator` 앞까지를 field로 돌려주고 rest를 구분자 다음으로 옮긴다. 구분자가
        // 없으면 남은 전부가 field이고 rest는 비며 false를 돌려준다.
        inline bool take_field(std::string_view& rest, const char separator, std::string_view& field) noexcept
        {
            const std::size_t position { rest.find(separator) };
            if (position == std::string_view::npos)
            {
                field = rest;
                rest = std::string_view {};
                return false;
            }
            field = rest.substr(0, position);
            rest.remove_prefix(position + 1);
            return true;
        }

        template<typename integer>
        [[nodiscard]] std::optional<integer> parse_integer(const std::string_view text) noexcept
        {
            integer value {};
            const auto [end, error] { std::from_chars(text.data(), text.data() + text.size(), value) };
            if (error != std::errc {} || end != text.data() + text.size())
                return std::nullopt;
            return value;
        }

        [[nodiscard]] inline bool starts_with(const std::string_view text, const std::string_view prefix) noexcept
        {
            return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
        }

        [[nodiscard]] inline bool ends_with(const std::string_view text, const std::string_view suffix) noexcept
        {
            return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
        }
    } // namespace git_text
} // namespace cyella
