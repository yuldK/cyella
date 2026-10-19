#include "domain/external_diff_tool.h"

#include <cstddef>
#include <utility>

namespace cyella {
    namespace {
        constexpr char8_t ascii_lower(const char8_t value) noexcept
        {
            if (value >= u8'A' && value <= u8'Z')
                return static_cast<char8_t>(value - u8'A' + u8'a');
            return value;
        }

        std::u8string lowered(const std::u8string_view text)
        {
            std::u8string result { text };
            for (char8_t& value : result)
                value = ascii_lower(value);
            return result;
        }

        constexpr bool is_template_space(const char8_t value) noexcept
        {
            return value == u8' ' || value == u8'\t' || value == u8'\r' || value == u8'\n';
        }

        struct token_value
        {
            std::u8string_view token {};
            const std::u8string* value { nullptr };
        };

        // 한 인자 안의 토큰을 값으로 바꾼다. 한 번만 훑으므로 값 안의 토큰은 다시 풀지 않는다.
        std::u8string substitute_tokens(const std::u8string_view argument, const external_diff_values& values)
        {
            const token_value tokens[] {
                { u8"{left}", &values.left },
                { u8"{right}", &values.right },
                { u8"{base}", &values.base },
                { u8"{path}", &values.path },
            };

            std::u8string result {};
            std::size_t position { 0 };
            while (position < argument.size())
            {
                bool replaced { false };
                if (argument[position] == u8'{')
                    for (const token_value& candidate : tokens)
                        if (argument.substr(position, candidate.token.size()) == candidate.token)
                        {
                            result.append(*candidate.value);
                            position += candidate.token.size();
                            replaced = true;
                            break;
                        }

                if (replaced == false)
                {
                    result.push_back(argument[position]);
                    ++position;
                }
            }
            return result;
        }
    } // namespace

    std::optional<std::vector<std::u8string>> expand_external_diff_arguments(const std::u8string_view argument_template, const external_diff_values& values)
    {
        std::vector<std::u8string> arguments {};
        std::u8string raw {};
        bool in_quotes { false };
        // 따옴표를 만났으면 내용이 비어도 인자 하나로 센다 (`""`).
        bool has_argument { false };

        const auto finish_argument = [&] {
            if (has_argument)
            {
                std::u8string expanded { substitute_tokens(raw, values) };
                // 내용이 있던 인자가 비었다면 쓸 값이 없는 것이라 인자를 뺀다.
                if (expanded.empty() == false || raw.empty())
                    arguments.push_back(std::move(expanded));
            }
            raw.clear();
            has_argument = false;
        };

        for (const char8_t value : argument_template)
        {
            if (value == u8'"')
            {
                in_quotes = in_quotes == false;
                has_argument = true;
                continue;
            }
            if (in_quotes == false && is_template_space(value))
            {
                finish_argument();
                continue;
            }
            raw.push_back(value);
            has_argument = true;
        }

        if (in_quotes)
            return std::nullopt;
        finish_argument();
        return arguments;
    }

    std::u8string normalize_diff_extension(const std::u8string_view extension)
    {
        std::u8string_view text { extension };
        while (text.empty() == false && is_template_space(text.front()))
            text.remove_prefix(1);
        while (text.empty() == false && is_template_space(text.back()))
            text.remove_suffix(1);
        // `*.png` 같은 glob 표기도 받아 준다.
        if (text.empty() == false && text.front() == u8'*')
            text.remove_prefix(1);
        if (text.empty())
            return {};

        std::u8string result {};
        if (text.front() != u8'.')
            result.push_back(u8'.');
        result.append(lowered(text));
        if (result.size() < 2)
            return {};
        return result;
    }

    const external_diff_tool* select_external_diff_tool(const std::vector<external_diff_tool>& tools, const std::u8string_view path) noexcept
    {
        try
        {
            const std::size_t separator { path.find_last_of(u8"\\/") };
            const std::u8string name { lowered(separator == std::u8string_view::npos ? path : path.substr(separator + 1)) };

            const external_diff_tool* best { nullptr };
            std::size_t best_length { 0 };
            for (const external_diff_tool& tool : tools)
                for (const std::u8string& configured : tool.extensions)
                {
                    const std::u8string extension { normalize_diff_extension(configured) };
                    // 이름이 확장자뿐인 파일(`.png`)은 확장자가 아니라 숨김 파일 이름으로 본다.
                    if (extension.empty() || name.size() <= extension.size())
                        continue;
                    if (name.compare(name.size() - extension.size(), extension.size(), extension) != 0)
                        continue;
                    if (extension.size() > best_length)
                    {
                        best = &tool;
                        best_length = extension.size();
                    }
                }
            return best;
        }
        catch (...)
        {
            return nullptr;
        }
    }
} // namespace cyella
