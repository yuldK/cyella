#include "platform/win32/win32_diff_tool_launcher.h"

#include "infrastructure/command_line_builder.h"
#include "platform/win32/utf8.h"

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
        class win32_diff_tool_launcher final : public diff_tool_launcher
        {
        public:
            tool_launch_result launch(const tool_launch_request& request) override
            {
                if (request.executable.empty())
                    return { false, "실행 파일이 비어 있습니다." };

                const std::u8string command_line_text { build_windows_command_line(request.executable, request.arguments) };
                const utf_conversion_result<std::wstring> command_line_wide { utf8_to_utf16(command_line_text) };
                if (command_line_wide.value.has_value() == false)
                    return { false, "명령줄을 UTF-16으로 바꾸지 못했습니다." };
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
                    return { false, "CreateProcess 실패 (오류 " + std::to_string(::GetLastError()) + ")" };
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
