#include "platform/win32/win32_webview_support.h"

#include <windows.h>

#include <KnownFolders.h>
#include <ShlObj.h>

#include <filesystem>
#include <vector>

// WebView2 로더(정적 라이브러리)가 내보내는 함수다. luil이 링크한 로더에서 가져온다. SDK 헤더를 앱 쪽에 들이지 않으려고
// 선언만 둔다.
extern "C" HRESULT STDAPICALLTYPE GetAvailableCoreWebView2BrowserVersionString(PCWSTR browserExecutableFolder, LPWSTR* versionInfo);

namespace cyella::win32 {
    bool webview_runtime_installed() noexcept
    {
        LPWSTR version { nullptr };
        const HRESULT result { GetAvailableCoreWebView2BrowserVersionString(nullptr, &version) };
        const bool installed { SUCCEEDED(result) && version != nullptr && version[0] != L'\0' };
        if (version != nullptr)
            CoTaskMemFree(version);
        return installed;
    }

    viewer_location locate_diff_viewer() noexcept
    {
        try
        {
            std::vector<wchar_t> buffer(32768);
            const DWORD length { GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())) };
            if (length == 0 || length >= buffer.size())
                return {};
            const std::filesystem::path index { std::filesystem::path { std::wstring { buffer.data(), length } }.parent_path() / L"diff-viewer" / L"index.html" };
            std::error_code error {};
            viewer_location location {};
            location.index_path = index.u8string();
            location.exists = std::filesystem::is_regular_file(index, error) && error.value() == 0;
            return location;
        }
        catch (...)
        {
            return {};
        }
    }

    std::u8string webview_user_data_directory() noexcept
    {
        try
        {
            PWSTR known_folder { nullptr };
            const HRESULT result { SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &known_folder) };
            if (FAILED(result) || known_folder == nullptr)
            {
                CoTaskMemFree(known_folder);
                return {};
            }
            const std::filesystem::path path { std::filesystem::path { known_folder } / L"cyella" / L"webview" };
            CoTaskMemFree(known_folder);
            return path.u8string();
        }
        catch (...)
        {
            return {};
        }
    }

    bool system_prefers_light_theme() noexcept
    {
        DWORD value { 1 };
        DWORD size { sizeof(value) };
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
            return true;
        return value != 0;
    }

    bool high_contrast_enabled() noexcept
    {
        HIGHCONTRASTW contrast {};
        contrast.cbSize = sizeof(contrast);
        if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) == FALSE)
            return false;
        return (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
    }
} // namespace cyella::win32
