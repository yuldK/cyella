#include "domain/text_catalog.h"

#include "domain/korean_josa.h"

#include <cstddef>
#include <string>
#include <utility>

namespace cyella {
    namespace {
        struct installed_catalogs
        {
            text_catalog base {};
            std::optional<text_catalog> active {};
        };

        installed_catalogs& catalogs() noexcept
        {
            static installed_catalogs value {};
            return value;
        }

        std::string to_narrow(const std::u8string_view text)
        {
            return std::string { reinterpret_cast<const char*>(text.data()), text.size() };
        }

        const text_argument* find_argument(const std::span<const text_argument> arguments, const std::u8string_view name) noexcept
        {
            for (const text_argument& argument : arguments)
                if (argument.name == name)
                    return &argument;
            return nullptr;
        }

        // `을/를`처럼 받침이 있을 때와 없을 때의 조사 한 쌍을 값 뒤에 붙인다. `으로/로`는 ㄹ 받침도 `로`다.
        std::u8string attach_particle(const std::u8string_view value, const std::u8string_view rule)
        {
            const std::size_t slash { rule.find(u8'/') };
            if (slash == std::u8string_view::npos)
                return std::u8string { value } + std::u8string { rule };
            const std::u8string_view with_batchim { rule.substr(0, slash) };
            const std::u8string_view without_batchim { rule.substr(slash + 1) };
            bool batchim { ends_with_batchim(value) };
            if (with_batchim == u8"으로" && ends_with_rieul(value))
                batchim = false;
            return std::u8string { value } + std::u8string { batchim ? with_batchim : without_batchim };
        }

        // `{` 다음부터 짝 `}`까지를 이름과 규칙으로 가른다. 닫히지 않으면 npos다.
        struct placeholder
        {
            std::u8string_view name {};
            std::u8string_view rule {};
            bool has_rule { false };
            std::size_t end { std::u8string_view::npos };
        };

        placeholder read_placeholder(const std::u8string_view pattern, const std::size_t open) noexcept
        {
            placeholder result {};
            const std::size_t close { pattern.find(u8'}', open + 1) };
            if (close == std::u8string_view::npos)
                return result;
            const std::u8string_view body { pattern.substr(open + 1, close - open - 1) };
            const std::size_t bar { body.find(u8'|') };
            result.name = body.substr(0, bar);
            if (bar != std::u8string_view::npos)
            {
                result.rule = body.substr(bar + 1);
                result.has_rule = true;
            }
            result.end = close + 1;
            return result;
        }
    } // namespace

    std::u8string_view language_preference_name(const language_preference preference) noexcept
    {
        switch (preference)
        {
        case language_preference::system:
            return u8"system";
        case language_preference::korean:
            return u8"ko";
        case language_preference::english:
            return u8"en";
        }
        return u8"system";
    }

    std::optional<language_preference> parse_language_preference(const std::u8string_view name) noexcept
    {
        if (name == u8"system")
            return language_preference::system;
        if (name == u8"ko")
            return language_preference::korean;
        if (name == u8"en")
            return language_preference::english;
        return std::nullopt;
    }

    ui_language resolve_ui_language(const language_preference preference, const ui_language system_language) noexcept
    {
        switch (preference)
        {
        case language_preference::korean:
            return ui_language::korean;
        case language_preference::english:
            return ui_language::english;
        case language_preference::system:
            break;
        }
        return system_language;
    }

    text_argument::text_argument(const std::u8string_view argument_name, const std::u8string_view argument_value)
        : name { argument_name }
        , value { argument_value }
    {}

    text_argument::text_argument(const std::u8string_view argument_name, const std::string_view argument_value)
        : name { argument_name }
        , value { reinterpret_cast<const char8_t*>(argument_value.data()), argument_value.size() }
    {}

    std::u8string text_argument::integer_text(const std::int64_t number)
    {
        const std::string digits { std::to_string(number) };
        return std::u8string { reinterpret_cast<const char8_t*>(digits.data()), digits.size() };
    }

