#include "platform/win32/win32_webview_support.h"

#include "platform/macos/posix_file.h"

#include <filesystem>
#include <system_error>

// `platform/win32/win32_webview_support.h`의 macOS 구현이다.
//  - 웹뷰는 시스템의 WKWebView다. 따로 설치하는 Runtime이 없으므로 늘 있다고 답한다.
//  - 뷰어는 앱 번들의 `Contents/Resources/diff-viewer`에 있다 (코드가 아닌 파일은 `Contents/MacOS`에 둘 수 없다).
//  - Windows의 LocalAppData 자리(지워도 되는 보관 자료)는 `~/Library/Caches/cyella`다.
namespace cyella::win32 {
    bool webview_runtime_installed() noexcept
    {
        return true;
    }

    viewer_location locate_diff_viewer() noexcept
    {
        try
        {
            const std::u8string resources { macos::resources_directory() };
            if (resources.empty())
                return {};
            const std::filesystem::path index { std::filesystem::path { resources } / "diff-viewer" / "index.html" };
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

    std::u8string local_data_directory(const std::wstring_view name) noexcept
    {
        try
        {
            const std::u8string caches { macos::library_directory(u8"Caches") };
            if (caches.empty())
                return {};
            // 이름은 코드에 적은 ASCII 상수(`L"avatars"` 등)다. 넓은 글자를 한 바이트씩 옮긴다.
            std::u8string path { caches + u8"/cyella/" };
            for (const wchar_t character : name)
            {
                if (character <= 0 || character >= 0x80)
                    return {};
                path.push_back(static_cast<char8_t>(character));
            }
            return path;
        }
        catch (...)
        {
            return {};
        }
    }

    std::u8string webview_user_data_directory() noexcept
    {
        return local_data_directory(L"webview");
    }
} // namespace cyella::win32
