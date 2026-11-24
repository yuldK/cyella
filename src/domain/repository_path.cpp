#include "domain/repository_path.h"

#include "domain/path_syntax.h"

#include <cstddef>

namespace cyella {
    namespace {
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
            result.push_back(is_native_path_separator(value) ? native_path_separator : value);

        // 뿌리(`C:\`, `/`)까지 지워 버리지 않도록 뿌리 길이 아래로는 줄이지 않는다.
        while (result.size() > native_root_length && result.back() == native_path_separator)
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
        const std::size_t separator { normalized.find_last_of(native_path_separator) };
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
        // 뿌리 자체(`C:\`, `/`)에는 부모가 없고, 뿌리 바로 아래(`C:\repo`, `/repo`)의 부모는 구분자를 남긴 뿌리다.
        return native_parent_path(normalize_repository_path(path));
    }
} // namespace cyella
