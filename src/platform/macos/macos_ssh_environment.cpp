#include "platform/win32/win32_ssh_environment.h"

#include "platform/macos/posix_file.h"

#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <string>

// `platform/win32/win32_ssh_environment.h`의 macOS 구현이다.
//  - macOS의 ssh-agent는 서비스가 아니라 launchd가 사용자 세션에 띄우는 에이전트다. 그 소켓 경로가 `SSH_AUTH_SOCK`에 있고
//    (Finder에서 띄운 앱도 받는다), 첫 접속에 launchd가 에이전트를 깨운다. 그래서 "설치됨·실행 중"은 그 소켓이 있는가로 본다.
//  - 시스템 OpenSSH는 `/usr/bin`에 있어 Windows 폴더 같은 기준이 없다. `windows_directory`는 빈 값이다.
namespace cyella::win32 {
    ssh_agent_service_snapshot query_ssh_agent_service_snapshot() noexcept
    {
        ssh_agent_service_snapshot snapshot {};
        try
        {
            const std::optional<std::u8string> socket { macos::environment_value(u8"SSH_AUTH_SOCK") };
            if (socket.has_value() == false || socket->empty())
                return snapshot;
            snapshot.installed = true;
            struct stat info {};
            const std::string path { macos::to_narrow(*socket) };
            snapshot.running = ::stat(path.c_str(), &info) == 0 && S_ISSOCK(info.st_mode);
        }
        catch (...)
        {
            return {};
        }
        return snapshot;
    }

    ssh_agent_service_status query_ssh_agent_service_status() noexcept
    {
        return classify_ssh_agent_service(query_ssh_agent_service_snapshot());
    }

    std::optional<std::filesystem::path> user_ssh_directory()
    {
        const std::u8string home { macos::home_directory() };
        if (home.empty())
            return std::nullopt;
        return std::filesystem::path { home } / ".ssh";
    }

    std::u8string windows_directory()
    {
        return {};
    }

    std::u8string default_ssh_key_comment()
    {
        try
        {
            const std::optional<std::u8string> user { macos::environment_value(u8"USER") };
            char host[_POSIX_HOST_NAME_MAX + 1] {};
            if (user.has_value() == false || user->empty() || ::gethostname(host, sizeof(host)) != 0 || host[0] == '\0')
                return u8"cyella";
            // `my-mac.local`의 `.local`은 mDNS 이름의 꼬리라 떼어 Windows의 컴퓨터 이름과 같은 모양으로 둔다.
            std::string name { host };
            if (name.ends_with(".local"))
                name.resize(name.size() - 6);
            return *user + u8"@" + macos::to_u8(name);
        }
        catch (...)
        {
            return u8"cyella";
        }
    }
} // namespace cyella::win32
