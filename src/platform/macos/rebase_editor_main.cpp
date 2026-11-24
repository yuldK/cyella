#include "domain/interactive_rebase.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// 인터랙티브 rebase에서 git이 부르는 편집기 도우미의 macOS 판이다 (Windows 판은 platform/win32/rebase_editor_main.cpp).
// 앱이 같은 실행 파일을 `GIT_SEQUENCE_EDITOR`와 `GIT_EDITOR`에 함께 걸고, git이 넘긴 파일 이름으로 역할을 가른다 (ADR-005).
//
// - **할 일 목록**(`.../rebase-merge/git-rebase-todo`): `CYELLA_REBASE_STATE` 폴더의 `todo`로 덮어쓴다. 그 전에 git이 만든 목록이
//   `expected`와 같은지 보고, 다르면 `mismatch`를 남기고 1로 끝난다. `messages/`의 메시지 파일은 `rebase-merge/cyella/messages/`로
//   옮기고 표지 `rebase-merge/cyella/active`를 쓴다.
// - **커밋 메시지**(그 밖의 파일, 보통 `<git_dir>/COMMIT_EDITMSG`): `<git_dir>/rebase-merge/done`의 마지막 줄이 지금 처리 중인
//   커밋이다. 그 커밋의 메시지 파일이 있으면 그 글로 덮어쓰고, 없으면 그대로 둔다(`true`와 같다).
//
// luil·Skia에 링크하지 않는다.
namespace {
    constexpr int exit_ok { 0 };
    constexpr int exit_failed { 1 };
    constexpr std::string_view todo_file_name { "git-rebase-todo" };
    constexpr std::uintmax_t file_limit { 16u * 1024u * 1024u };

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

    // 파일 이름이 같은가. macOS 기본 볼륨은 대소문자를 가리지 않으므로 Windows 판처럼 ASCII 대소문자를 무시한다.
    bool same_name(const std::filesystem::path& path, const std::string_view name)
    {
        const std::string file { path.filename().string() };
        if (file.size() != name.size())
            return false;
        for (std::size_t index { 0 }; index < file.size(); ++index)
        {
            const auto lower = [](const char value) { return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value; };
            if (lower(file[index]) != lower(name[index]))
                return false;
        }
        return true;
    }

    int run_sequence(const std::filesystem::path& todo_path)
    {
        const char* const state_value { std::getenv("CYELLA_REBASE_STATE") };
        if (state_value == nullptr || state_value[0] == '\0')
            return exit_failed;
        const std::filesystem::path state { state_value };

        const std::optional<std::string> prepared { read_file(state / "todo") };
        const std::optional<std::string> expected { read_file(state / "expected") };
        const std::optional<std::string> generated { read_file(todo_path) };
        if (prepared.has_value() == false || expected.has_value() == false || generated.has_value() == false)
            return exit_failed;

        const std::vector<std::string> expected_shas { read_lines(*expected) };
        if (cyella::rebase_todo_matches(*generated, expected_shas) == false)
        {
            static_cast<void>(write_file(state / "mismatch", *generated));
            return exit_failed;
        }

        const std::filesystem::path messages { state / "messages" };
        const std::filesystem::path cyella_state { todo_path.parent_path() / "cyella" };
        const std::filesystem::path target { cyella_state / "messages" };
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
        if (write_file(cyella_state / "active", "1\n") == false)
            return exit_failed;
        return write_file(todo_path, *prepared) ? exit_ok : exit_failed;
    }

    int run_message(const std::filesystem::path& message_path)
    {
        const std::filesystem::path git_dir { message_path.parent_path() };
        const std::optional<std::string> done { read_file(git_dir / "rebase-merge" / "done") };
        if (done.has_value() == false)
            return exit_ok;
        const std::optional<std::string> key { cyella::message_key_for_editor(*done) };
        if (key.has_value() == false || cyella::is_hex_object_id(*key) == false)
            return exit_ok;
        const std::filesystem::path stored { git_dir / "rebase-merge" / "cyella" / "messages" / *key };
        std::error_code error {};
        if (std::filesystem::is_regular_file(stored, error) == false)
            return exit_ok;
        const std::optional<std::string> message { read_file(stored) };
        if (message.has_value() == false)
            return exit_failed;
        return write_file(message_path, *message) ? exit_ok : exit_failed;
    }

    int run(const int argc, const char* const* const argv)
    {
        // git은 편집기 명령 뒤에 파일 경로 하나를 붙인다. 마지막 인자를 쓴다.
        if (argc < 2 || argv[argc - 1] == nullptr || argv[argc - 1][0] == '\0')
            return exit_failed;
        std::filesystem::path path { argv[argc - 1] };
        // git이 편집기를 저장소 폴더에서 띄우며 상대 경로를 줄 수 있다.
        if (path.is_relative())
        {
            std::error_code error {};
            path = std::filesystem::absolute(path, error);
            if (error)
                return exit_failed;
        }
        if (same_name(path, todo_file_name))
            return run_sequence(path);
        return run_message(path);
    }
} // namespace

int main(const int argc, const char* const* const argv)
{
    try
    {
        return run(argc, argv);
    }
    catch (...)
    {
        return exit_failed;
    }
}
