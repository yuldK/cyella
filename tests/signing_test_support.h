#pragma once

#include "application/process_request.h"
#include "application/process_runner.h"
#include "infrastructure/ssh_info.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_process_runner.h"
#include "platform/win32/win32_ssh_environment.h"

#include "git_test_support.h"
#include "process_test_support.h"
#include "temporary_directory_support.h"

#include <windows.h>

#include <cctype>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 커밋 서명 통합 test가 쓰는 격리 환경이다. 사용자의 전역 git 설정, ~/.gnupg, ~/.ssh, ssh-agent를 건드리지 않는다.
//  - `GIT_CONFIG_GLOBAL`은 임시 파일, `GIT_CONFIG_NOSYSTEM=1`.
//  - `GNUPGHOME`은 임시 폴더다. Git for Windows의 gpg는 msys 프로그램이라 Windows 경로(`C:\...`, `C:/...`)를 상대 경로로 읽으므로
//    `/c/...` 꼴로 줘야 하고, gpg-agent의 유닉스 소켓 경로가 108자를 넘으면 agent가 뜨지 못하므로 경로를 짧게 둔다.
//  - 키는 모두 임시 폴더에 새로 만든다.
// test마다 새 프로세스로 도므로(ctest) 환경 변수를 바꿔도 다른 test에 새지 않는다. 그래도 소멸자가 되돌린다.
namespace cyella::test {
    class environment_variable_guard
    {
    public:
        environment_variable_guard(const wchar_t* name, const std::wstring& value)
            : name_ { name }
        {
            DWORD size { GetEnvironmentVariableW(name, nullptr, 0) };
            if (size != 0)
            {
                std::wstring old(size, L'\0');
                size = GetEnvironmentVariableW(name, old.data(), static_cast<DWORD>(old.size()));
                old.resize(size);
                previous_ = std::move(old);
            }
            SetEnvironmentVariableW(name, value.c_str());
        }

        environment_variable_guard(const environment_variable_guard&) = delete;
        environment_variable_guard& operator=(const environment_variable_guard&) = delete;

        ~environment_variable_guard()
        {
            SetEnvironmentVariableW(name_, previous_.has_value() ? previous_->c_str() : nullptr);
        }

    private:
        const wchar_t* name_;
        std::optional<std::wstring> previous_ {};
    };

    struct program_result
    {
        std::int32_t exit_code { -1 };
        std::string standard_output {};
        std::string standard_error {};
    };

    // 프로그램 하나를 실행하고 끝나기를 기다린다. cyella의 process runner를 쓰므로 자식 트리가 job object에 묶여 끝나면 정리된다.
    inline program_result run_program(const std::filesystem::path& executable, const std::vector<std::string>& arguments, const std::filesystem::path& working_directory)
    {
        const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
        process_request request {};
        request.executable = executable.generic_u8string();
        request.working_directory = working_directory.generic_u8string();
        for (const std::string& argument : arguments)
            request.arguments.push_back(to_u8string(argument));
        request.standard_output_mode = process_output_mode::raw;
        request.timeout = std::chrono::seconds { 60 };
        request.maximum_captured_bytes_per_stream = 4u * 1024u * 1024u;
        collecting_sink sink {};
        const process_result result { runner->run(request, &sink, {}) };
        program_result run {};
        run.exit_code = result.exit_code.value_or(-1);
        run.standard_output = sink.standard_output_bytes;
        for (const std::u8string& line : sink.record_texts(process_stream::standard_error))
            run.standard_error.append(to_string(line)).push_back('\n');
        return run;
    }

    // `C:\a\b`를 `/c/a/b`로 바꾼다. msys 프로그램(gpg)에 넘기는 홈 폴더 경로다.
    inline std::string to_msys_path(const std::filesystem::path& path)
    {
        std::string text { path.generic_string() };
        if (text.size() >= 2 && text[1] == ':')
        {
            const char drive { static_cast<char>(std::tolower(static_cast<unsigned char>(text[0]))) };
            text = std::string { "/" } + drive + text.substr(2);
        }
        return text;
    }

    // Git for Windows의 gpg를 찾는다: git 실행 파일(`<루트>\cmd\git.exe`, `<루트>\mingw64\bin\git.exe`)의 위쪽 `usr\bin\gpg.exe`.
    inline std::optional<std::filesystem::path> find_gpg(const std::u8string_view git_executable)
    {
        std::filesystem::path directory { std::filesystem::path { git_executable }.parent_path() };
        for (int level { 0 }; level < 4 && directory.has_parent_path(); ++level, directory = directory.parent_path())
        {
            const std::filesystem::path candidate { directory / "usr" / "bin" / "gpg.exe" };
            if (std::filesystem::is_regular_file(candidate))
                return candidate;
        }
        return std::nullopt;
    }

