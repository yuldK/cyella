#include "platform/win32/win32_url_opener.h"

#include "domain/remote_web_link.h"

#include <CoreServices/CoreServices.h>

// `platform/win32/win32_url_opener.h`의 macOS 구현이다. Launch Services가 기본 브라우저로 연다. 어느 thread에서 불러도 된다.
namespace cyella::win32 {
    bool is_openable_web_url(const std::u8string_view url) noexcept
    {
        return is_safe_web_url(std::string_view { reinterpret_cast<const char*>(url.data()), url.size() });
    }

    bool open_url_in_browser(const std::u8string_view url) noexcept
    {
        if (is_openable_web_url(url) == false)
            return false;
        const CFURLRef address { CFURLCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(url.data()), static_cast<CFIndex>(url.size()), kCFStringEncodingUTF8, nullptr) };
        if (address == nullptr)
            return false;
        const OSStatus status { LSOpenCFURLRef(address, nullptr) };
        CFRelease(address);
        return status == noErr;
    }
} // namespace cyella::win32
