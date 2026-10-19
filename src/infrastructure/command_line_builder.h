#pragma once

#include <span>
#include <string>
#include <string_view>

namespace cyella {
    // Win32는 인자 배열을 직접 받는 프로세스 시작 API가 없으므로 `CommandLineToArgvW`
    // 규칙에 맞는 명령줄 하나를 만든다. 셸을 거치지 않기 때문에 `&`, `|`, `^` 같은
    // 문자는 특별한 의미 없이 인자 값의 일부로 전달된다.
    [[nodiscard]] std::u8string build_windows_command_line(std::u8string_view executable, std::span<const std::u8string> arguments);
    // 하나의 인자를 위 규칙으로 인용해 덧붙인다. 명령줄 조립 test와 재사용을 위해 노출한다.
    void append_windows_command_line_argument(std::u8string& target, std::u8string_view argument);
} // namespace cyella
