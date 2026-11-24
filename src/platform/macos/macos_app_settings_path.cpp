#include "platform/win32/win32_app_settings_path.h"

#include "domain/app_settings.h"
#include "platform/macos/posix_file.h"

// `platform/win32/win32_app_settings_path.h`의 macOS 구현이다. Windows의 로밍 AppData 자리는 macOS에서
// `~/Library/Application Support`다. 설정은 그 아래 `cyella/settings.json`이고, 창 배치도 같은 폴더에 둔다.
namespace cyella::win32 {
    namespace {
        constexpr std::u8string_view app_settings_directory_name { u8"cyella" };
    } // namespace

    std::u8string app_settings_file_path_under(const std::u8string_view roaming_app_data)
    {
        std::u8string path { roaming_app_data };
        while (path.size() > 1 && path.back() == u8'/')
            path.pop_back();
        if (path.empty())
            return {};
        if (path != u8"/")
            path.push_back(u8'/');
        path.append(app_settings_directory_name);
        path.push_back(u8'/');
        path.append(app_settings_file_name);
        return path;
    }

    std::u8string roaming_app_data_folder()
    {
        try
        {
            return macos::library_directory(u8"Application Support");
        }
        catch (...)
        {
            return {};
        }
    }

    std::u8string app_settings_file_path()
    {
        try
        {
            const std::u8string roaming { roaming_app_data_folder() };
            if (roaming.empty())
                return {};
            return prepared_app_settings_file_path_under(roaming);
        }
        catch (...)
        {
            return {};
        }
    }

    std::u8string prepared_app_settings_file_path_under(const std::u8string_view roaming_app_data)
    {
        try
        {
            const std::u8string path { app_settings_file_path_under(roaming_app_data) };
            if (path.empty())
                return {};
            // 폴더는 첫 실행에서 만들어 둔다. 저장이 한 번도 일어나지 않아도 사용자가 위치를 볼 수 있다.
            int error { 0 };
            if (macos::create_directory_tree(path.substr(0, path.find_last_of(u8'/')), error) == false)
                return {};
            return path;
        }
        catch (...)
        {
            return {};
        }
    }
} // namespace cyella::win32
