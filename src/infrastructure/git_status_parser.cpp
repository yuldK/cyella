#include "infrastructure/git_status_parser.h"

#include "infrastructure/git_text.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace cyella {
    namespace {
        // 앞의 `count`개 필드를 공백으로 잘라 fields에 담고, 나머지(경로, 공백을 포함할 수
        // 있다)를 tail로 돌려준다. 필드가 모자라면 false다.
        template<std::size_t count>
        bool split_leading_fields(std::string_view record, std::array<std::string_view, count>& fields, std::string_view& tail) noexcept
        {
            for (std::size_t index { 0 }; index < count; ++index)
                if (git_text::take_field(record, ' ', fields[index]) == false)
                    return false;
            tail = record;
            return tail.empty() == false;
        }

        bool is_status_letter(const char value) noexcept
        {
            return value == '.' || (value >= 'A' && value <= 'Z');
        }

        void parse_branch_header(const std::string_view header, git_branch_status& branch)
        {
            // `# ` 뒤가 `<key> <value>`다.
            std::string_view rest { header.substr(2) };
            std::string_view key {};
            git_text::take_field(rest, ' ', key);

            if (key == "branch.oid")
            {
                if (rest == "(initial)")
                    branch.is_initial = true;
                else
                    branch.oid = std::string { rest };
            }
            else if (key == "branch.head")
            {
                if (rest == "(detached)")
                    branch.is_detached = true;
                else
                    branch.head = std::string { rest };
            }
            else if (key == "branch.upstream")
            {
                branch.upstream = std::string { rest };
            }
            else if (key == "branch.ab")
            {
                // `+<ahead> -<behind>`
                std::string_view ahead_text {};
                git_text::take_field(rest, ' ', ahead_text);
                if (ahead_text.size() >= 2 && rest.size() >= 2 && ahead_text.front() == '+' && rest.front() == '-')
                {
                    const auto ahead { git_text::parse_integer<std::uint32_t>(ahead_text.substr(1)) };
                    const auto behind { git_text::parse_integer<std::uint32_t>(rest.substr(1)) };
                    if (ahead && behind)
                    {
                        branch.has_ahead_behind = true;
                        branch.ahead = *ahead;
                        branch.behind = *behind;
                    }
                }
            }
            // 그 밖의 머리말(`# stash N` 등)은 무시한다.
        }

        file_change make_change(const std::string_view path, const char status, const change_side side, const bool is_submodule)
        {
            file_change change {};
            change.path = std::string { path };
            change.status = status;
            change.side = side;
            change.is_submodule = is_submodule;
            return change;
        }

        // `1 XY sub mH mI mW hH hI path` (ordinary)와
        // `2 XY sub mH mI mW hH hI Xscore path` (rename/copy, 원래 경로는 다음 레코드).
        bool parse_tracked_entry(const std::string_view record, const bool is_rename, const std::string_view original_path, git_status_result& result)
        {
            std::string_view tail {};
            std::string_view xy {};
            std::string_view submodule {};
            if (is_rename)
            {
                std::array<std::string_view, 9> fields {};
                if (split_leading_fields(record, fields, tail) == false)
                    return false;
                xy = fields[1];
                submodule = fields[2];
            }
            else
            {
                std::array<std::string_view, 8> fields {};
                if (split_leading_fields(record, fields, tail) == false)
                    return false;
                xy = fields[1];
                submodule = fields[2];
            }

            if (xy.size() != 2 || is_status_letter(xy[0]) == false || is_status_letter(xy[1]) == false)
                return false;

            const bool is_submodule { submodule.empty() == false && submodule.front() == 'S' };
            if (xy[0] != '.')
            {
                file_change change { make_change(tail, xy[0], change_side::staged, is_submodule) };
                if (is_rename)
                    change.original_path = std::string { original_path };
                if (is_submodule)
                    change.submodule_flags = std::string { submodule };
                result.staged.push_back(std::move(change));
            }
            if (xy[1] != '.')
            {
                file_change change { make_change(tail, xy[1], change_side::unstaged, is_submodule) };
                if (is_submodule)
                    change.submodule_flags = std::string { submodule };
                result.unstaged.push_back(std::move(change));
            }
            return true;
        }

        // `u XY sub m1 m2 m3 mW h1 h2 h3 path`
        bool parse_unmerged_entry(const std::string_view record, git_status_result& result)
        {
            std::array<std::string_view, 10> fields {};
            std::string_view tail {};
            if (split_leading_fields(record, fields, tail) == false)
                return false;
            if (fields[1].size() != 2)
                return false;

            file_change change { make_change(tail, 'U', change_side::unstaged, fields[2].empty() == false && fields[2].front() == 'S') };
            change.conflict_code = std::string { fields[1] };
            result.conflicts.push_back(std::move(change));
            return true;
        }
    } // namespace

    git_status_result parse_git_status(const std::string_view bytes)
    {
        git_status_result result {};
        std::string_view rest { bytes };

        while (rest.empty() == false)
        {
            std::string_view record {};
            git_text::take_field(rest, '\0', record);
            if (record.empty())
                continue;

            bool accepted { true };
            if (git_text::starts_with(record, "# "))
            {
                parse_branch_header(record, result.branch);
            }
            else if (record.size() >= 3 && record[1] == ' ')
            {
                switch (record.front())
                {
                case '1':
                    accepted = parse_tracked_entry(record, false, {}, result);
                    break;
                case '2': {
                    // 원래 경로는 바로 다음 NUL 레코드다. 없으면 잘린 출력이다.
                    std::string_view original_path {};
                    if (rest.empty())
                    {
                        accepted = false;
                        break;
                    }
                    git_text::take_field(rest, '\0', original_path);
                    accepted = parse_tracked_entry(record, true, original_path, result);
                    break;
                }
                case 'u':
                    accepted = parse_unmerged_entry(record, result);
                    break;
                case '?':
                    result.unstaged.push_back(make_change(record.substr(2), '?', change_side::unstaged, false));
                    break;
                case '!':
                    result.ignored.push_back(make_change(record.substr(2), '!', change_side::unstaged, false));
                    break;
                default:
                    accepted = false;
                    break;
                }
            }
            else
            {
                accepted = false;
            }

            if (accepted == false)
                ++result.skipped_entry_count;
        }
        return result;
    }

    void apply_git_status(repository_summary& summary, const git_status_result& status)
    {
        const git_branch_status& branch { status.branch };
        summary.current_branch = branch.head;
        summary.is_detached = branch.is_detached;
        summary.head_sha = branch.oid;
        summary.upstream = branch.upstream;
        summary.has_upstream = branch.upstream.empty() == false;
        summary.ahead = branch.has_ahead_behind ? branch.ahead : 0;
        summary.behind = branch.has_ahead_behind ? branch.behind : 0;
        summary.conflict_count = static_cast<std::uint32_t>(status.conflicts.size());

        // 같은 파일이 staged와 unstaged에 모두 있어도 한 번만 센다.
        std::vector<std::string_view> paths {};
        paths.reserve(status.staged.size() + status.unstaged.size() + status.conflicts.size());
        for (const file_change& change : status.staged)
            paths.push_back(change.path);
        for (const file_change& change : status.unstaged)
            paths.push_back(change.path);
        for (const file_change& change : status.conflicts)
            paths.push_back(change.path);
        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
        summary.changed_file_count = static_cast<std::uint32_t>(paths.size());
    }
} // namespace cyella
