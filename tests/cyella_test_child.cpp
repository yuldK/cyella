// process runner test가 띄우는 도우미 실행 파일이다. 인자를 앞에서부터 명령으로 읽어
// 차례로 수행하므로 `echo_stdin write_text done exit 3`처럼 이어 붙일 수 있다.
//
// 출력은 CRT가 아니라 Win32 handle에 직접 쓴다. CRT의 text 모드는 `\n`을 `\r\n`으로
// 바꾸고 NUL 이후를 자르는 경우가 있어, 원시 byte를 검증하는 test가 깨진다.

#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {
    void write_all(const DWORD standard_handle, const char* const data, const std::size_t size)
    {
        const HANDLE handle { GetStdHandle(standard_handle) };
        std::size_t offset { 0 };
        while (offset < size)
        {
            DWORD written { 0 };
            const DWORD length { static_cast<DWORD>(std::min<std::size_t>(size - offset, 64u * 1024u)) };
            if (WriteFile(handle, data + offset, length, &written, nullptr) == FALSE)
                return;
            offset += written;
        }
    }

    // `\n`, `\r`, `\0`, `\\`만 해석한다. NUL을 명령줄로 직접 넘길 수 없어서 필요하다.
    std::string unescape(const std::string_view text)
    {
        std::string result {};
        for (std::size_t index = 0; index < text.size(); ++index)
        {
            if (text[index] != '\\' || index + 1 >= text.size())
            {
                result.push_back(text[index]);
                continue;
            }
            ++index;
            switch (text[index])
            {
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case '0':
                result.push_back('\0');
                break;
            default:
                result.push_back(text[index]);
                break;
            }
        }
        return result;
    }

    void echo_standard_input()
    {
        const HANDLE input { GetStdHandle(STD_INPUT_HANDLE) };
        std::vector<char> buffer(64u * 1024u);
        while (true)
        {
            DWORD read_count { 0 };
            if (ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &read_count, nullptr) == FALSE || read_count == 0)
                return;
            write_all(STD_OUTPUT_HANDLE, buffer.data(), read_count);
        }
    }

    // byte i의 값이 `i % 256`이다. NUL(0)과 `\r`(13), `\n`(10)이 주기적으로 들어간다.
    void write_pattern_bytes(const std::size_t count)
    {
        std::string block(4096, '\0');
        std::size_t position { 0 };
        while (position < count)
        {
            const std::size_t length { std::min(block.size(), count - position) };
            for (std::size_t index = 0; index < length; ++index)
                block[index] = static_cast<char>((position + index) % 256u);
            write_all(STD_OUTPUT_HANDLE, block.data(), length);
            position += length;
        }
    }

    void write_numbered_lines(const std::size_t count)
    {
        for (std::size_t index = 0; index < count; ++index)
        {
            const std::string line { "line " + std::to_string(index) + "\n" };
            write_all(STD_ERROR_HANDLE, line.data(), line.size());
        }
    }

    // 자기 자신을 `sleep`으로 다시 띄우고 기다리지 않는다. 상속 가능한 handle을 그대로
    // 물려주므로 손자가 부모의 stdout·stderr pipe를 계속 잡고 있게 된다.
    void spawn_grandchild(const std::uint32_t milliseconds)
    {
        wchar_t module_path[MAX_PATH] {};
        GetModuleFileNameW(nullptr, module_path, MAX_PATH);
        std::wstring command_line { L"\"" };
        command_line.append(module_path);
        command_line.append(L"\" sleep ");
        command_line.append(std::to_wstring(milliseconds));

        STARTUPINFOW startup {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION created {};
        if (CreateProcessW(module_path, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &created) == FALSE)
            std::exit(97);
        CloseHandle(created.hThread);
        CloseHandle(created.hProcess);
    }
} // namespace

int wmain(const int argument_count, wchar_t** const wide_arguments)
{
    std::vector<std::string> arguments {};
    for (int index = 1; index < argument_count; ++index)
    {
        const int length { WideCharToMultiByte(CP_UTF8, 0, wide_arguments[index], -1, nullptr, 0, nullptr, nullptr) };
        std::string converted(static_cast<std::size_t>(std::max(length, 1)), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide_arguments[index], -1, converted.data(), length, nullptr, nullptr);
        converted.resize(converted.size() - 1);
        arguments.push_back(std::move(converted));
    }

    std::size_t position { 0 };
    const auto next_value = [&arguments, &position]() -> std::string {
        if (position >= arguments.size())
            std::exit(99);
        return arguments[position++];
    };

    while (position < arguments.size())
    {
        const std::string command { arguments[position++] };
        if (command == "echo_stdin")
            echo_standard_input();
        else if (command == "write_bytes")
            write_pattern_bytes(static_cast<std::size_t>(std::stoull(next_value())));
        else if (command == "write_text")
        {
            const std::string text { unescape(next_value()) };
            write_all(STD_OUTPUT_HANDLE, text.data(), text.size());
        }
        else if (command == "write_error_text")
        {
            const std::string text { unescape(next_value()) };
            write_all(STD_ERROR_HANDLE, text.data(), text.size());
        }
        else if (command == "error_lines")
            write_numbered_lines(static_cast<std::size_t>(std::stoull(next_value())));
        else if (command == "sleep")
            Sleep(static_cast<DWORD>(std::stoul(next_value())));
        else if (command == "grandchild")
            spawn_grandchild(static_cast<std::uint32_t>(std::stoul(next_value())));
        else if (command == "exit")
            return std::stoi(next_value());
        else
            return 96;
    }
    return 0;
}
