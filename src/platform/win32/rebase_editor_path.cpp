#include "platform/win32/rebase_editor_path.h"

#include <windows.h>

#include <string>
#include <system_error>

namespace cyella::win32 {
    std::optional<std::filesystem::path> locate_rebase_editor_helper()
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
                const std::filesystem::path helper { std::filesystem::path { buffer }.parent_path() / rebase_editor_file_name };
                std::error_code error {};
                if (std::filesystem::is_regular_file(helper, error) == false || error)
                    return std::nullopt;
                return helper;
            }
            buffer.resize(buffer.size() * 2u);
        }
        return std::nullopt;
    }
} // namespace cyella::win32
