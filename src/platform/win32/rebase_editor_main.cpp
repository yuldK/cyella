#include "domain/interactive_rebase.h"

#include <windows.h>

#include <shellapi.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// 인터랙티브 rebase에서 git이 부르는 편집기 도우미다. 앱이 같은 실행 파일을 `GIT_SEQUENCE_EDITOR`와
// `GIT_EDITOR`에 함께 건다. 두 변수는 한 환경에 같이 있어 환경 변수로는 역할을 가를 수 없으므로, git이 넘긴
// 파일 이름으로 가른다 (ADR-005).
//
// - **할 일 목록**(`.../rebase-merge/git-rebase-todo`): `CYELLA_REBASE_STATE` 폴더의 `todo`로 덮어쓴다. 그 전에
//   git이 만든 목록이 `expected`(편집기에 보인 커밋의 SHA)와 같은지 보고, 다르면 `mismatch`를 남기고 1로 끝난다
//   (git은 rebase를 시작하지 않고 깨끗이 멈춘다). `messages/`의 메시지 파일은 `rebase-merge/cyella/messages/`로
//   옮기고 표지 `rebase-merge/cyella/active`를 쓴다. git이 rebase를 끝내거나 중단할 때 `rebase-merge`를 통째로
//   지우므로 메시지도 함께 사라진다.
// - **커밋 메시지**(그 밖의 파일, 보통 `<git_dir>/COMMIT_EDITMSG`): `<git_dir>/rebase-merge/done`의 마지막 줄이
//   지금 처리 중인 커밋이다. 그 커밋의 메시지 파일이 있으면 그 글로 덮어쓰고, 없으면 그대로 둔다(`true`와 같다).
//
// luil·Skia에 링크하지 않는다. 콘솔 창이 뜨지 않게 WIN32 subsystem이다.
namespace cyella::win32 {
    namespace {
        constexpr int exit_ok { 0 };
        constexpr int exit_failed { 1 };
        constexpr std::wstring_view todo_file_name { L"git-rebase-todo" };
        // 메시지·목록 파일의 상한이다. 비정상적으로 큰 파일을 읽지 않는다.
        constexpr std::uintmax_t file_limit { 16u * 1024u * 1024u };

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

        std::optional<std::wstring> file_argument()
        {
            int count { 0 };
            LPWSTR* const arguments { CommandLineToArgvW(GetCommandLineW(), &count) };
            if (arguments == nullptr)
                return std::nullopt;
            std::optional<std::wstring> path {};
            // git은 편집기 명령 뒤에 파일 경로 하나를 붙인다. 마지막 인자를 쓴다.
            if (count >= 2)
                path = std::wstring { arguments[count - 1] };
            LocalFree(arguments);
            return path;
        }

        std::optional<std::string> read_file(const std::filesystem::path& path)
        {
            std::error_code error {};
            const std::uintmax_t size { std::filesystem::file_size(path, error) };
            if (error || size > file_limit)
                return std::nullopt;
            std::ifstream stream { path, std::ios::binary };
            if (stream.is_open() == false)
                return std::nullopt;
            std::string text { std::istreambuf_iterator<char> { stream }, std::istreambuf_iterator<char> {} };
            if (stream.bad())
                return std::nullopt;
            return text;
        }

        bool write_file(const std::filesystem::path& path, const std::string_view text)
        {
            std::ofstream stream { path, std::ios::binary | std::ios::trunc };
            if (stream.is_open() == false)
                return false;
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            stream.close();
            return stream.fail() == false;
        }

        std::vector<std::string> read_lines(const std::string_view text)
        {
            std::vector<std::string> lines {};
            std::string_view rest { text };
            while (rest.empty() == false)
            {
                const std::size_t end { rest.find('\n') };
                std::string_view line { rest.substr(0, end) };
                if (line.empty() == false && line.back() == '\r')
                    line.remove_suffix(1);
                if (line.empty() == false)
                    lines.emplace_back(line);
                if (end == std::string_view::npos)
                    break;
                rest.remove_prefix(end + 1);
            }
            return lines;
        }

