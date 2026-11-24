#include "platform/win32/win32_git_environment.h"

#include "platform/macos/posix_file.h"

#include <optional>

// `platform/win32/win32_git_environment.h`의 macOS 구현이다. git의 전역 설정 파일 규칙은 OS와 같다.
namespace cyella::win32 {
    namespace {
        // 환경 변수의 값이다. 없거나 비어 있으면 값이 없다.
        std::optional<std::u8string> non_empty_environment(const std::u8string_view name)
        {
            std::optional<std::u8string> value { macos::environment_value(name) };
            if (value.has_value() && value->empty())
                return std::nullopt;
            return value;
        }

        std::u8string joined(std::u8string base, const std::u8string_view tail)
        {
            if (base.empty() == false && base.back() != u8'/')
                base.push_back(u8'/');
            base.append(tail);
            return base;
        }
    } // namespace

    std::vector<std::u8string> global_git_config_files()
    {
        if (const std::optional<std::u8string> explicit_file { non_empty_environment(u8"GIT_CONFIG_GLOBAL") }; explicit_file.has_value())
            return { *explicit_file };
        std::vector<std::u8string> files {};
        std::u8string home { macos::home_directory() };
        if (home.empty() == false)
            files.push_back(joined(home, u8".gitconfig"));
        if (const std::optional<std::u8string> xdg { non_empty_environment(u8"XDG_CONFIG_HOME") }; xdg.has_value())
            files.push_back(joined(joined(*xdg, u8"git"), u8"config"));
        else if (home.empty() == false)
            files.push_back(joined(joined(joined(home, u8".config"), u8"git"), u8"config"));
        return files;
    }
} // namespace cyella::win32
