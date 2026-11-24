#include "platform/win32/win32_diff_tool_launcher.h"

#include "domain/path_syntax.h"
#include "domain/text_catalog.h"
#include "infrastructure/git_tool_discovery.h"
#include "platform/macos/posix_file.h"
#include "platform/win32/win32_file_probe.h"

#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <thread>
#include <utility>
#include <vector>

extern char** environ;

// `platform/win32/win32_diff_tool_launcher.h`의 macOS 구현이다.
//  - 이름만 적힌 도구는 앱 폴더, `/usr/bin`, `PATH`, Homebrew 폴더(`/opt/homebrew/bin`, `/usr/local/bin`)에서 찾는다. Finder에서
//    띄운 앱의 `PATH`에는 Homebrew 폴더가 없어 따로 더한다. `posix_spawn`은 이름을 현재 폴더에서 찾지 않지만, 찾은 절대 경로를
//    넘겨 무엇이 뜨는지 분명히 한다.
//  - 도구는 새 세션으로 띄워 앱이 끝나도 함께 끝나지 않게 하고, 끝나면 작은 thread가 거둔다(zombie를 남기지 않는다).
namespace cyella::win32 {
    namespace {
        std::vector<std::u8string> current_tool_search_directories()
        {
            std::u8string path { read_environment_variable(u8"PATH") };
            for (const std::u8string_view extra : { u8"/opt/homebrew/bin", u8"/usr/local/bin" })
            {
                if (path.empty() == false)
                    path.push_back(path_list_separator);
                path.append(extra);
            }
            return tool_search_directories(macos::current_executable_directory(), u8"/usr/bin", {}, path);
        }

        class posix_diff_tool_launcher final : public diff_tool_launcher
        {
        public:
            tool_launch_result launch(const tool_launch_request& request) override
            {
                if (request.executable.empty())
                    return { false, tr_narrow(u8"diff_tool.executable_empty") };
                const std::unique_ptr<file_probe> probe { make_file_probe() };
                const std::optional<std::u8string> executable { resolve_tool_executable(request.executable, current_tool_search_directories(), *probe) };
                if (executable.has_value() == false)
                    return { false, tr_narrow(u8"diff_tool.create_process_failed", { { u8"error", ENOENT } }) };

                std::vector<std::string> storage {};
                storage.push_back(macos::to_narrow(*executable));
                for (const std::u8string& argument : request.arguments)
                    storage.push_back(macos::to_narrow(argument));
                std::vector<char*> argv {};
                for (std::string& value : storage)
                {
                    if (value.find('\0') != std::string::npos)
                        return { false, tr_narrow(u8"diff_tool.command_line_conversion_failed") };
                    argv.push_back(value.data());
                }
                argv.push_back(nullptr);

                posix_spawn_file_actions_t actions {};
                posix_spawnattr_t attributes {};
                if (::posix_spawn_file_actions_init(&actions) != 0)
                    return { false, tr_narrow(u8"diff_tool.create_process_failed", { { u8"error", errno } }) };
                if (::posix_spawnattr_init(&attributes) != 0)
                {
                    static_cast<void>(::posix_spawn_file_actions_destroy(&actions));
                    return { false, tr_narrow(u8"diff_tool.create_process_failed", { { u8"error", errno } }) };
                }
                bool prepared { ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSID) == 0 };
                prepared = prepared && ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) == 0;
                prepared = prepared && ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0;
                prepared = prepared && ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0) == 0;
                const std::string directory { macos::to_narrow(request.working_directory) };
                if (directory.empty() == false)
                    prepared = prepared && ::posix_spawn_file_actions_addchdir_np(&actions, directory.c_str()) == 0;
                pid_t child { -1 };
                const int spawned { prepared ? ::posix_spawn(&child, argv.front(), &actions, &attributes, argv.data(), environ) : EINVAL };
                static_cast<void>(::posix_spawn_file_actions_destroy(&actions));
                static_cast<void>(::posix_spawnattr_destroy(&attributes));
                if (spawned != 0)
                    return { false, tr_narrow(u8"diff_tool.create_process_failed", { { u8"error", spawned } }) };
                try
                {
                    const auto reap = [child] {
                        int status { 0 };
                        while (::waitpid(child, &status, 0) < 0 && errno == EINTR)
                        {
                        }
                    };
                    std::thread { reap }.detach();
                }
                catch (...)
                {
                    // 거둘 thread를 못 만들면 zombie가 하나 남는다. 도구는 이미 떴으므로 성공으로 답한다.
                }
                return { true, {} };
            }
        };
    } // namespace

    std::unique_ptr<diff_tool_launcher> make_diff_tool_launcher()
    {
        return std::make_unique<posix_diff_tool_launcher>();
    }

    std::vector<std::u8string> tool_search_directories(
        const std::u8string_view application_directory, const std::u8string_view system_directory, const std::u8string_view windows_directory, const std::u8string_view path_environment)
    {
        std::vector<std::u8string> directories {};
        for (const std::u8string_view directory : { application_directory, system_directory, windows_directory })
            if (directory.empty() == false && is_absolute_path(directory))
                directories.emplace_back(directory);
        for (std::u8string& directory : split_search_path(path_environment))
            directories.push_back(std::move(directory));
        return directories;
    }

    std::optional<std::u8string> resolve_tool_executable(const std::u8string_view executable, const std::vector<std::u8string>& directories, const file_probe& probe)
    {
        // 경로가 적혔으면 그대로 쓴다. POSIX에서는 `/`만 구분자다.
        if (executable.find(u8'/') != std::u8string_view::npos)
            return std::u8string { executable };
        for (const std::u8string& directory : directories)
        {
            if (is_absolute_path(directory) == false)
                continue;
            std::u8string candidate { join_native_path(directory, executable) };
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
        if (executable != u8"claude")
            return std::nullopt;
        // Claude Code의 네이티브 설치(`~/.local/bin/claude`)와 옛 로컬 설치(`~/.claude/local/claude`) 자리다.
        if (is_absolute_path(user_profile))
            for (const std::u8string_view relative : { u8".local/bin/claude", u8".claude/local/claude" })
            {
                std::u8string candidate { join_native_path(user_profile, relative) };
                if (probe.probe(candidate) == path_kind::file)
                    return candidate;
            }
        return std::nullopt;
    }

    std::optional<std::u8string> resolve_suggestion_executable(const std::u8string_view executable)
    {
        const std::unique_ptr<file_probe> probe { make_file_probe() };
        return resolve_suggestion_executable(executable, current_tool_search_directories(), macos::home_directory(), *probe);
    }
} // namespace cyella::win32