        bool same_name(const std::filesystem::path& path, const std::wstring_view name)
        {
            const std::wstring file { path.filename().wstring() };
            return CompareStringOrdinal(file.c_str(), static_cast<int>(file.size()), name.data(), static_cast<int>(name.size()), TRUE) == CSTR_EQUAL;
        }

        int run_sequence(const std::filesystem::path& todo_path)
        {
            const std::optional<std::wstring> state_value { read_environment(L"CYELLA_REBASE_STATE") };
            if (state_value.has_value() == false || state_value->empty())
                return exit_failed;
            const std::filesystem::path state { *state_value };

            const std::optional<std::string> prepared { read_file(state / L"todo") };
            const std::optional<std::string> expected { read_file(state / L"expected") };
            const std::optional<std::string> generated { read_file(todo_path) };
            if (prepared.has_value() == false || expected.has_value() == false || generated.has_value() == false)
                return exit_failed;

            // 편집기를 연 뒤 브랜치가 움직였거나 git이 다르게 고른 범위면 시작하지 않는다.
            const std::vector<std::string> expected_shas { read_lines(*expected) };
            if (rebase_todo_matches(*generated, expected_shas) == false)
            {
                static_cast<void>(write_file(state / L"mismatch", *generated));
                return exit_failed;
            }

            const std::filesystem::path messages { state / L"messages" };
            const std::filesystem::path cyella_state { todo_path.parent_path() / L"cyella" };
            const std::filesystem::path target { cyella_state / L"messages" };
            std::error_code error {};
            std::filesystem::create_directories(target, error);
            if (error)
                return exit_failed;
            if (std::filesystem::is_directory(messages, error))
            {
                for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator { messages, error })
                {
                    if (entry.is_regular_file(error) == false)
                        continue;
                    std::filesystem::copy_file(entry.path(), target / entry.path().filename(), std::filesystem::copy_options::overwrite_existing, error);
                    if (error)
                        return exit_failed;
                }
                if (error)
                    return exit_failed;
            }
            // 앱이 continue·skip에서 이 rebase에 메시지 도우미를 다시 걸지 정하는 표지다.
            if (write_file(cyella_state / L"active", "1\n") == false)
                return exit_failed;
            return write_file(todo_path, *prepared) ? exit_ok : exit_failed;
        }

        int run_message(const std::filesystem::path& message_path)
        {
            // 메시지 파일(`COMMIT_EDITMSG`)은 git 디렉터리 바로 아래에 있다. worktree에서도 그 worktree의 git
            // 디렉터리이고 `rebase-merge`도 같은 곳에 있다.
            const std::filesystem::path git_dir { message_path.parent_path() };
            const std::optional<std::string> done { read_file(git_dir / L"rebase-merge" / L"done") };
            if (done.has_value() == false)
                return exit_ok;
            const std::optional<std::string> key { message_key_for_editor(*done) };
            if (key.has_value() == false || is_hex_object_id(*key) == false)
                return exit_ok;
            const std::filesystem::path stored { git_dir / L"rebase-merge" / L"cyella" / L"messages" / std::filesystem::path { std::wstring { key->begin(), key->end() } } };
            std::error_code error {};
            if (std::filesystem::is_regular_file(stored, error) == false)
                return exit_ok;
            const std::optional<std::string> message { read_file(stored) };
            if (message.has_value() == false)
                return exit_failed;
            return write_file(message_path, *message) ? exit_ok : exit_failed;
        }

        int run()
        {
            const std::optional<std::wstring> argument { file_argument() };
            if (argument.has_value() == false || argument->empty())
                return exit_failed;
            const std::filesystem::path path { *argument };
            if (same_name(path, todo_file_name))
                return run_sequence(path);
            return run_message(path);
        }
    } // namespace
} // namespace cyella::win32

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int)
{
    try
    {
        return cyella::win32::run();
    }
    catch (...)
    {
        return 1;
    }
}
