#include "platform/win32/askpass_protocol.h"
#include "platform/win32/utf8.h"

#include <windows.h>

#include <shellapi.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// git과 OpenSSH가 비밀이나 예/아니요 답이 필요할 때 실행하는 작은 도우미다. 프롬프트 문구를
// `argv[1]`로 받아 앱의 named pipe에 묻고, 답을 표준 출력에 한 줄로 쓴 뒤 0으로 끝난다.
// 취소, 오류, 환경 변수 누락, 앱이 없는 경우는 아무것도 쓰지 않고 1로 끝난다.
//
// 콘솔 창을 띄우지 않으려고 WIN32 subsystem으로 만들며, 그래서 CRT stdio 대신 상속받은 표준
// 출력 핸들에 직접 쓴다. luil과 Skia에는 링크하지 않는다.
namespace cyella::win32 {
    namespace {
        constexpr int exit_answered { 0 };
        constexpr int exit_failed { 1 };
        constexpr std::wstring_view pipe_prefix { L"\\\\.\\pipe\\" };
        constexpr int connect_attempts { 5 };
        constexpr DWORD busy_wait_milliseconds { 2000u };

        std::optional<std::wstring> read_environment(const wchar_t* const name)
        {
            const DWORD needed { GetEnvironmentVariableW(name, nullptr, 0) };
            if (needed == 0)
                return std::nullopt;
            std::wstring value(needed, L'\0');
            const DWORD length { GetEnvironmentVariableW(name, value.data(), needed) };
            if (length == 0 || length >= needed)
                return std::nullopt;
            value.resize(length);
            return value;
        }

        std::optional<std::string> to_narrow_utf8(const std::wstring_view text)
        {
            const utf_conversion_result<std::u8string> converted { utf16_to_utf8(text) };
            if (converted.value.has_value() == false)
                return std::nullopt;
            return std::string { reinterpret_cast<const char*>(converted.value->data()), converted.value->size() };
        }

        // 첫 인자만 필요하다. 실행 파일 이름 뒤의 인자는 git과 ssh가 프롬프트 하나만 넘긴다.
        std::wstring prompt_argument()
        {
            int count { 0 };
            LPWSTR* const arguments { CommandLineToArgvW(GetCommandLineW(), &count) };
            if (arguments == nullptr)
                return {};
            std::wstring prompt { count >= 2 ? std::wstring { arguments[1] } : std::wstring {} };
            LocalFree(arguments);
            return prompt;
        }

        HANDLE connect_to_pipe(const std::wstring& pipe_name)
        {
            for (int attempt { 0 }; attempt < connect_attempts; ++attempt)
            {
                // 서버가 신원(impersonation) 수준을 올려 우리 토큰을 쓰지 못하게 식별 수준으로만 연다.
                const HANDLE pipe { CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr) };
                if (pipe != INVALID_HANDLE_VALUE)
                    return pipe;
                if (GetLastError() != ERROR_PIPE_BUSY || WaitNamedPipeW(pipe_name.c_str(), busy_wait_milliseconds) == FALSE)
                    break;
            }
            return INVALID_HANDLE_VALUE;
        }

        bool write_all(const HANDLE handle, const char* const data, const std::size_t size)
        {
            std::size_t done { 0 };
            while (done < size)
            {
                DWORD written { 0 };
                if (WriteFile(handle, data + done, static_cast<DWORD>(size - done), &written, nullptr) == FALSE || written == 0)
                    return false;
                done += written;
            }
            return true;
        }

        bool read_all(const HANDLE handle, char* const data, const std::size_t size)
        {
            std::size_t done { 0 };
            while (done < size)
            {
                DWORD read { 0 };
                if (ReadFile(handle, data + done, static_cast<DWORD>(size - done), &read, nullptr) == FALSE || read == 0)
                    return false;
                done += read;
            }
            return true;
        }

        void wipe(std::string& text) noexcept
        {
            if (text.empty() == false)
                SecureZeroMemory(text.data(), text.size());
            text.clear();
        }

        int run()
        {
            const std::optional<std::wstring> pipe_name { read_environment(L"CYELLA_ASKPASS_PIPE") };
            const std::optional<std::wstring> token { read_environment(L"CYELLA_ASKPASS_TOKEN") };
            const std::optional<std::wstring> operation { read_environment(L"CYELLA_ASKPASS_OPERATION") };
            if (pipe_name.has_value() == false || token.has_value() == false || operation.has_value() == false)
                return exit_failed;
            // 환경 변수가 pipe가 아닌 경로를 가리키면 토큰이 엉뚱한 곳에 쓰인다.
            if (pipe_name->starts_with(pipe_prefix) == false)
                return exit_failed;

            const std::optional<std::string> token_bytes { to_narrow_utf8(*token) };
            const std::optional<std::string> operation_bytes { to_narrow_utf8(*operation) };
            const std::optional<std::string> prompt_bytes { to_narrow_utf8(prompt_argument()) };
            if (token_bytes.has_value() == false || operation_bytes.has_value() == false || prompt_bytes.has_value() == false)
                return exit_failed;

            const std::optional<std::string> request { encode_askpass_request({ *token_bytes, *operation_bytes, *prompt_bytes }) };
            if (request.has_value() == false)
                return exit_failed;

            const HANDLE standard_output { GetStdHandle(STD_OUTPUT_HANDLE) };
            if (standard_output == nullptr || standard_output == INVALID_HANDLE_VALUE)
                return exit_failed;

            const HANDLE pipe { connect_to_pipe(*pipe_name) };
            if (pipe == INVALID_HANDLE_VALUE)
                return exit_failed;

            int exit_code { exit_failed };
            std::string header(askpass_reply_header_bytes, '\0');
            if (write_all(pipe, request->data(), request->size()) && read_all(pipe, header.data(), header.size()))
            {
                const std::optional<askpass_reply_header> reply { decode_askpass_reply_header(header) };
                if (reply.has_value() && reply->answered)
                {
                    // 답과 줄 바꿈을 한 번에 써서 중간에 끊긴 답이 나가지 않게 한다.
                    std::string answer(reply->answer_bytes, '\0');
                    if (read_all(pipe, answer.data(), answer.size()))
                    {
                        answer.push_back('\n');
                        if (write_all(standard_output, answer.data(), answer.size()))
                            exit_code = exit_answered;
                    }
                    wipe(answer);
                }
            }
            CloseHandle(pipe);
            return exit_code;
        }
    } // namespace
} // namespace cyella::win32

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    return cyella::win32::run();
}
