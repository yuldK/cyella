#include "platform/win32/win32_app_settings_path.h"

#include "domain/app_settings.h"
#include "platform/win32/utf8.h"
#include "platform/win32/win32_file_io.h"

#include <windows.h>

#include <KnownFolders.h>
#include <ShlObj.h>

#include <cstddef>
#include <cstdint>

namespace cyella::win32 {
    namespace {
        constexpr std::u8string_view app_settings_directory_name { u8"cyella" };
    } // namespace

    std::u8string app_settings_file_path_under(const std::u8string_view roaming_app_data)
    {
        std::u8string path { roaming_app_data };
        while (path.empty() == false && (path.back() == u8'\\' || path.back() == u8'/'))
            path.pop_back();
        if (path.empty())
            return {};

        path.push_back(u8'\\');
        path.append(app_settings_directory_name);
        path.push_back(u8'\\');
        path.append(app_settings_file_name);
        return path;
    }

    std::u8string app_settings_file_path()
    {
        try
        {
            PWSTR known_folder { nullptr };
            const HRESULT result { SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_CREATE, nullptr, &known_folder) };
            if (FAILED(result) || known_folder == nullptr)
            {
                CoTaskMemFree(known_folder);
                return {};
            }

            const auto converted { utf16_to_utf8(known_folder) };
            CoTaskMemFree(known_folder);
            if (converted.value.has_value() == false)
                return {};

            std::u8string path { app_settings_file_path_under(*converted.value) };
            if (path.empty())
                return {};

            // 폴더는 첫 실행에서 만들어 둔다. 저장이 한 번도 일어나지 않아도 사용자가 위치를 볼 수 있다.
            const prepared_path prepared { prepare_file_path(path) };
            if (prepared.value.has_value() == false)
                return {};
            const std::size_t separator { prepared.value->find_last_of(L'\\') };
            std::uint32_t native_error { 0 };
            if (create_directory_tree(prepared.value->substr(0, separator), native_error) == false)
                return {};
            return path;
        }
        catch (...)
        {
            return {};
        }
    }
} // namespace cyella::win32
