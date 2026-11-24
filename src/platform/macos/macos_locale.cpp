#include "platform/win32/win32_locale.h"

#include "infrastructure/json_app_settings_store.h"
#include "infrastructure/json_text_catalog.h"
#include "platform/macos/macos_resources.h"
#include "platform/win32/win32_settings_file_system.h"

#include <CoreFoundation/CoreFoundation.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

// `platform/win32/win32_locale.h`의 macOS 구현이다.
//  - 문구 파일은 실행 파일에 실린 바이트다 (macos_resources). Windows와 같은 파일을 싣고, 그 위에 macOS의 낱말로 바꾸는
//    덮어쓰기(`assets/locales/macos/*.json`)를 얹는다. 덮어쓰기는 원본에 있는 키만 바꾼다 (test가 본다).
//  - 시스템 언어는 사용자의 선호 언어 목록(시스템 설정 > 일반 > 언어 및 지역)의 첫 항목이다.
namespace cyella::win32 {
    namespace {
        // 원본 표에 덮어쓰기 표의 항목을 얹는다. 원본에 없는 키는 버린다 (덮어쓰기는 낱말만 바꾼다).
        text_catalog overlaid(text_catalog base, const std::string_view overlay_source)
        {
            const text_catalog overlay { parse_text_catalog_json(overlay_source, base.language).catalog };
            for (const auto& [key, value] : overlay.entries)
                if (const auto found { base.entries.find(key) }; found != base.entries.end())
                    found->second = value;
            return base;
        }
    } // namespace

    std::string_view locale_resource(const ui_language language) noexcept
    {
        return macos::embedded_bytes(language == ui_language::english ? macos::embedded_resource::locale_english : macos::embedded_resource::locale_korean);
    }

    ui_language system_ui_language() noexcept
    {
        const CFArrayRef languages { CFLocaleCopyPreferredLanguages() };
        if (languages == nullptr)
            return ui_language::english;
        ui_language result { ui_language::english };
        if (CFArrayGetCount(languages) > 0)
        {
            const auto first { static_cast<CFStringRef>(CFArrayGetValueAtIndex(languages, 0)) };
            // `ko`, `ko-KR`, `ko-Kore-KR` 꼴이다. 언어 부분만 본다.
            if (first != nullptr && CFStringHasPrefix(first, CFSTR("ko")))
                result = ui_language::korean;
        }
        CFRelease(languages);
        return result;
    }

    language_preference read_language_preference(const std::u8string_view settings_path)
    {
        if (settings_path.empty())
            return language_preference::system;
        const std::unique_ptr<settings_file_system> files { make_settings_file_system() };
        const settings_file_read_result read { files->read(settings_path) };
        if (read.state != settings_file_read_state::available)
            return language_preference::system;
        return parse_app_settings_json(read.bytes).settings.language;
    }

    void install_ui_language(const ui_language language)
    {
        const std::string_view korean_overlay { macos::embedded_bytes(macos::embedded_resource::locale_korean_macos) };
        text_catalog base { overlaid(parse_text_catalog_json(locale_resource(ui_language::korean), ui_language::korean).catalog, korean_overlay) };
        std::optional<text_catalog> active {};
        if (language == ui_language::english)
        {
            const std::string_view english_overlay { macos::embedded_bytes(macos::embedded_resource::locale_english_macos) };
            active = overlaid(parse_text_catalog_json(locale_resource(ui_language::english), ui_language::english).catalog, english_overlay);
        }
        install_text_catalogs(std::move(base), std::move(active));
    }
} // namespace cyella::win32
