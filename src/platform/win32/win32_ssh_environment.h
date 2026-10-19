#pragma once

#include "infrastructure/ssh_tools.h"

#include <filesystem>
#include <optional>

namespace cyella::win32 {
    // Windows `ssh-agent` 서비스의 상태를 서비스 제어 관리자에서 읽는다. 시작 권한은 필요 없는 조회
    // 권한만 연다. 이 서비스는 시스템 OpenSSH(`C:\Windows\System32\OpenSSH`)의 에이전트다. Git for
    // Windows의 ssh는 `SSH_AUTH_SOCK`으로 다른 에이전트를 쓸 수 있어 둘이 같지 않을 수 있다.
    [[nodiscard]] ssh_agent_service_snapshot query_ssh_agent_service_snapshot() noexcept;
    [[nodiscard]] ssh_agent_service_status query_ssh_agent_service_status() noexcept;

    // `%USERPROFILE%\.ssh`다. 사용자 프로필 폴더를 알 수 없으면 값이 없다.
    [[nodiscard]] std::optional<std::filesystem::path> user_ssh_directory();

    // `%SystemRoot%`(보통 `C:\Windows`)다. 시스템 OpenSSH(`System32\OpenSSH`)를 찾는 데 쓴다. 모르면 비어 있다.
    [[nodiscard]] std::u8string windows_directory();

    // 새 키의 주석 기본값 `사용자@컴퓨터`다. 이름을 알 수 없으면 `cyella`다.
    [[nodiscard]] std::u8string default_ssh_key_comment();
} // namespace cyella::win32
