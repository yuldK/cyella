#include "infrastructure/ssh_info.h"

#include "domain/path_syntax.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace cyella {
    namespace {
        constexpr std::size_t public_key_line_limit { 8192u };

        std::u8string join(std::u8string base, const std::u8string_view tail)
        {
            while (base.empty() == false && (base.back() == u8'\\' || base.back() == u8'/'))
                base.pop_back();
            base.push_back(u8'\\');
            base.append(tail);
            return base;
        }

        // 마지막 구성 요소를 뗀 부모다. 더 뗄 것이 없으면 빈 값이다.
        std::u8string parent_of(const std::u8string_view path)
        {
            std::u8string trimmed { path };
            while (trimmed.empty() == false && (trimmed.back() == u8'\\' || trimmed.back() == u8'/'))
                trimmed.pop_back();
            const std::size_t cut { trimmed.find_last_of(u8"\\/") };
            if (cut == std::u8string::npos || cut == 0)
                return {};
            return trimmed.substr(0, cut);
        }

        bool has_file(const file_probe& probe, const std::u8string& path)
        {
            return probe.probe(path) == path_kind::file;
        }

        ssh_programs programs_in(const std::u8string_view directory, const file_probe& probe, std::u8string origin)
        {
            ssh_programs programs {};
            const std::u8string ssh { join(std::u8string { directory }, u8"ssh.exe") };
            const std::u8string ssh_add { join(std::u8string { directory }, u8"ssh-add.exe") };
            const std::u8string ssh_keygen { join(std::u8string { directory }, u8"ssh-keygen.exe") };
            if (has_file(probe, ssh) == false || has_file(probe, ssh_keygen) == false)
                return programs;
            programs.ssh = ssh;
            programs.ssh_keygen = ssh_keygen;
            if (has_file(probe, ssh_add))
                programs.ssh_add = ssh_add;
            programs.origin = std::move(origin);
            return programs;
        }

        std::u8string first_line_of(const std::filesystem::path& path)
        {
            std::ifstream stream { path, std::ios::binary };
            std::string line(public_key_line_limit, '\0');
            stream.getline(line.data(), static_cast<std::streamsize>(line.size()));
            if (const std::size_t terminator { line.find('\0') }; terminator != std::string::npos)
                line.resize(terminator);
            while (line.empty() == false && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
                line.pop_back();
            return std::u8string { line.begin(), line.end() };
        }

        class capture_sink final : public process_output_sink
        {
        public:
            void on_record(const process_output_record& record) override
            {
                if (record.stream != process_stream::standard_output)
                    return;
                text.append(record.text);
                text.push_back(u8'\n');
            }

            std::u8string text {};
        };
    } // namespace

    ssh_programs locate_ssh_programs(const std::u8string_view git_executable, const std::u8string_view windows_directory, const file_probe& probe)
    {
        // `...\Git\cmd\git.exe`와 `...\Git\mingw64\bin\git.exe`가 모두 Git 루트의 아래다. 위로 올라가며 `usr\bin`을 찾는다.
        ssh_programs from_git {};
        std::u8string directory { parent_of(git_executable) };
        for (int level { 0 }; level < 3 && directory.empty() == false && from_git.found() == false; ++level)
        {
            from_git = programs_in(join(directory, u8"usr\\bin"), probe, u8"Git for Windows");
            directory = parent_of(directory);
        }
        ssh_programs from_system {};
        if (windows_directory.empty() == false)
            from_system = programs_in(join(join(std::u8string { windows_directory }, u8"System32"), u8"OpenSSH"), probe, u8"Windows OpenSSH");

        // git은 자기 ssh를 쓰므로 ssh와 ssh-keygen은 Git for Windows 쪽을 먼저 고른다. 반면 Windows의 ssh-agent 서비스는
        // 시스템 OpenSSH의 `ssh-add`만 닿는 named pipe라, 에이전트 조회는 그쪽을 먼저 쓴다.
        ssh_programs chosen { from_git.found() ? from_git : from_system };
        if (from_system.ssh_add.empty() == false)
            chosen.ssh_add = from_system.ssh_add;
        return chosen;
    }

    ssh_info_snapshot collect_ssh_info(const ssh_info_sources& sources) noexcept
    {
        ssh_info_snapshot info {};
        try
        {
            if (sources.service_status != nullptr)
                info.service = sources.service_status();

            if (sources.runner != nullptr && sources.ssh_add_executable.empty() == false)
            {
                info.agent_list_available = true;
                capture_sink sink {};
                // 프로세스 요청은 절대 경로의 작업 디렉터리를 요구한다. 어디서 돌아도 상관없으므로 ssh-add가 있는 폴더를 쓴다.
                const process_result result { sources.runner->run(make_ssh_add_list_request(sources.ssh_add_executable, parent_of(sources.ssh_add_executable)), &sink, sources.cancellation) };
                if (result.completion == process_completion::exited && result.exit_code.has_value())
                    info.agent = parse_ssh_add_list(*result.exit_code, sink.text);
                else
                    info.agent.state = ssh_agent_list_state::failed;
            }

            info.ssh_directory = sources.ssh_directory.u8string();
            for (const ssh_key_pair& pair : list_ssh_key_pairs(sources.ssh_directory))
            {
                ssh_key_entry entry {};
                entry.name = pair.name;
                entry.key_type = pair.key_type;
                entry.comment = pair.comment;
                entry.public_key_line = first_line_of(pair.public_key_path);
                entry.private_key_path = pair.private_key_path.u8string();
                info.keys.push_back(std::move(entry));
            }
        }
        catch (...)
        {
            // 읽은 데까지가 결과다. 화면은 비어 있는 부분을 "알 수 없음"으로 보인다.
        }
        return info;
    }
} // namespace cyella
