#include "platform/win32/askpass_helper_path.h"
#include "platform/win32/rebase_editor_path.h"

#include "platform/macos/posix_file.h"

#include <system_error>

// `platform/win32/askpass_helper_path.h`·`rebase_editor_path.h`의 macOS 구현이다. 두 도우미는 앱 실행 파일 옆
// (`Cyella.app/Contents/MacOS`)에 놓인다.
namespace cyella::win32 {
    namespace {
        std::optional<std::filesystem::path> helper_beside_executable(const char* const name)
        {
            const std::u8string directory { macos::current_executable_directory() };
            if (directory.empty())
                return std::nullopt;
            const std::filesystem::path helper { std::filesystem::path { directory } / name };
            std::error_code error {};
            if (std::filesystem::is_regular_file(helper, error) == false || error)
                return std::nullopt;
            return helper;
        }
    } // namespace

    std::filesystem::path askpass_helper_path_beside(const std::filesystem::path& executable_directory)
    {
        return executable_directory / askpass_helper_file_name;
    }

    std::optional<std::filesystem::path> locate_askpass_helper()
    {
        return helper_beside_executable(askpass_helper_file_name);
    }

    std::optional<std::filesystem::path> locate_rebase_editor_helper()
    {
        return helper_beside_executable(rebase_editor_file_name);
    }
} // namespace cyella::win32
