#include "platform/win32/win32_url_opener.h"

#include "domain/remote_web_link.h"
#include "platform/win32/utf8.h"

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <string>
#include <string_view>

namespace cyella::win32 {
    bool is_openable_web_url(const std::u8string_view url) noexcept
    {
        // 규칙은 domain 한곳에 둔다. presentation도 같은 규칙으로 링크를 열지 정해야 하기 때문이다.
        return is_safe_web_url(std::string_view { reinterpret_cast<const char*>(url.data()), url.size() });
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