    const std::u8string* text_catalog::find(const std::u8string_view key) const
    {
        const auto found { entries.find(key) };
        return found == entries.end() ? nullptr : &found->second;
    }

    std::u8string format_text(const std::u8string_view pattern, const std::span<const text_argument> arguments)
    {
        std::u8string result {};
        result.reserve(pattern.size() + 16);
        std::size_t index { 0 };
        while (index < pattern.size())
        {
            const char8_t current { pattern[index] };
            if ((current == u8'{' || current == u8'}') && index + 1 < pattern.size() && pattern[index + 1] == current)
            {
                result.push_back(current);
                index += 2;
                continue;
            }
            if (current != u8'{')
            {
                result.push_back(current);
                ++index;
                continue;
            }

            const placeholder slot { read_placeholder(pattern, index) };
            if (slot.end == std::u8string_view::npos)
            {
                result.append(pattern.substr(index));
                break;
            }
            const text_argument* const argument { find_argument(arguments, slot.name) };
            if (argument == nullptr)
                result.append(pattern.substr(index, slot.end - index));
            else if (slot.has_rule)
                result.append(attach_particle(argument->value, slot.rule));
            else
                result.append(argument->value);
            index = slot.end;
        }
        return result;
    }

    std::optional<std::set<std::u8string, std::less<>>> text_placeholders(const std::u8string_view pattern)
    {
        std::set<std::u8string, std::less<>> names {};
        std::size_t index { 0 };
        while (index < pattern.size())
        {
            const char8_t current { pattern[index] };
            if ((current == u8'{' || current == u8'}') && index + 1 < pattern.size() && pattern[index + 1] == current)
            {
                index += 2;
                continue;
            }
            if (current == u8'}')
                return std::nullopt;
            if (current != u8'{')
            {
                ++index;
                continue;
            }
            const placeholder slot { read_placeholder(pattern, index) };
            if (slot.end == std::u8string_view::npos || slot.name.empty())
                return std::nullopt;
            names.emplace(slot.name);
            index = slot.end;
        }
        return names;
    }

    void install_text_catalogs(text_catalog base, std::optional<text_catalog> active)
    {
        installed_catalogs& value { catalogs() };
        value.base = std::move(base);
        value.active = std::move(active);
    }

    ui_language active_ui_language() noexcept
    {
        const installed_catalogs& value { catalogs() };
        return value.active.has_value() ? value.active->language : value.base.language;
    }

    std::u8string tr(const std::u8string_view key)
    {
        return tr(key, {});
    }

    std::u8string tr(const std::u8string_view key, const std::initializer_list<text_argument> arguments)
    {
        const installed_catalogs& value { catalogs() };
        const std::u8string* pattern { value.active.has_value() ? value.active->find(key) : nullptr };
        if (pattern == nullptr)
            pattern = value.base.find(key);
        if (pattern == nullptr)
            return std::u8string { key };
        if (arguments.size() == 0)
            return format_text(*pattern, {});
        return format_text(*pattern, std::span { arguments.begin(), arguments.size() });
    }

    std::map<std::u8string, std::u8string, std::less<>> text_patterns_with_prefix(const std::u8string_view prefix)
    {
        std::map<std::u8string, std::u8string, std::less<>> result {};
        const installed_catalogs& value { catalogs() };
        const auto collect = [&result, prefix](const text_catalog& catalog) {
            for (auto entry { catalog.entries.lower_bound(prefix) }; entry != catalog.entries.end() && entry->first.starts_with(prefix); ++entry)
                result.emplace(entry->first, entry->second);
        };
        // emplace는 있는 키를 덮지 않으므로 고른 언어를 먼저 모은다.
        if (value.active.has_value())
            collect(*value.active);
        collect(value.base);
        return result;
    }

    std::string tr_narrow(const std::u8string_view key)
    {
        return to_narrow(tr(key));
    }

    std::string tr_narrow(const std::u8string_view key, const std::initializer_list<text_argument> arguments)
    {
        return to_narrow(tr(key, arguments));
    }
} // namespace cyella
