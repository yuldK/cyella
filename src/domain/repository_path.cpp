#include "domain/repository_path.h"

#include <cstddef>

namespace cyella {
    namespace {
        constexpr bool is_path_separator(const char8_t value) noexcept
        {
            return value == u8'\\' || value == u8'/';
        }

        constexpr char8_t ascii_lower(const char8_t value) noexcept
        {
            if (value >= u8'A' && value <= u8'Z')
                return static_cast<char8_t>(value - u8'A' + u8'a');
            return value;
        }
    } // namespace

    std::u8string normalize_repository_path(const std::u8string_view path)
    {
        std::u8string result {};
        result.reserve(path.size());
        for (const char8_t value : path)
            result.push_back(is_path_separator(value) ? u8'\\' : value);

        // 뿌리(`C:\`)까지 지워 버리지 않도록 세 글자 아래로는 줄이지 않는다.
        while (result.size() > 3 && result.back() == u8'\\')
            result.pop_back();
        return result;
    }

    std::u8string repository_path_key(const std::u8string_view path)
    {
        std::u8string key { normalize_repository_path(path) };
        for (char8_t& value : key)
            value = ascii_lower(value);
        return key;
    }

    std::u8string repository_display_name(const std::u8string_view path)
    {
        const std::u8string normalized { normalize_repository_path(path) };
        const std::size_t separator { normalized.find_last_of(u8'\\') };
        if (separator == std::u8string::npos)
            return normalized;

        std::u8string name { normalized.substr(separator + 1) };
        // `C:\`처럼 이름이 비는 뿌리는 경로 자체가 가장 알아보기 쉬운 이름이다.
        if (name.empty())
            return normalized;
        return name;
    }

    std::u8string repository_parent_folder(const std::u8string_view path)
    {
        const std::u8string normalized { normalize_repository_path(path) };
        const std::size_t separator { normalized.find_last_of(u8'\\') };
        if (separator == std::u8string::npos)
            return {};

        // 뿌리 자체(`C:\`)에는 부모가 없다.
        if (separator + 1 == normalized.size())
            return {};
        // 뿌리 바로 아래(`C:\repo`)의 부모는 구분자를 남긴 `C:\`다.
        if (separator == 2 && normalized[1] == u8':')
            return normalized.substr(0, separator + 1);
        return normalized.substr(0, separator);
    }
} // namespace cyella
