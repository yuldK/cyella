#include "platform/win32/win32_locale.h"

#include "infrastructure/json_app_settings_store.h"
#include "infrastructure/json_text_catalog.h"
#include "platform/win32/resources/resource_ids.h"
#include "platform/win32/win32_settings_file_system.h"

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace cyella::win32 {
    namespace {
        // 실행 파일에 실린 RCDATA의 바이트다. resource는 프로세스가 끝날 때까지 살아 있다.
        std::string_view resource_bytes(const int id) noexcept
        {
            const HMODULE module { ::GetModuleHandleW(nullptr) };
            const HRSRC resource { ::FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA) };
            if (resource == nullptr)
                return {};
            const HGLOBAL loaded { ::LoadResource(module, resource) };
            const DWORD size { ::SizeofResource(module, resource) };
            const void* const data { loaded != nullptr ? ::LockResource(loaded) : nullptr };
            if (data == nullptr || size == 0)
                return {};
            return { static_cast<const char*>(data), static_cast<std::size_t>(size) };
        }
    } // namespace

    std::string_view locale_resource(const ui_language language) noexcept
    {
        return resource_bytes(language == ui_language::english ? IDR_LOCALE_EN : IDR_LOCALE_KO);
    }

    ui_language system_ui_language() noexcept
    {
        return PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_KOREAN ? ui_language::korean : ui_language::english;
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
        text_catalog base { parse_text_catalog_json(locale_resource(ui_language::korean), ui_language::korean).catalog };
        std::optional<text_catalog> active {};
        if (language == ui_language::english)
            active = parse_text_catalog_json(locale_resource(ui_language::english), ui_language::english).catalog;
        install_text_catalogs(std::move(base), std::move(active));
    }
} // namespace cyella::win32
