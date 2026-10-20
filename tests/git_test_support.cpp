#include "git_test_support.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <system_error>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifndef CYELLA_TEST_DATA_DIRECTORY
#error "CYELLA_TEST_DATA_DIRECTORY must be defined by the build."
#endif

namespace cyella {
    namespace test {
        namespace {
            std::wstring widen(const std::string_view text)
            {
                if (text.empty())
                    return {};
                const int size { ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0) };
                std::wstring result(static_cast<std::size_t>(size), L'\0');
                ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
                return result;
            }

            // CommandLineToArgvW가 되돌릴 수 있게 인용한다.
            void append_quoted(std::wstring& command_line, const std::wstring& argument)
            {
                if (command_line.empty() == false)
                    command_line.push_back(L' ');
                command_line.push_back(L'"');
                std::size_t backslashes { 0 };
                for (const wchar_t value : argument)
                {
                    if (value == L'\\')
                    {
                        ++backslashes;
                        continue;
                    }
                    if (value == L'"')
                        command_line.append(backslashes * 2 + 1, L'\\');
                    else
                        command_line.append(backslashes, L'\\');
                    backslashes = 0;
                    command_line.push_back(value);
                }
                command_line.append(backslashes * 2, L'\\');
                command_line.push_back(L'"');
            }

            void set_environment(const char* name, const char* value)
            {
                ::_putenv_s(name, value);
            }

            // 첫 호출에서 한 번만 git 환경을 정한다. 자식 프로세스가 이 환경을 상속한다.
            void configure_environment_once()
            {
                static const bool configured = [] {
                    set_environment("LC_ALL", "C");
                    set_environment("LANG", "C");
                    set_environment("GIT_CONFIG_NOSYSTEM", "1");
                    set_environment("GIT_TERMINAL_PROMPT", "0");
                    set_environment("GIT_EDITOR", "true");
                    set_environment("GIT_OPTIONAL_LOCKS", "0");
                    set_environment("GIT_AUTHOR_NAME", "Ann Author");
                    set_environment("GIT_AUTHOR_EMAIL", "ann@example.com");
                    set_environment("GIT_COMMITTER_NAME", "Cy Committer");
                    set_environment("GIT_COMMITTER_EMAIL", "cy@example.com");
                    set_environment("GIT_AUTHOR_DATE", "2026-04-01T10:00:00 +0900");
                    set_environment("GIT_COMMITTER_DATE", "2026-04-01T10:00:00 +0900");
                    // 사용자 전역 설정을 쓰지 않고 필요한 값만 환경으로 준다.
                    set_environment("GIT_CONFIG_GLOBAL", "NUL");
                    set_environment("GIT_CONFIG_COUNT", "3");
                    set_environment("GIT_CONFIG_KEY_0", "core.autocrlf");
                    set_environment("GIT_CONFIG_VALUE_0", "false");
                    set_environment("GIT_CONFIG_KEY_1", "init.defaultBranch");
                    set_environment("GIT_CONFIG_VALUE_1", "main");
                    set_environment("GIT_CONFIG_KEY_2", "protocol.file.allow");
                    set_environment("GIT_CONFIG_VALUE_2", "always");
                    return true;
                }();
                static_cast<void>(configured);
            }

            std::string drain_file(const std::filesystem::path& path)
            {
                return read_file(path);
            }

            std::atomic<unsigned int> run_counter { 0 };
        } // namespace

        std::string read_fixture(const std::string_view name)
        {
            const std::filesystem::path path { std::filesystem::path { CYELLA_TEST_DATA_DIRECTORY } / "git" / std::string { name } };
            std::ifstream stream { path, std::ios::binary };
            if (stream.is_open() == false)
                throw std::runtime_error { "fixture not found: " + path.string() };
            return std::string { std::istreambuf_iterator<char> { stream }, std::istreambuf_iterator<char> {} };
        }