    // 임시 전역 설정과 GNUPGHOME을 만든다. 소멸할 때 이 폴더의 gpg-agent를 끈다.
    class signing_sandbox
    {
    public:
        signing_sandbox()
            : directory_ { "sg" }
        {
            global_config_ = directory_.path() / "gitconfig";
            gnupg_home_ = directory_.path() / "h";
            ssh_directory_ = directory_.path() / "ssh";
            std::filesystem::create_directories(gnupg_home_);
            std::filesystem::create_directories(ssh_directory_);
            write_file(global_config_, "[user]\n\tname = Ann Author\n\temail = ann@example.com\n");
            config_ = std::make_unique<environment_variable_guard>(L"GIT_CONFIG_GLOBAL", global_config_.wstring());
            no_system_ = std::make_unique<environment_variable_guard>(L"GIT_CONFIG_NOSYSTEM", L"1");
            const std::string home { to_msys_path(gnupg_home_) };
            home_ = std::make_unique<environment_variable_guard>(L"GNUPGHOME", std::wstring { home.begin(), home.end() });
        }

        signing_sandbox(const signing_sandbox&) = delete;
        signing_sandbox& operator=(const signing_sandbox&) = delete;

        ~signing_sandbox()
        {
            kill_agent();
        }

        [[nodiscard]] const std::filesystem::path& root() const noexcept
        {
            return directory_.path();
        }

        [[nodiscard]] const std::filesystem::path& global_config() const noexcept
        {
            return global_config_;
        }

        [[nodiscard]] const std::filesystem::path& gnupg_home() const noexcept
        {
            return gnupg_home_;
        }

        [[nodiscard]] const std::filesystem::path& ssh_directory() const noexcept
        {
            return ssh_directory_;
        }

        // 임시 전역 설정에 줄을 더한다 (`gpg.program` 등). 사용자의 전역 설정이 아니다.
        void append_global_config(const std::string_view text) const
        {
            std::string content { read_file(global_config_) };
            content.append(text);
            write_file(global_config_, content);
        }

        // 이 폴더에서 띄운 gpg-agent를 끈다 (우리가 시작한 것만이다: GNUPGHOME이 이 폴더다).
        void kill_agent() const
        {
            if (gpgconf_.empty())
                return;
            static_cast<void>(run_program(gpgconf_, { "--kill", "gpg-agent" }, directory_.path()));
        }

        void set_gpgconf(std::filesystem::path path)
        {
            gpgconf_ = std::move(path);
        }

    private:
        temporary_directory directory_;
        std::filesystem::path global_config_ {};
        std::filesystem::path gnupg_home_ {};
        std::filesystem::path ssh_directory_ {};
        std::filesystem::path gpgconf_ {};
        std::unique_ptr<environment_variable_guard> config_ {};
        std::unique_ptr<environment_variable_guard> no_system_ {};
        std::unique_ptr<environment_variable_guard> home_ {};
    };

    // ssh-keygen으로 passphrase 없는(또는 있는) ed25519 키를 만든다. `<경로>`(개인 키)와 `<경로>.pub`가 생긴다.
    inline bool make_ssh_key(const std::filesystem::path& ssh_keygen, const std::filesystem::path& key, const std::string& comment, const std::string& passphrase)
    {
        const program_result result { run_program(ssh_keygen, { "-q", "-t", "ed25519", "-N", passphrase, "-C", comment, "-f", key.generic_string() }, key.parent_path()) };
        return result.exit_code == 0 && std::filesystem::is_regular_file(key);
    }

    // `<principal> <키 종류> <공개 키>` 한 줄짜리 allowed_signers 파일을 쓴다.
    inline void write_allowed_signers(const std::filesystem::path& file, const std::string& principal, const std::filesystem::path& public_key)
    {
        std::string line { read_file(public_key) };
        while (line.empty() == false && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        // 공개 키 파일은 `<종류> <키> <주석>`이다. 주석은 뗀다.
        const std::size_t first { line.find(' ') };
        const std::size_t second { first == std::string::npos ? std::string::npos : line.find(' ', first + 1) };
        if (second != std::string::npos)
            line.resize(second);
        write_file(file, principal + " " + line + "\n");
    }
} // namespace cyella::test
