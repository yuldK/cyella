#include "infrastructure/secret_masking.h"

#include <array>
#include <cstddef>

namespace cyella {
    namespace {
        // 값을 감추는 대상 목록이다. Git의 인자, HTTP 헤더 형태 출력과 널리
        // 쓰이는 token 접두어를 한곳에 모아 확장하기 쉽게 한다.
        constexpr std::array credential_options {
            std::u8string_view { u8"--password" },
            std::u8string_view { u8"--pass" },
            std::u8string_view { u8"--token" },
            std::u8string_view { u8"--access-token" },
            std::u8string_view { u8"--private-token" },
            std::u8string_view { u8"--auth-token" },
        };
        constexpr std::array credential_headers {
            std::u8string_view { u8"authorization:" },
            std::u8string_view { u8"private-token:" },
            std::u8string_view { u8"x-access-token:" },
        };
        constexpr std::array token_prefixes {
            std::u8string_view { u8"ghp_" },
            std::u8string_view { u8"gho_" },
            std::u8string_view { u8"ghu_" },
            std::u8string_view { u8"ghs_" },
            std::u8string_view { u8"ghr_" },
            std::u8string_view { u8"github_pat_" },
            std::u8string_view { u8"glpat-" },
        };
        constexpr std::size_t minimum_basic_credential_length { 8 };

        constexpr char8_t ascii_lower(const char8_t value) noexcept
        {
            if (value >= u8'A' && value <= u8'Z')
                return static_cast<char8_t>(value + (u8'a' - u8'A'));
            return value;
        }

        constexpr bool is_inline_space(const char8_t value) noexcept
        {
            return value == u8' ' || value == u8'\t';
        }

        constexpr bool is_line_break(const char8_t value) noexcept
        {
            return value == u8'\r' || value == u8'\n';
        }

        constexpr bool is_word_byte(const char8_t value) noexcept
        {
            const char8_t lowered { ascii_lower(value) };
            return (lowered >= u8'a' && lowered <= u8'z') || (value >= u8'0' && value <= u8'9') || value == u8'_' || value == u8'-';
        }

        constexpr bool is_base64_byte(const char8_t value) noexcept
        {
            const char8_t lowered { ascii_lower(value) };
            return (lowered >= u8'a' && lowered <= u8'z') || (value >= u8'0' && value <= u8'9') || value == u8'+' || value == u8'/' || value == u8'=';
        }

        bool matches_ignoring_case(const std::u8string_view text, const std::size_t index, const std::u8string_view literal) noexcept
        {
            if (index + literal.size() > text.size())
                return false;
            for (std::size_t offset = 0; offset < literal.size(); ++offset)
                if (ascii_lower(text[index + offset]) != ascii_lower(literal[offset]))
                    return false;
            return true;
        }

        // option과 token 이름은 단어의 처음에서만 인식한다. 다른 값의 일부를 잘못
        // 가리지 않기 위해서다.
        bool at_word_start(const std::u8string_view text, const std::size_t index) noexcept
        {
            return index == 0 || is_word_byte(text[index - 1]) == false;
        }

        void append_masked_value(const std::u8string_view text, std::size_t& cursor, std::u8string& output)
        {
            if (cursor >= text.size() || is_line_break(text[cursor]))
                return;

            // 명령줄 인용이 남아 있을 수 있으므로 따옴표 안쪽만 가린다.
            if (text[cursor] == u8'"')
            {
                ++cursor;
                while (cursor < text.size() && text[cursor] != u8'"')
                    ++cursor;
                output.push_back(u8'"');
                output.append(secret_mask);
                if (cursor < text.size())
                {
                    output.push_back(u8'"');
                    ++cursor;
                }
                return;
            }

            const std::size_t start { cursor };
            while (cursor < text.size() && is_inline_space(text[cursor]) == false && is_line_break(text[cursor]) == false)
                ++cursor;
            if (cursor > start)
                output.append(secret_mask);
        }

        // `ssh://git@host`의 `git`은 비밀이 아니라 접속 계정 이름이다. 이것까지 가리면 작업 로그에서
        // 어떤 원격에 붙었는지 알아볼 수 없다. 이 scheme들은 비밀번호(`user:pass@`)가 있을 때만 가린다.
        bool scheme_keeps_plain_user(const std::u8string_view text, const std::size_t separator_index) noexcept
        {
            std::size_t begin { separator_index };
            while (begin > 0 && (is_word_byte(text[begin - 1]) || text[begin - 1] == u8'+' || text[begin - 1] == u8'.'))
                --begin;
            const std::u8string_view scheme { text.substr(begin, separator_index - begin) };
            const auto equals = [&scheme](const std::u8string_view literal) { return scheme.size() == literal.size() && matches_ignoring_case(scheme, 0, literal); };
            return equals(u8"ssh") || equals(u8"git+ssh") || equals(u8"ssh+git");
        }