        git_run_result run_git(const std::filesystem::path& working_directory, const std::vector<std::u8string>& arguments, const std::string_view standard_input)
        {
            configure_environment_once();

            const std::filesystem::path scratch { std::filesystem::temp_directory_path() / ("cyella_run_" + std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(run_counter.fetch_add(1))) };
            const std::filesystem::path input_path { scratch.string() + ".in" };
            const std::filesystem::path output_path { scratch.string() + ".out" };
            const std::filesystem::path error_path { scratch.string() + ".err" };
            write_file(input_path, standard_input);

            SECURITY_ATTRIBUTES inheritable {};
            inheritable.nLength = sizeof(inheritable);
            inheritable.bInheritHandle = TRUE;
            const auto open_handle = [&inheritable](const std::filesystem::path& path, const DWORD access, const DWORD disposition) {
                return ::CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
            };
            HANDLE input { open_handle(input_path, GENERIC_READ, OPEN_EXISTING) };
            HANDLE output { open_handle(output_path, GENERIC_WRITE, CREATE_ALWAYS) };
            HANDLE error { open_handle(error_path, GENERIC_WRITE, CREATE_ALWAYS) };

            std::wstring command_line { L"git" };
            for (const std::u8string& argument : arguments)
                append_quoted(command_line, widen(std::string_view { reinterpret_cast<const char*>(argument.data()), argument.size() }));
            // "git"만 있는 첫 토큰은 인용하지 않았으므로 위에서 공백이 앞서 붙는다.

            STARTUPINFOW startup {};
            startup.cb = sizeof(startup);
            startup.dwFlags = STARTF_USESTDHANDLES;
            startup.hStdInput = input;
            startup.hStdOutput = output;
            startup.hStdError = error;

            git_run_result result {};
            PROCESS_INFORMATION process {};
            const std::wstring directory { working_directory.wstring() };
            const BOOL started = ::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, directory.empty() ? nullptr : directory.c_str(), &startup, &process);
            if (started)
            {
                ::WaitForSingleObject(process.hProcess, 120000);
                DWORD exit_code { 0 };
                ::GetExitCodeProcess(process.hProcess, &exit_code);
                result.exit_code = static_cast<int>(exit_code);
                ::CloseHandle(process.hThread);
                ::CloseHandle(process.hProcess);
            }

            ::CloseHandle(input);
            ::CloseHandle(output);
            ::CloseHandle(error);
            result.standard_output = drain_file(output_path);
            result.standard_error = drain_file(error_path);

            std::error_code ignored {};
            std::filesystem::remove(input_path, ignored);
            std::filesystem::remove(output_path, ignored);
            std::filesystem::remove(error_path, ignored);
            return result;
        }

        git_run_result run_git(const std::filesystem::path& working_directory, const std::vector<std::string>& arguments, const std::string_view standard_input)
        {
            std::vector<std::u8string> converted {};
            converted.reserve(arguments.size());
            for (const std::string& argument : arguments)
                converted.emplace_back(argument.begin(), argument.end());
            return run_git(working_directory, converted, standard_input);
        }

        std::vector<std::string> to_text(const std::vector<std::u8string>& arguments)
        {
            std::vector<std::string> result {};
            result.reserve(arguments.size());
            for (const std::u8string& argument : arguments)
                result.emplace_back(argument.begin(), argument.end());
            return result;
        }

        bool git_is_available()
        {
            static const bool available = run_git(std::filesystem::temp_directory_path(), std::vector<std::string> { "--version" }).exit_code == 0;
            return available;
        }

        temp_directory::temp_directory()
        {
            // 이름에 프로세스 id와 번호만 쓰면 이전 프로세스가 지우지 못하고 죽은 폴더(타임아웃·강제 종료)와 새 프로세스의 이름이 겹친다. Windows는
            // 프로세스 id를 빨리 다시 쓰고 ctest는 test마다 새 프로세스를 띄우기 때문이다. 겹치면 잔해가 든 폴더를 그대로 받아 `git clone`이
            // "이미 있고 비어 있지 않다"로 실패한다. 그래서 이름에 무작위 값을 더하고, `create_directory`가 **새로 만들었을 때만**(이미 있으면
            // false) 쓴다.
            static std::atomic<unsigned int> counter { 0 };
            std::random_device entropy {};
            const std::filesystem::path root { std::filesystem::temp_directory_path() };
            for (int attempt { 0 }; attempt < 100; ++attempt)
            {
                char suffix[16] {};
                std::snprintf(suffix, sizeof(suffix), "%08x", static_cast<unsigned int>(entropy()));
                path_ = root / ("cyella_git_" + std::to_string(::GetCurrentProcessId()) + "_" + std::to_string(counter.fetch_add(1)) + "_" + suffix);
                std::error_code error {};
                if (std::filesystem::create_directories(path_.parent_path(), error); std::filesystem::create_directory(path_, error) && !error)
                    return;
            }
            throw std::runtime_error { "could not create a fresh temporary directory" };
        }

        temp_directory::~temp_directory()
        {
            std::error_code error {};
            for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator { path_, std::filesystem::directory_options::skip_permission_denied, error })
                std::filesystem::permissions(entry.path(), std::filesystem::perms::owner_write, std::filesystem::perm_options::add, error);
            std::filesystem::remove_all(path_, error);
        }
    } // namespace test
} // namespace cyella
