#pragma once

#include "application/file_probe.h"
#include "application/process_cancellation.h"
#include "application/process_runner.h"
#include "infrastructure/ssh_tools.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 환경 설정 SSH 화면이 쓰는 도구들의 위치다. 비어 있으면 못 찾은 것이다.
    struct ssh_programs
    {
        std::u8string ssh {};
        std::u8string ssh_add {};
        std::u8string ssh_keygen {};
        // 어느 쪽을 골랐는지 화면에 보이려는 이름이다 ("Git for Windows", "Windows OpenSSH").
        std::u8string origin {};

        [[nodiscard]] bool found() const noexcept
        {
            return ssh.empty() == false && ssh_keygen.empty() == false;
        }
    };

    // git이 쓰는 ssh와 같은 것을 고른다. git 실행 파일이 있는 Git for Windows의 `usr\bin`을 먼저 보고(git은 자기 ssh를 쓴다),
    // 없으면 시스템의 OpenSSH(`<windows>\System32\OpenSSH`)다. `windows_directory`가 비면 시스템 쪽은 건너뛴다.
    [[nodiscard]] ssh_programs locate_ssh_programs(std::u8string_view git_executable, std::u8string_view windows_directory, const file_probe& probe);

    // 화면에 보일 키 한 쌍이다. 공개 키 한 줄은 "공개 키 복사"가 그대로 클립보드에 쓴다 (개인 키 파일은 열지 않는다).
    struct ssh_key_entry
    {
        std::u8string name {};
        std::u8string key_type {};
        std::u8string comment {};
        std::u8string public_key_line {};
        std::u8string private_key_path {};

        [[nodiscard]] bool operator==(const ssh_key_entry&) const noexcept = default;
    };

    struct ssh_info_snapshot
    {
        ssh_agent_service_status service { ssh_agent_service_status::not_installed };
        // `ssh-add -l`을 부를 수 없었다 (실행 파일을 못 찾았다).
        bool agent_list_available { false };
        ssh_agent_list agent {};
        std::vector<ssh_key_entry> keys {};
        // `%USERPROFILE%\.ssh`. 위치를 모르면 비어 있다.
        std::u8string ssh_directory {};
    };

    struct ssh_info_sources
    {
        std::function<ssh_agent_service_status()> service_status {};
        process_runner* runner { nullptr };
        std::u8string ssh_add_executable {};
        std::filesystem::path ssh_directory {};
        process_cancellation_token cancellation {};
    };

    // 서비스 상태, `ssh-add -l`, `~/.ssh`의 키 쌍을 읽는다. 프로세스를 띄우고 파일을 읽으므로 worker에서 부른다.
    // 어떤 실패도 값으로 돌려주고 예외를 던지지 않는다.
    [[nodiscard]] ssh_info_snapshot collect_ssh_info(const ssh_info_sources& sources) noexcept;
} // namespace cyella