        bool try_mask_url_userinfo(const std::u8string_view text, std::size_t& index, std::u8string& output)
        {
            if (matches_ignoring_case(text, index, u8"://") == false)
                return false;

            const std::size_t authority { index + 3 };
            std::size_t cursor { authority };
            std::size_t separator { std::u8string_view::npos };
            while (cursor < text.size())
            {
                const char8_t value { text[cursor] };
                // percent-encoding 없이 password에 `@`가 들어간 URL도 있으므로 authority가
                // 끝나기 전의 마지막 `@`를 userinfo 구분자로 삼아야 값이 새지 않는다.
                if (value == u8'@')
                    separator = cursor;
                // authority가 끝나면 userinfo가 없는 URL이다.
                else if (value == u8'/' || value == u8'?' || value == u8'#' || value == u8'"' || is_inline_space(value) || is_line_break(value))
                    break;
                ++cursor;
            }
            if (separator == std::u8string_view::npos || separator == authority)
                return false;
            if (text.substr(authority, separator - authority).find(u8':') == std::u8string_view::npos && scheme_keeps_plain_user(text, index))
                return false;

            const std::u8string_view userinfo { text.substr(authority, separator - authority) };
            const std::size_t colon { userinfo.find(u8':') };
            output.append(u8"://");
            if (colon == std::u8string_view::npos)
            {
                // 사용자 이름 없이 들어간 값은 대개 token이므로 전체를 가린다.
                output.append(secret_mask);
            }
            else
            {
                output.append(userinfo.substr(0, colon));
                output.push_back(u8':');
                output.append(secret_mask);
            }
            output.push_back(u8'@');
            index = separator + 1;
            return true;
        }

        bool try_mask_option_value(const std::u8string_view text, std::size_t& index, std::u8string& output)
        {
            if (at_word_start(text, index) == false)
                return false;

            for (const std::u8string_view option : credential_options)
            {
                if (matches_ignoring_case(text, index, option) == false)
                    continue;

                std::size_t cursor { index + option.size() };
                // 이름이 여기서 끝나지 않으면 더 긴 다른 option이다.
                const bool assigned { cursor < text.size() && text[cursor] == u8'=' };
                const bool separated { cursor < text.size() && is_inline_space(text[cursor]) };
                if (assigned == false && separated == false)
                    continue;

                output.append(text.substr(index, option.size()));
                if (assigned)
                {
                    output.push_back(u8'=');
                    ++cursor;
                }
                else
                {
                    while (cursor < text.size() && is_inline_space(text[cursor]))
                    {
                        output.push_back(text[cursor]);
                        ++cursor;
                    }
                }
                append_masked_value(text, cursor, output);
                index = cursor;
                return true;
            }
            return false;
        }

        bool try_mask_header_value(const std::u8string_view text, std::size_t& index, std::u8string& output)
        {
            if (at_word_start(text, index) == false)
                return false;

            for (const std::u8string_view header : credential_headers)
            {
                if (matches_ignoring_case(text, index, header) == false)
                    continue;

                output.append(text.substr(index, header.size()));
                std::size_t cursor { index + header.size() };
                while (cursor < text.size() && is_inline_space(text[cursor]))
                {
                    output.push_back(text[cursor]);
                    ++cursor;
                }

                // 헤더 값은 scheme과 자격 증명이 함께 있으므로 줄 끝까지 가린다.
                const std::size_t start { cursor };
                while (cursor < text.size() && is_line_break(text[cursor]) == false)
                    ++cursor;
                if (cursor > start)
                    output.append(secret_mask);
                index = cursor;
                return true;
            }
            return false;
        }

        bool try_mask_basic_credentials(const std::u8string_view text, std::size_t& index, std::u8string& output)
        {
            constexpr std::u8string_view scheme { u8"basic" };
            if (at_word_start(text, index) == false || matches_ignoring_case(text, index, scheme) == false)
                return false;

            std::size_t cursor { index + scheme.size() };
            if (cursor >= text.size() || is_inline_space(text[cursor]) == false)
                return false;

            std::size_t value { cursor };
            while (value < text.size() && is_inline_space(text[value]))
                ++value;

            std::size_t end { value };
            while (end < text.size() && is_base64_byte(text[end]))
                ++end;
            if (end - value < minimum_basic_credential_length)
                return false;

            output.append(text.substr(index, scheme.size()));
            output.append(text.substr(cursor, value - cursor));
            output.append(secret_mask);
            index = end;
            return true;
        }

        bool try_mask_token(const std::u8string_view text, std::size_t& index, std::u8string& output)
        {
            if (at_word_start(text, index) == false)
                return false;

            for (const std::u8string_view prefix : token_prefixes)
            {
                if (matches_ignoring_case(text, index, prefix) == false)
                    continue;

                std::size_t cursor { index + prefix.size() };
                const std::size_t start { cursor };
                while (cursor < text.size() && is_word_byte(text[cursor]))
                    ++cursor;
                if (cursor == start)
                    continue;

                output.append(secret_mask);
                index = cursor;
                return true;
            }
            return false;
        }
    } // namespace

    std::u8string mask_secrets(const std::u8string_view text)
    {
        std::u8string output {};
        output.reserve(text.size());

        std::size_t index { 0 };
        while (index < text.size())
        {
            if (try_mask_url_userinfo(text, index, output))
                continue;
            if (try_mask_option_value(text, index, output))
                continue;
            if (try_mask_header_value(text, index, output))
                continue;
            if (try_mask_basic_credentials(text, index, output))
                continue;
            if (try_mask_token(text, index, output))
                continue;

            output.push_back(text[index]);
            ++index;
        }
        return output;
    }
} // namespace cyella
