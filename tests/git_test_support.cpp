#include "git_test_support.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
#endif

#ifndef CYELLA_TEST_DATA_DIRECTORY
#error "CYELLA_TEST_DATA_DIRECTORY must be defined by the build."
#endif

namespace cyella {
    namespace test {
        namespace {
#if defined(_WIN32)
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

#endif

            void set_environment(const char* name, const char* value)
            {
#if defined(_WIN32)
                ::_putenv_s(name, value);
#else
                ::setenv(name, value, 1);
#endif
            }

            // 빈 설정 파일 자리다 (사용자의 전역 설정을 읽지 않게 한다).
#if defined(_WIN32)
            constexpr const char* null_device { "NUL" };
#else
            constexpr const char* null_device { "/dev/null" };
#endif

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
                    set_environment("GIT_CONFIG_GLOBAL", null_device);
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

            // git이 이 시간 안에 끝나지 않으면 끝내고 실패로 돌려준다. 자격 증명 질문에 걸린 git이 test를 무기한 붙잡지 않게 한다.
            constexpr unsigned long git_timeout_milliseconds { 120000 };

            unsigned long current_process_id() noexcept
            {
#if defined(_WIN32)
                return ::GetCurrentProcessId();
#else
                return static_cast<unsigned long>(::getpid());
#endif
            }
        } // namespace

        void isolate_git_environment()
        {
            configure_environment_once();
        }

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

            const std::filesystem::path scratch { std::filesystem::temp_directory_path() / ("cyella_run_" + std::to_string(current_process_id()) + "_" + std::to_string(run_counter.fetch_add(1))) };
            const std::filesystem::path input_path { scratch.string() + ".in" };
            const std::filesystem::path output_path { scratch.string() + ".out" };
            const std::filesystem::path error_path { scratch.string() + ".err" };
            write_file(input_path, standard_input);

#if !defined(_WIN32)
            // POSIX 판이다. PATH의 git을 띄우고, 입출력은 같은 파일로 잇고, 제한 시간이 지나면 끝낸다.
            git_run_result result {};
            {
                std::vector<std::string> storage { "git" };
                for (const std::u8string& argument : arguments)
                    storage.emplace_back(reinterpret_cast<const char*>(argument.data()), argument.size());
                std::vector<char*> argv {};
                for (std::string& value : storage)
                    argv.push_back(value.data());
                argv.push_back(nullptr);
                posix_spawn_file_actions_t actions {};
                ::posix_spawn_file_actions_init(&actions);
                const std::string input_text { input_path.string() };
                const std::string output_text { output_path.string() };
                const std::string error_text { error_path.string() };
                const std::string directory { working_directory.string() };
                ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, input_text.c_str(), O_RDONLY, 0);
                ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, output_text.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
                ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, error_text.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
                if (directory.empty() == false)
                    ::posix_spawn_file_actions_addchdir_np(&actions, directory.c_str());
                pid_t child { -1 };
                const int spawned { ::posix_spawnp(&child, "git", &actions, nullptr, argv.data(), environ) };
                ::posix_spawn_file_actions_destroy(&actions);
                if (spawned == 0)
                {
                    const auto deadline { std::chrono::steady_clock::now() + std::chrono::milliseconds { git_timeout_milliseconds } };
                    int status { 0 };
                    while (true)
                    {
                        const pid_t waited { ::waitpid(child, &status, WNOHANG) };
                        if (waited == child)
                        {
                            if (WIFEXITED(status))
                                result.exit_code = WEXITSTATUS(status);
                            break;
                        }
                        if (waited < 0)
                            break;
                        if (std::chrono::steady_clock::now() >= deadline)
                        {
                            // 시간 안에 끝나지 않은 git을 끝낸다. 종료 코드는 기본값(-1)으로 남아 실패로 읽힌다.
                            ::kill(child, SIGKILL);
                            ::waitpid(child, &status, 0);
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds { 2 });
                    }
                }
            }
#else
            SECURITY_ATTRIBUTES inheritable {};
            inheritable.nLength = sizeof(inheritable);
            inheritable.bInheritHandle = TRUE;
            const auto open_handle = [&inheritable](const std::filesystem::path& path, const DWORD access, const DWORD disposition) {
                return ::CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
            };
            HANDLE input { open_handle(input_path, GENERIC_READ, OPEN_EXISTING) };
            HANDLE output { open_handle(output_path, GENERIC_WRITE, CREATE_ALWAYS) };
            HANDLE error { open_handle(error_path, GENERIC_WRITE, CREATE_ALWAYS) };
            const bool redirected { input != INVALID_HANDLE_VALUE && output != INVALID_HANDLE_VALUE && error != INVALID_HANDLE_VALUE };

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
            // 입출력 파일을 하나라도 열지 못했으면 git을 띄우지 않는다. 핸들 없이 띄우면 출력을 잃은 채 성공처럼 보인다.
            const BOOL started = redirected ? ::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, directory.empty() ? nullptr : directory.c_str(), &startup, &process) : FALSE;
            if (started)
            {
                if (::WaitForSingleObject(process.hProcess, git_timeout_milliseconds) == WAIT_OBJECT_0)
                {
                    DWORD exit_code { 0 };
                    ::GetExitCodeProcess(process.hProcess, &exit_code);
                    result.exit_code = static_cast<int>(exit_code);
                }
                else
                {
                    // 시간 안에 끝나지 않은 git을 끝낸다. 그대로 두면 출력 파일과 작업 폴더를 계속 잡고 있어 뒤의 정리가 실패한다.
                    // 종료 코드는 기본값(-1)으로 남아 실패로 읽힌다. git이 띄운 자손까지는 끝내지 않는다.
                    ::TerminateProcess(process.hProcess, 1);
                    ::WaitForSingleObject(process.hProcess, 10000);
                }
                ::CloseHandle(process.hThread);
                ::CloseHandle(process.hProcess);
            }

            for (const HANDLE handle : { input, output, error })
            {
                if (handle != INVALID_HANDLE_VALUE)
                    ::CloseHandle(handle);
            }
