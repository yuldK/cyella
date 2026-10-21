#include "platform/win32/win32_diff_tool_launcher.h"

#include "domain/text_catalog.h"
#include "infrastructure/command_line_builder.h"
#include "platform/win32/utf8.h"

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

        class win32_diff_tool_launcher final : public diff_tool_launcher
        {
        public:
            tool_launch_result launch(const tool_launch_request& request) override
            {
                if (request.executable.empty())
                    return { false, tr_narrow(u8"diff_tool.executable_empty") };

                const std::u8string command_line_text { build_windows_command_line(resolve_file_app(request.executable), request.arguments) };
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
} // namespace cyella::win32
