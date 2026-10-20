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

    std::optional<std::u8string> resolve_workspace_child(const std::u8string_view root, const std::string_view relative_path)
    {
        if (root.empty() || is_absolute_windows_path(root) == false || relative_path.empty())
            return std::nullopt;
        std::u8string result { root };
        while (result.empty() == false && is_path_separator(result.back()))
            result.pop_back();
        std::size_t start { 0 };
        while (start <= relative_path.size())
        {
            std::size_t end { relative_path.find_first_of("/\\", start) };
            if (end == std::string_view::npos)
                end = relative_path.size();
            const std::string_view component { relative_path.substr(start, end - start) };
            // 빈 구성 요소(맨 앞의 `/`, 이어진 `//`, 끝의 `/`)와 `.`·`..`은 저장소 밖이나 다른 파일을 가리킬 수 있다.
            if (component.empty() || component == "." || component == ".." || component.find('\0') != std::string_view::npos || component.find(':') != std::string_view::npos)
                return std::nullopt;
            // `.git`은 어느 깊이에서든 막는다(중첩 저장소 포함). ASCII 대소문자를 가리지 않는다 (NTFS).
            if (component.size() == 4 && (component[0] == '.') && (component[1] == 'g' || component[1] == 'G') && (component[2] == 'i' || component[2] == 'I')
                && (component[3] == 't' || component[3] == 'T'))
                return std::nullopt;
            result.push_back(u8'\\');
            result.append(component.begin(), component.end());
            if (end == relative_path.size())
                break;
            start = end + 1;
        }
        return result;
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
