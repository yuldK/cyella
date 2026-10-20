#include "platform/win32/win32_git_environment.h"

#include "platform/win32/utf8.h"

#include <windows.h>

#include <optional>

namespace cyella::win32 {
    namespace {
        // 환경 변수의 값이다. 없거나 비어 있으면 값이 없다.
        std::optional<std::u8string> environment_value(const wchar_t* const name)
        {
            const DWORD needed { GetEnvironmentVariableW(name, nullptr, 0) };
            if (needed == 0)
                return std::nullopt;
            std::wstring buffer(needed, L'\0');
            const DWORD written { GetEnvironmentVariableW(name, buffer.data(), needed) };
            if (written == 0 || written >= needed)
                return std::nullopt;
            buffer.resize(written);
            const auto converted { utf16_to_utf8(buffer) };
            if (converted.value.has_value() == false || converted.value->empty())
                return std::nullopt;
            return *converted.value;
        }

        std::u8string joined(std::u8string base, const std::u8string_view tail)
        {
            if (base.empty() == false && base.back() != u8'\\' && base.back() != u8'/')
                base.push_back(u8'\\');
            base.append(tail);
            return base;
        }
    } // namespace

    std::vector<std::u8string> global_git_config_files()
    {
        if (const std::optional<std::u8string> explicit_file { environment_value(L"GIT_CONFIG_GLOBAL") }; explicit_file.has_value())
            return { *explicit_file };
        std::vector<std::u8string> files {};
        std::optional<std::u8string> home { environment_value(L"HOME") };
        if (home.has_value() == false)
            home = environment_value(L"USERPROFILE");
        if (home.has_value())
            files.push_back(joined(*home, u8".gitconfig"));
        if (const std::optional<std::u8string> xdg { environment_value(L"XDG_CONFIG_HOME") }; xdg.has_value())
            files.push_back(joined(joined(*xdg, u8"git"), u8"config"));
        else if (home.has_value())
            files.push_back(joined(joined(joined(*home, u8".config"), u8"git"), u8"config"));
        return files;
    }
} // namespace cyella::win32
