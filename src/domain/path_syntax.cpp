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

        // 맨 앞 구분자 두 개를 뗀 `server\share[\...]`꼴이다. 서버와 공유 이름이 모두 비어 있지 않아야 한다.
        bool is_unc_share(const std::u8string_view rest) noexcept
        {
            const std::size_t server_end { rest.find_first_of(u8"\\/") };
            if (server_end == std::u8string_view::npos || server_end == 0)
                return false;
            return server_end + 1 < rest.size() && is_path_separator(rest[server_end + 1]) == false;
        }

        // `\\?\` 뒤의 꼴이다. `C:\...`와 `UNC\server\share`만 받고 `GLOBALROOT`·`Volume{...}` 같은 이름 공간은 거절한다.
        bool is_extended_path_rest(const std::u8string_view rest) noexcept
        {
            if (rest.size() >= 3 && is_ascii_letter(rest[0]) && rest[1] == u8':' && is_path_separator(rest[2]))
                return true;
            if (rest.size() < 4 || is_path_separator(rest[3]) == false)
                return false;
            // `UNC`는 대소문자를 가리지 않는다.
            const bool is_unc { (rest[0] == u8'U' || rest[0] == u8'u') && (rest[1] == u8'N' || rest[1] == u8'n') && (rest[2] == u8'C' || rest[2] == u8'c') };
            return is_unc && is_unc_share(rest.substr(4));
        }

        // 구성 요소 하나가 `.git`의 8.3 짧은 이름(`GIT~1`)이다. ASCII 대소문자를 가리지 않는다.
        bool is_git_short_name(const std::string_view name) noexcept
        {
            constexpr std::string_view stem { "git~" };
            if (name.size() <= stem.size())
                return false;
            for (std::size_t index { 0 }; index < stem.size(); ++index)
            {
                const char value { name[index] >= 'A' && name[index] <= 'Z' ? static_cast<char>(name[index] - 'A' + 'a') : name[index] };
                if (value != stem[index])
                    return false;
            }
            return name.find_first_not_of("0123456789", stem.size()) == std::string_view::npos;
        }
    } // namespace

    bool is_absolute_windows_path(const std::u8string_view path) noexcept
    {
        // UNC(`\\server\share`)와 확장 길이 경로(`\\?\C:\`)만 받는다. 공유 이름이 없는 `\\server`와 장치 이름 공간
        // `\\.\`은 CreateProcess에 넘길 폴더나 실행 파일이 아니다.
        if (path.size() >= 2 && is_path_separator(path[0]) && is_path_separator(path[1]))
        {
            const std::u8string_view rest { path.substr(2) };
            if (rest.size() >= 2 && rest[0] == u8'?' && is_path_separator(rest[1]))
                return is_extended_path_rest(rest.substr(2));
            if (rest.size() >= 2 && rest[0] == u8'.' && is_path_separator(rest[1]))
                return false;
            return is_unc_share(rest);
        }
        // `C:relative`는 drive 기준 상대 경로이고 `\absolute`는 현재 drive에 의존한다.
        if (path.size() < 3)
            return false;
        return is_ascii_letter(path[0]) && path[1] == u8':' && is_path_separator(path[2]);
    }

    bool is_absolute_path(const std::u8string_view path) noexcept
    {
#if defined(_WIN32)
        return is_absolute_windows_path(path);
#else
        return path.empty() == false && path.front() == u8'/';
#endif
    }

    std::u8string join_native_path(const std::u8string_view directory, const std::u8string_view name)
    {
        std::u8string result { directory };
        if (result.empty() == false && is_native_path_separator(result.back()) == false)
            result.push_back(native_path_separator);
        result.append(name);
        return result;
    }

    std::u8string native_parent_path(const std::u8string_view path)
    {
        std::u8string_view trimmed { path };
        while (trimmed.size() > native_root_length && is_native_path_separator(trimmed.back()))
            trimmed.remove_suffix(1);
        const std::size_t separator { trimmed.find_last_of(native_path_separators) };
        if (separator == std::u8string_view::npos)
            return {};
        // 뿌리 자체에는 부모가 없다.
        if (separator + 1 == trimmed.size())
            return {};
#if defined(_WIN32)
        // 뿌리 바로 아래(`C:\repo`)의 부모는 구분자를 남긴 `C:\`다.
        if (separator == 2 && trimmed[1] == u8':')
            return std::u8string { trimmed.substr(0, separator + 1) };
#else
        // 뿌리 바로 아래(`/repo`)의 부모는 `/`다.
        if (separator == 0)
            return std::u8string { trimmed.substr(0, 1) };
#endif
        return std::u8string { trimmed.substr(0, separator) };
    }

    std::optional<std::u8string> resolve_workspace_child(const std::u8string_view root, const std::string_view relative_path)
    {
        if (root.empty() || is_absolute_path(root) == false || relative_path.empty())
            return std::nullopt;
        std::u8string result { root };
        while (result.size() > native_root_length && is_native_path_separator(result.back()))
            result.pop_back();
        // POSIX에서 `\`는 파일 이름의 글자다. git은 구분자로 늘 `/`를 쓴다.
#if defined(_WIN32)
        constexpr std::string_view component_separators { "/\\" };
#else
        constexpr std::string_view component_separators { "/" };
#endif
        std::size_t start { 0 };
        while (start <= relative_path.size())
        {
            std::size_t end { relative_path.find_first_of(component_separators, start) };
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
            // NTFS는 구성 요소 끝의 점과 공백을 떼므로 `.git.`·`.git `은 `.git`이고 `...`은 이름이 없다. 8.3 짧은 이름
            // `GIT~1`도 `.git`이다. 이런 이름이 실제로 올 일은 드물지만 되돌리기 어려운 동작의 방어선으로 막는다.
            if (component.back() == '.' || component.back() == ' ' || is_git_short_name(component))
                return std::nullopt;
            if (is_native_path_separator(result.back()) == false)
                result.push_back(native_path_separator);
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
            if (is_native_path_separator(value))
                key.push_back(u8'/');
            else if (value >= u8'A' && value <= u8'Z')
                key.push_back(static_cast<char8_t>(value - u8'A' + u8'a'));
            else
                key.push_back(value);
        // `C:/`·`/`의 구분자는 루트라서 남긴다.
        while (key.size() > native_root_length && key.back() == u8'/')
            key.pop_back();
        return key;
    }

    std::string gitignore_literal(const std::string_view path)
    {
        std::size_t kept { path.size() };
        while (kept > 0 && path[kept - 1] == ' ')
            --kept;
        std::string result {};
        result.reserve(path.size() * 2);
        for (std::size_t index { 0 }; index < path.size(); ++index)
        {
            const char value { path[index] };
            if (value == '*' || value == '?' || value == '[' || value == '\\' || index >= kept)
                result.push_back('\\');
            result.push_back(value);
        }
        return result;
    }
} // namespace cyella
