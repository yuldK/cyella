#pragma once

#include <string>
#include <string_view>

namespace cyella::test {
    // 시험에 Windows 꼴(`C:\work\repo`, `C:/work/repo`)로 적은 절대 경로를 이 OS의 꼴로 옮긴다.
    //  - Windows에서는 그대로다. 시험이 원래 보던 값이다.
    //  - POSIX에서는 드라이브를 떼고 `\`를 `/`로 바꾼다: `C:\work\repo` → `/work/repo`. C가 아닌 드라이브는 맨 위 폴더 이름으로
    //    남겨 서로 다른 경로가 같아지지 않게 한다: `D:\bin` → `/d/bin`.
    // 입력과 기대값을 같은 함수로 옮기므로 시험의 뜻(어느 경로가 어디로 가는가)은 그대로이고 모양만 그 OS의 것이 된다. 드라이브·UNC처럼
    // Windows 경로 자체의 규칙을 보는 시험은 이것을 쓰지 않고 Windows에서만 돈다.
    [[nodiscard]] inline std::string native_path(const std::string_view windows_path)
    {
#if defined(_WIN32)
        return std::string { windows_path };
#else
        std::string_view rest { windows_path };
        std::string result {};
        if (rest.size() >= 2 && rest[1] == ':' && ((rest[0] >= 'A' && rest[0] <= 'Z') || (rest[0] >= 'a' && rest[0] <= 'z')))
        {
            const char drive { static_cast<char>(rest[0] >= 'A' && rest[0] <= 'Z' ? rest[0] - 'A' + 'a' : rest[0]) };
            if (drive != 'c')
            {
                result.push_back('/');
                result.push_back(drive);
            }
            rest.remove_prefix(2);
        }
        for (const char value : rest)
            result.push_back(value == '\\' ? '/' : value);
        if (result.empty())
            result.push_back('/');
        return result;
#endif
    }

    [[nodiscard]] inline std::u8string native_path(const std::u8string_view windows_path)
    {
        const std::string converted { native_path(std::string_view { reinterpret_cast<const char*>(windows_path.data()), windows_path.size() }) };
        return std::u8string { converted.begin(), converted.end() };
    }

    // 시험에 `\`로 적은 상대 경로(감시기가 Windows에서 알리는 꼴)를 이 OS의 구분자로 옮긴다. Windows에서는 그대로이고, POSIX에서는
    // `\`를 `/`로 바꾼다 (POSIX의 감시기와 git은 `/`만 쓴다).
    [[nodiscard]] inline std::u8string native_separators(const std::u8string_view windows_relative)
    {
        std::u8string result { windows_relative };
#if !defined(_WIN32)
        for (char8_t& value : result)
            if (value == u8'\\')
                value = u8'/';
#endif
        return result;
    }
} // namespace cyella::test
