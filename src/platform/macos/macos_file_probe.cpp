#include "platform/win32/win32_file_probe.h"

#include "platform/macos/posix_file.h"

#include <sys/stat.h>

#include <string>
#include <utility>
#include <vector>

// `platform/win32/win32_file_probe.h`의 macOS 구현이다 (stat·getenv).
namespace cyella::win32 {
    namespace {
        class posix_file_probe final : public file_probe
        {
        public:
            [[nodiscard]] path_kind probe(const std::u8string_view absolute_path) const noexcept override
            {
                try
                {
                    if (absolute_path.empty())
                        return path_kind::missing;
                    const std::string path { macos::to_narrow(absolute_path) };
                    // 경로 가운데의 NUL은 C 경로에서 잘려 다른 파일을 가리킨다.
                    if (path.find('\0') != std::string::npos)
                        return path_kind::missing;
                    struct stat info {};
                    if (::stat(path.c_str(), &info) != 0)
                        return path_kind::missing;
                    return S_ISDIR(info.st_mode) ? path_kind::directory : path_kind::file;
                }
                catch (...)
                {
                    return path_kind::missing;
                }
            }
        };
    } // namespace

    std::unique_ptr<file_probe> make_file_probe()
    {
        return std::make_unique<posix_file_probe>();
    }

    std::u8string read_environment_variable(const std::u8string_view name) noexcept
    {
        try
        {
            return macos::environment_value(name).value_or(std::u8string {});
        }
        catch (...)
        {
            return {};
        }
    }

    git_tool_environment current_git_tool_environment() noexcept
    {
        try
        {
            git_tool_environment environment {};
            environment.path_environment = read_environment_variable(u8"PATH");
            // Finder·Dock에서 띄운 앱의 `PATH`는 `/usr/bin:/bin:/usr/sbin:/sbin`뿐이다. Homebrew의 git(Apple Silicon은
            // `/opt/homebrew`, Intel은 `/usr/local`)은 `PATH` 다음 차례로 본다. `/usr/bin/git`은 Xcode 명령줄 도구의 git이다.
            environment.program_files_directories = { u8"/opt/homebrew", u8"/usr/local" };
            return environment;
        }
        catch (...)
        {
            return {};
        }
    }
} // namespace cyella::win32