#endif
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
#if defined(_WIN32)
            const std::filesystem::path root { std::filesystem::temp_directory_path() };
#else
            // macOS의 임시 폴더는 심볼릭 링크(`/var` → `/private/var`) 아래다. git이 알려 주는 경로와 같은 모양이 되게 푼다.
            const std::filesystem::path root { std::filesystem::canonical(std::filesystem::temp_directory_path()) };
#endif
            for (int attempt { 0 }; attempt < 100; ++attempt)
            {
                char suffix[16] {};
                std::snprintf(suffix, sizeof(suffix), "%08x", static_cast<unsigned int>(entropy()));
                path_ = root / ("cyella_git_" + std::to_string(current_process_id()) + "_" + std::to_string(counter.fetch_add(1)) + "_" + suffix);
                std::error_code error {};
                if (std::filesystem::create_directories(path_.parent_path(), error); std::filesystem::create_directory(path_, error) && !error)
                    return;
            }
            throw std::runtime_error { "could not create a fresh temporary directory" };
        }

        temp_directory::~temp_directory()
        {
            // 소멸자는 암묵적으로 noexcept라 예외가 나가면 프로세스가 끝난다. 범위 for의 증가는 오류 때 filesystem_error를 던지므로
            // (gc --auto 같은 백그라운드 git이 순회 중에 객체를 지우는 경합) 오류 코드를 받는 `increment`로 돌고, 오류가 나면 거기서
            // 멈춘다. 남은 것은 아래 `remove_all`이 지운다.
            std::error_code iteration_error {};
            std::filesystem::recursive_directory_iterator entries { path_, std::filesystem::directory_options::skip_permission_denied, iteration_error };
            for (; iteration_error == std::error_code {} && entries != std::filesystem::recursive_directory_iterator {}; entries.increment(iteration_error))
            {
                std::error_code permission_error {};
                std::filesystem::permissions(entries->path(), std::filesystem::perms::owner_write, std::filesystem::perm_options::add, permission_error);
            }
            std::error_code error {};
            std::filesystem::remove_all(path_, error);
        }
    } // namespace test
} // namespace cyella
