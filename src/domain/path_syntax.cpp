#include "domain/path_syntax.h"

#include <cstddef>

namespace cyella {
    namespace {
        constexpr bool is_path_separator(const char8_t value) noexcept
        {
            return value == u8'\\' || value == u8'/';
        }

        constexpr bool is_ascii_letter(const char8_t value) noexcept
        {
            return (value >= u8'a' && value <= u8'z') || (value >= u8'A' && value <= u8'Z');
        }
    } // namespace

    bool is_absolute_windows_path(const std::u8string_view path) noexcept
    {
        // UNC(`\server\share`)와 device(`\?\C:\`) 경로는 separator 두 개 뒤에 이름이 있어야 한다.
        if (path.size() >= 2 && is_path_separator(path[0]) && is_path_separator(path[1]))
            return path.size() > 2 && is_path_separator(path[2]) == false;
        // `C:relative`는 drive 기준 상대 경로이고 `\absolute`는 현재 drive에 의존한다.
        if (path.size() < 3)
            return false;
        return is_ascii_letter(path[0]) && path[1] == u8':' && is_path_separator(path[2]);
    }

    std::u8string normalize_path_key(const std::u8string_view path)
    {
        std::u8string key {};
        key.reserve(path.size());
        for (const char8_t value : path)
            if (is_path_separator(value))
                key.push_back(u8'/');
            else if (value >= u8'A' && value <= u8'Z')
                key.push_back(static_cast<char8_t>(value - u8'A' + u8'a'));
            else
                key.push_back(value);
        // `C:/`의 구분자는 루트라서 남긴다.
        while (key.size() > 3 && key.back() == u8'/')
            key.pop_back();
        return key;
    }
} // namespace cyella
