#include "platform/win32/win32_ssh_environment.h"

#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

#include <iterator>
#include <string>
#include <vector>

namespace cyella::win32 {
    namespace {
        constexpr const wchar_t* ssh_agent_service_name { L"ssh-agent" };

        class service_handle
        {
        public:
            explicit service_handle(const SC_HANDLE handle) noexcept
                : handle_ { handle }
            {}

            service_handle(const service_handle&) = delete;
            service_handle& operator=(const service_handle&) = delete;

            ~service_handle()
            {
                if (handle_ != nullptr)
                    CloseServiceHandle(handle_);
            }

            [[nodiscard]] SC_HANDLE get() const noexcept
            {
                return handle_;
            }

        private:
            SC_HANDLE handle_ { nullptr };
        };
    } // namespace

    ssh_agent_service_snapshot query_ssh_agent_service_snapshot() noexcept
    {
        ssh_agent_service_snapshot snapshot {};
        const service_handle manager { OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT) };
        if (manager.get() == nullptr)
            return snapshot;

        const service_handle service { OpenServiceW(manager.get(), ssh_agent_service_name, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG) };
        if (service.get() == nullptr)
            return snapshot;
        snapshot.installed = true;

        SERVICE_STATUS_PROCESS status {};
        DWORD needed { 0 };
        if (QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed) != FALSE)
            snapshot.running = status.dwCurrentState == SERVICE_RUNNING || status.dwCurrentState == SERVICE_START_PENDING;

        QueryServiceConfigW(service.get(), nullptr, 0, &needed);
        if (needed != 0)
        {
            std::vector<unsigned char> buffer(needed);
            auto* const config { reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data()) };
            if (QueryServiceConfigW(service.get(), config, needed, &needed) != FALSE)
                snapshot.start_disabled = config->dwStartType == SERVICE_DISABLED;
        }
        return snapshot;
    }

    ssh_agent_service_status query_ssh_agent_service_status() noexcept
    {
        return classify_ssh_agent_service(query_ssh_agent_service_snapshot());
    }

    std::optional<std::filesystem::path> user_ssh_directory()
    {
        PWSTR profile { nullptr };
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &profile)))
        {
            CoTaskMemFree(profile);
            return std::nullopt;
        }
        std::filesystem::path directory { std::filesystem::path { profile } / L".ssh" };
        CoTaskMemFree(profile);
        return directory;
    }

    std::u8string windows_directory()
    {
        wchar_t buffer[MAX_PATH] {};
        const UINT length { GetSystemWindowsDirectoryW(buffer, MAX_PATH) };
        if (length == 0 || length >= MAX_PATH)
            return {};
        return std::filesystem::path { std::wstring { buffer, length } }.u8string();
    }

    std::u8string default_ssh_key_comment()
    {
        wchar_t user[256] {};
        DWORD user_length { static_cast<DWORD>(std::size(user)) };
        wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1] {};
        DWORD computer_length { static_cast<DWORD>(std::size(computer)) };
        // GetUserNameW의 길이는 끝의 NUL을 센다.
        const bool has_user { GetUserNameW(user, &user_length) != FALSE && user_length > 1 };
        const bool has_computer { GetComputerNameW(computer, &computer_length) != FALSE && computer_length > 0 };
        if (has_user == false || has_computer == false)
            return u8"cyella";
        return std::filesystem::path { std::wstring { user, user_length - 1 } + L"@" + std::wstring { computer, computer_length } }.u8string();
    }
} // namespace cyella::win32
