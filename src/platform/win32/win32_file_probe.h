#pragma once

#include "application/file_probe.h"
#include "infrastructure/git_tool_discovery.h"

#include <memory>
#include <string>
#include <string_view>

namespace cyella::win32 {
    // 실제 filesystem을 조회하는 `file_probe`를 만든다. 상태가 없으므로 여러 스레드에서
    // 같은 instance를 써도 안전하다.
    [[nodiscard]] std::unique_ptr<file_probe> make_file_probe();

    // 환경 변수 값을 UTF-8로 읽는다. 없거나 변환할 수 없으면 빈 문자열이다.
    [[nodiscard]] std::u8string read_environment_variable(std::u8string_view name) noexcept;

    // 현재 프로세스의 `PATH`와 Program Files 위치로 git 탐색 환경을 채운다.
    [[nodiscard]] git_tool_environment current_git_tool_environment() noexcept;
} // namespace cyella::win32
