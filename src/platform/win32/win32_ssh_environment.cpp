#include "platform/win32/win32_ssh_environment.h"

#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

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
} // namespace cyella::win32
