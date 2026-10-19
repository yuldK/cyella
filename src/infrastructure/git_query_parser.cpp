#include "infrastructure/git_query_parser.h"

#include "infrastructure/git_text.h"

#include <cstdint>
#include <utility>

namespace cyella {
    namespace {
        std::string_view trim(std::string_view text) noexcept
        {
            while (text.empty() == false && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
                text.remove_suffix(1);
            while (text.empty() == false && (text.front() == ' ' || text.front() == '\t'))
                text.remove_prefix(1);
            return text;
        }

        std::string_view first_line(const std::string_view text) noexcept
        {
            return trim(text.substr(0, text.find('\n')));
        }

        std::uint32_t read_number(const std::optional<std::string>& text) noexcept
        {
            if (text.has_value() == false)
                return 0;
            return git_text::parse_integer<std::uint32_t>(trim(*text)).value_or(0);
        }

        std::string read_trimmed(const std::optional<std::string>& text)
        {
            return text.has_value() ? std::string { trim(*text) } : std::string {};
        }
    } // namespace

    std::vector<remote_info> parse_git_remotes(const std::string_view bytes)
    {
        std::vector<remote_info> remotes {};
        std::string_view rest { bytes };
        while (rest.empty() == false)
        {
            std::string_view line {};
            git_text::take_field(rest, '\n', line);
            line = trim(line);

            std::string_view name {};
            if (git_text::take_field(line, '\t', name) == false || name.empty())
                continue;
            // `<url> (fetch)`. URL에 공백이 있을 수 있어 마지막 공백에서 자른다.
            const std::size_t space { line.rfind(' ') };
            if (space == std::string_view::npos)
                continue;
            const std::string_view url { line.substr(0, space) };
            const std::string_view role { line.substr(space + 1) };

            remote_info* target { nullptr };
            for (remote_info& remote : remotes)
                if (remote.name == name)
                    target = &remote;
            if (target == nullptr)
            {
                remotes.push_back(remote_info { std::string { name }, {}, {} });
                target = &remotes.back();
            }
            if (role == "(fetch)")
                target->fetch_url = std::string { url };
            else if (role == "(push)")
                target->push_url = std::string { url };
        }
        return remotes;
    }

    overwritten_files parse_overwritten_files(const std::string_view standard_error)
    {
        overwritten_files result {};
        bool in_list { false };
        std::string_view rest { standard_error };
        while (rest.empty() == false)
        {
            std::string_view line {};
            git_text::take_field(rest, '\n', line);
            if (line.empty() == false && line.back() == '\r')
                line.remove_suffix(1);

            if (line.find("would be overwritten") != std::string_view::npos)
            {
                in_list = true;
                if (line.find("untracked working tree files") != std::string_view::npos)
                    result.untracked = true;
                continue;
            }
            // 목록은 탭으로 들여 쓴 줄이 이어지는 동안이다. 다음 안내 문구에서 끝난다.
            if (in_list && line.empty() == false && line.front() == '\t')
            {
                const std::string_view path { trim(line) };
                if (path.empty() == false)
                    result.files.emplace_back(path);
                continue;
            }
            in_list = false;
        }
        return result;
    }

    std::vector<file_change> parse_git_name_status(const std::string_view bytes)
    {
        std::vector<file_change> changes {};
        std::string_view rest { bytes };
        while (rest.empty() == false)
        {
            std::string_view status {};
            git_text::take_field(rest, '\0', status);
            // 어떤 git은 머리에 빈 줄을 둔다(`--format=` 뒤의 줄바꿈).
            while (status.empty() == false && (status.front() == '\n' || status.front() == '\r'))
                status.remove_prefix(1);
            if (status.empty())
                continue;

            file_change change {};
            change.status = status.front();
            change.side = change_side::staged;
            std::string_view path {};
            if (change.status == 'R' || change.status == 'C')
            {
                std::string_view original {};
                if (git_text::take_field(rest, '\0', original) == false)
                    break;
                change.original_path = std::string { original };
            }
            if (git_text::take_field(rest, '\0', path) == false && path.empty())
                break;
            change.path = std::string { path };
            changes.push_back(std::move(change));
        }
        return changes;
    }

    in_progress_operation read_in_progress_operation(const repository_operation_state kind, const std::function<std::optional<std::string>(std::string_view relative_path)>& read)
    {
        in_progress_operation result {};
        result.kind = kind;
        if (kind == repository_operation_state::none || read == nullptr)
            return result;

        if (kind == repository_operation_state::rebase)
        {
            // merge 방식(기본)이 먼저다. apply 방식은 `rebase --apply`나 오래된 설정에서만 나온다.
            if (const auto msgnum { read("rebase-merge/msgnum") }; msgnum.has_value())
            {
                result.step = read_number(msgnum);
                result.total = read_number(read("rebase-merge/end"));
                result.onto = read_trimmed(read("rebase-merge/onto"));
                result.head_name = read_trimmed(read("rebase-merge/head-name"));
                if (const auto message { read("rebase-merge/message") })
                    result.current_summary = std::string { first_line(*message) };
            }
            else
            {
                result.step = read_number(read("rebase-apply/next"));
                result.total = read_number(read("rebase-apply/last"));
                result.onto = read_trimmed(read("rebase-apply/onto"));
                result.head_name = read_trimmed(read("rebase-apply/head-name"));
                if (const auto message { read("rebase-apply/msg") })
                    result.current_summary = std::string { first_line(*message) };
            }
            // detached HEAD에서 시작한 rebase는 head-name이 `detached HEAD`다.
            if (git_text::starts_with(result.head_name, "refs/") == false)
                result.head_name.clear();
            return result;
        }

        if (const auto message { read("MERGE_MSG") })
            result.current_summary = std::string { first_line(*message) };
        return result;
    }
} // namespace cyella
