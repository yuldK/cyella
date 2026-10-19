#include "platform/win32/askpass_helper_path.h"

#include <windows.h>

#include <string>
#include <system_error>

namespace cyella::win32 {
    namespace {
        std::optional<std::filesystem::path> current_executable_path()
        {
            // 긴 경로가 잘리면 버퍼를 두 배로 늘려 다시 읽는다.
            std::wstring buffer(MAX_PATH, L'\0');
            for (int attempt { 0 }; attempt < 6; ++attempt)
            {
                const DWORD length { GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())) };
                if (length == 0)
                    return std::nullopt;
                if (length < buffer.size())
                {
                    buffer.resize(length);
                    return std::filesystem::path { buffer };
                }
                buffer.resize(buffer.size() * 2u);
            }
            return std::nullopt;
        }
    } // namespace

    std::filesystem::path askpass_helper_path_beside(const std::filesystem::path& executable_directory)
    {
        return executable_directory / askpass_helper_file_name;
    }

    std::optional<std::filesystem::path> locate_askpass_helper()
    {
        const std::optional<std::filesystem::path> executable { current_executable_path() };
        if (executable.has_value() == false)
            return std::nullopt;

        std::filesystem::path helper { askpass_helper_path_beside(executable->parent_path()) };
        std::error_code error {};
        if (std::filesystem::is_regular_file(helper, error) == false || error)
            return std::nullopt;
        return helper;
    }
} // namespace cyella::win32
