#include "platform/win32/win32_diff_tool_launcher.h"

#include "domain/path_syntax.h"
#include "domain/text_catalog.h"
#include "infrastructure/command_line_builder.h"
#include "infrastructure/git_tool_discovery.h"
#include "platform/win32/utf8.h"
#include "platform/win32/win32_file_probe.h"

#include <array>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace cyella::win32 {
    namespace {
        bool file_exists(const std::wstring& path)
        {
            const DWORD attributes { ::GetFileAttributesW(path.c_str()) };
            return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }

        std::u8string resolve_file_app(const std::u8string& executable)
        {
            if (executable != u8"Code.exe")
                return executable;
            constexpr wchar_t app_key[] { L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\Code.exe" };
            for (const HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE })
            {
                std::array<wchar_t, 32768> buffer {};
                DWORD bytes { static_cast<DWORD>(buffer.size() * sizeof(wchar_t)) };
                if (::RegGetValueW(root, app_key, nullptr, RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes) == ERROR_SUCCESS && file_exists(buffer.data()))
                    if (const auto text { utf16_to_utf8(buffer.data()) }; text.value.has_value())
                        return *text.value;
            }
            for (const wchar_t* variable : { L"LOCALAPPDATA", L"ProgramFiles", L"ProgramFiles(x86)" })
            {
                std::array<wchar_t, 32768> buffer {};
                const DWORD size { ::GetEnvironmentVariableW(variable, buffer.data(), static_cast<DWORD>(buffer.size())) };
                if (size == 0 || size >= buffer.size())
                    continue;
                const std::wstring folder { buffer.data() };
                for (const wchar_t* suffix : { L"\\Programs\\Microsoft VS Code\\Code.exe", L"\\Microsoft VS Code\\Code.exe" })
                {
                    const std::wstring candidate { folder + suffix };
                    if (file_exists(candidate))
                        if (const auto text { utf16_to_utf8(candidate) }; text.value.has_value())
                            return *text.value;
                }
            }
            return executable;
        }

        std::u8string join_directory(const std::u8string_view directory, const std::u8string_view name)
        {
            std::u8string path { directory };
            if (path.ends_with(u8'\\') == false && path.ends_with(u8'/') == false)
                path.push_back(u8'\\');
            path.append(name);
            return path;
        }

        // GetSystemDirectoryW와 GetSystemWindowsDirectoryW는 모양이 같아 하나로 읽는다. 실패하면 빈 값이고, 빈 값은 찾는 목록에서 빠진다.
        std::u8string read_system_folder(UINT(WINAPI* const query)(LPWSTR, UINT))
        {
            std::array<wchar_t, MAX_PATH + 1> buffer {};
            const UINT size { query(buffer.data(), static_cast<UINT>(buffer.size())) };
            if (size == 0 || size >= buffer.size())
                return {};
            const utf_conversion_result<std::u8string> text { utf16_to_utf8(buffer.data()) };
            return text.value.value_or(std::u8string {});
        }

        std::u8string application_folder()
        {
            std::array<wchar_t, 32768> buffer {};
            const DWORD size { ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size())) };
            if (size == 0 || size >= buffer.size())
                return {};
            const utf_conversion_result<std::u8string> text { utf16_to_utf8(buffer.data()) };
            if (text.value.has_value() == false)
                return {};
            return executable_directory(*text.value);
        }

        std::vector<std::u8string> current_tool_search_directories()
        {
            return tool_search_directories(application_folder(), read_system_folder(&::GetSystemDirectoryW), read_system_folder(&::GetSystemWindowsDirectoryW), read_environment_variable(u8"PATH"));
        }

        class win32_diff_tool_launcher final : public diff_tool_launcher
        {
        public:
            tool_launch_result launch(const tool_launch_request& request) override
            {
                if (request.executable.empty())
                    return { false, tr_narrow(u8"diff_tool.executable_empty") };

                // 이름만 적힌 도구를 CreateProcessW에 그대로 넘기면 부모의 현재 폴더를 먼저 찾으므로 여기서 절대 경로로 푼다.
                // 못 찾았을 때도 넘기지 않는다. 넘기면 결국 현재 폴더에서 찾는다. lpApplicationName은 계속 nullptr로 두어
                // .cmd·.bat 같은 도구가 사용자가 절대 경로를 적었을 때와 똑같이 뜨게 한다.
                const std::unique_ptr<file_probe> probe { make_file_probe() };
                const std::optional<std::u8string> executable { resolve_tool_executable(resolve_file_app(request.executable), current_tool_search_directories(), *probe) };
                if (executable.has_value() == false)
                    return { false, tr_narrow(u8"diff_tool.create_process_failed", { { u8"error", static_cast<DWORD>(ERROR_FILE_NOT_FOUND) } }) };

                const std::u8string command_line_text { build_windows_command_line(*executable, request.arguments) };
                const utf_conversion_result<std::wstring> command_line_wide { utf8_to_utf16(command_line_text) };
                if (command_line_wide.value.has_value() == false)
                    return { false, tr_narrow(u8"diff_tool.command_line_conversion_failed") };
                // CreateProcessW는 명령줄 버퍼를 고칠 수 있다.
                std::vector<wchar_t> command_line(command_line_wide.value->begin(), command_line_wide.value->end());
                command_line.push_back(L'\0');

                std::wstring directory {};
                if (request.working_directory.empty() == false)
                    if (const utf_conversion_result<std::wstring> wide { utf8_to_utf16(request.working_directory) }; wide.value.has_value())
                        directory = *wide.value;

                STARTUPINFOW startup {};
                startup.cb = sizeof(startup);
                PROCESS_INFORMATION process {};
                // 콘솔 앱(git)이 창을 깜빡이지 않게 하고, 앱이 끝나도 도구가 함께 끝나지 않게 새 프로세스 그룹에 둔다.
                const DWORD flags { CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP };
                const BOOL created { ::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, flags, nullptr, directory.empty() ? nullptr : directory.c_str(), &startup, &process) };
                if (created == FALSE)
                    return { false, tr_narrow(u8"diff_tool.create_process_failed", { { u8"error", ::GetLastError() } }) };
                ::CloseHandle(process.hThread);
                ::CloseHandle(process.hProcess);
                return { true, {} };
            }
        };
    } // namespace

    std::unique_ptr<diff_tool_launcher> make_diff_tool_launcher()
    {
        return std::make_unique<win32_diff_tool_launcher>();
    }

    std::vector<std::u8string> tool_search_directories(
        const std::u8string_view application_directory, const std::u8string_view system_directory, const std::u8string_view windows_directory, const std::u8string_view path_environment)
    {
        std::vector<std::u8string> directories {};
        for (const std::u8string_view directory : { application_directory, system_directory, windows_directory })
        {
            if (directory.empty() == false && is_absolute_windows_path(directory))
                directories.emplace_back(directory);
        }
        // split_search_path가 빈 항목·`.` 같은 상대 항목을 이미 버린다. git 탐색과 같은 규칙이다.
        for (std::u8string& directory : split_search_path(path_environment))
            directories.push_back(std::move(directory));
        return directories;
    }

    std::optional<std::u8string> resolve_tool_executable(const std::u8string_view executable, const std::vector<std::u8string>& directories, const file_probe& probe)
    {
        if (executable.find_first_of(u8"\\/:") != std::u8string_view::npos)
            return std::u8string { executable };

        std::u8string name { executable };
        if (name.find(u8'.') == std::u8string::npos)
            name.append(u8".exe");
        for (const std::u8string& directory : directories)
        {
            // probe는 절대 경로만 받는다. 상대 폴더가 섞여 와도 현재 폴더 기준으로 찾지 않게 건너뛴다.
            if (is_absolute_windows_path(directory) == false)
                continue;
            std::u8string candidate { join_directory(directory, name) };
            if (probe.probe(candidate) == path_kind::file)
                return candidate;
        }
        return std::nullopt;
    }

    std::optional<std::u8string> resolve_suggestion_executable(
        const std::u8string_view executable, const std::vector<std::u8string>& directories, const std::u8string_view user_profile, const file_probe& probe)
    {
        if (std::optional<std::u8string> found { resolve_tool_executable(executable, directories, probe) }; found.has_value())
            return found;

        std::u8string name { executable };
        for (char8_t& value : name)
            if (value >= u8'A' && value <= u8'Z')
                value = static_cast<char8_t>(value - u8'A' + u8'a');
        if (name != u8"claude" && name != u8"claude.exe")
            return std::nullopt;

        if (is_absolute_windows_path(user_profile))
        {
            std::u8string candidate { join_directory(user_profile, u8".local\\bin\\claude.exe") };
            if (probe.probe(candidate) == path_kind::file)
                return candidate;
        }
        for (const std::u8string& directory : directories)
        {
            if (is_absolute_windows_path(directory) == false || probe.probe(join_directory(directory, u8"claude.cmd")) != path_kind::file)
                continue;
            std::u8string candidate { join_directory(directory, u8"node_modules\\@anthropic-ai\\claude-code\\bin\\claude.exe") };
            if (probe.probe(candidate) == path_kind::file)
                return candidate;
        }
        return std::nullopt;
    }

    std::optional<std::u8string> resolve_suggestion_executable(const std::u8string_view executable)
    {
        const std::unique_ptr<file_probe> probe { make_file_probe() };
        return resolve_suggestion_executable(executable, current_tool_search_directories(), read_environment_variable(u8"USERPROFILE"), *probe);
    }
} // namespace cyella::win32
