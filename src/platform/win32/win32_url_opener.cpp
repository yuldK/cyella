#include "platform/win32/win32_url_opener.h"

#include "platform/win32/utf8.h"

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <string>

namespace cyella::win32 {
    bool is_openable_web_url(const std::u8string_view url) noexcept
    {
        const bool scheme { url.starts_with(u8"https://") || url.starts_with(u8"http://") };
        return scheme && url.size() < 4096 && std::ranges::none_of(url, [](const char8_t value) { return value < 0x20 || value == u8' ' || value == 0x7f; });
    }

    bool open_url_in_browser(const std::u8string_view url) noexcept
    {
        if (is_openable_web_url(url) == false)
            return false;
        const auto wide { utf8_to_utf16(url) };
        if (wide.value.has_value() == false)
            return false;
        // 다른 스레드에서 셸 동작을 부르므로 COM을 이 스레드에서 잠시 연다. 이미 다른 모드로 열려 있으면 그대로 쓴다.
        const HRESULT initialized { ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE) };
        const HINSTANCE result { ::ShellExecuteW(nullptr, L"open", wide.value->c_str(), nullptr, nullptr, SW_SHOWNORMAL) };
        if (SUCCEEDED(initialized))
            ::CoUninitialize();
        return reinterpret_cast<INT_PTR>(result) > 32;
    }
} // namespace cyella::win32
