#include "domain/interactive_rebase.h"

#include <algorithm>
#include <array>
#include <set>
#include <utility>

namespace cyella {
    namespace {
        constexpr std::string_view fixup_prefix { "fixup! " };
        constexpr std::string_view squash_prefix { "squash! " };
        constexpr std::string_view amend_prefix { "amend! " };

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::u8string short_sha(const std::string_view sha)
        {
            return to_u8(sha.substr(0, std::min<std::size_t>(sha.size(), 7)));
        }

        std::string_view trim(std::string_view text) noexcept
        {
            constexpr std::string_view whitespace { " \t\r\n" };
            const std::size_t begin { text.find_first_not_of(whitespace) };
            if (begin == std::string_view::npos)
                return {};
            const std::size_t end { text.find_last_not_of(whitespace) };
            return text.substr(begin, end - begin + 1);
        }

        // 공백으로 나눈 다음 낱말을 꺼낸다.
        std::string_view take_word(std::string_view& text) noexcept
        {
            text = trim(text);
            const std::size_t end { text.find_first_of(" \t") };
            const std::string_view word { text.substr(0, end) };
            text = end == std::string_view::npos ? std::string_view {} : text.substr(end);
            return word;
        }

        bool starts_with(const std::string_view text, const std::string_view prefix) noexcept
        {
            return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
        }

        // 메시지에 주석과 공백이 아닌 글이 있다. git은 편집기에서 돌아온 메시지의 `#` 줄을 지우므로 그 줄만 있는
        // 메시지는 빈 메시지가 되어 커밋이 실패한다.
        bool has_meaningful_text(const std::string_view message)
        {
            std::string_view rest { message };
            while (rest.empty() == false)
            {
                const std::size_t end { rest.find('\n') };
                const std::string_view line { trim(rest.substr(0, end)) };
                if (line.empty() == false && line.front() != '#')
                    return true;
                if (end == std::string_view::npos)
                    break;
                rest.remove_prefix(end + 1);
            }
            return false;
        }

        std::string normalized_command(const std::string_view word)
        {
            static constexpr std::pair<std::string_view, std::string_view> names[] {
                { "p", "pick" },
                { "pick", "pick" },
                { "r", "reword" },
                { "reword", "reword" },
                { "e", "edit" },
                { "edit", "edit" },
                { "s", "squash" },
                { "squash", "squash" },
                { "f", "fixup" },
                { "fixup", "fixup" },
                { "d", "drop" },
                { "drop", "drop" },
            };
            for (const auto& [alias, name] : names)
                if (word == alias)
                    return std::string { name };
            return {};
        }

        const rebase_commit* find_commit(const std::span<const rebase_commit> commits, const std::string_view sha)
        {
            for (const rebase_commit& commit : commits)
                if (commit.sha == sha)
                    return &commit;
            return nullptr;
        }

        std::string first_line(const std::string_view text)
        {
            return std::string { trim(text.substr(0, text.find('\n'))) };
        }
    } // namespace

    std::string_view rebase_action_word(const rebase_action action) noexcept
    {
        switch (action)
        {
        case rebase_action::pick:
            return "pick";
        case rebase_action::reword:
            return "reword";
        case rebase_action::squash:
            return "squash";
        case rebase_action::fixup:
            return "fixup";
        case rebase_action::fixup_keep_message:
            return "fixup -C";
        case rebase_action::drop:
            return "drop";
        }
        return "pick";
    }

    std::u8string_view rebase_action_label(const rebase_action action) noexcept
    {
        switch (action)
        {
        case rebase_action::pick:
            return u8"pick";
        case rebase_action::reword:
            return u8"reword";
        case rebase_action::squash:
            return u8"squash";
        case rebase_action::fixup:
            return u8"fixup";
        case rebase_action::fixup_keep_message:
            return u8"fixup -C";
        case rebase_action::drop:
            return u8"drop";
        }
        return u8"pick";
    }

    std::optional<rebase_action> rebase_action_for_key(const char32_t key) noexcept
    {
        switch (key)
        {
        case U'p':
        case U'P':
            return rebase_action::pick;
        case U'r':
        case U'R':
            return rebase_action::reword;
        case U's':
        case U'S':
            return rebase_action::squash;
        case U'f':
        case U'F':
            return rebase_action::fixup;
        case U'd':
        case U'D':
            return rebase_action::drop;
        default:
            break;
        }
        return std::nullopt;
    }

    bool is_meld_action(const rebase_action action) noexcept
    {
        return action == rebase_action::squash || action == rebase_action::fixup || action == rebase_action::fixup_keep_message;
    }

    std::vector<rebase_group> rebase_groups(const std::span<const rebase_todo_entry> todo)
    {
        std::vector<rebase_group> groups {};
        for (std::size_t index { 0 }; index < todo.size(); ++index)
        {
            const rebase_action action { todo[index].action };
            if (action == rebase_action::drop)
                continue;
            if (is_meld_action(action) && groups.empty() == false)
            {
                rebase_group& group { groups.back() };
                group.last = index;
                group.members.push_back(index);
                if (action == rebase_action::squash)
                    group.has_squash = true;
                continue;
            }
            rebase_group group {};
            group.head = index;
            group.last = index;
            group.members.push_back(index);
            group.has_squash = action == rebase_action::squash;
            groups.push_back(std::move(group));
        }
        return groups;
    }

    std::optional<rebase_todo_issue> find_rebase_todo_problem(const std::span<const rebase_todo_entry> todo, const std::span<const std::string> range_shas)
    {
        if (todo.empty())
            return rebase_todo_issue { rebase_todo_problem::empty, 0, {} };

        const std::set<std::string_view> range { range_shas.begin(), range_shas.end() };
        std::set<std::string_view> seen {};
        for (std::size_t index { 0 }; index < todo.size(); ++index)
        {
            const rebase_todo_entry& entry { todo[index] };
            if (range.contains(entry.sha) == false)
                return rebase_todo_issue { rebase_todo_problem::unknown_commit, index, entry.sha };
            if (seen.insert(entry.sha).second == false)
                return rebase_todo_issue { rebase_todo_problem::duplicate_commit, index, entry.sha };
        }
        for (const std::string& sha : range_shas)
            if (seen.contains(sha) == false)
                return rebase_todo_issue { rebase_todo_problem::missing_commit, 0, sha };

        // 합칠 앞 커밋이 없는 squash·fixup이다. git은 "cannot 'squash' without a previous commit"으로 시작하지 않는다.
        for (std::size_t index { 0 }; index < todo.size(); ++index)
        {
            if (todo[index].action == rebase_action::drop)
                continue;
            if (is_meld_action(todo[index].action))
                return rebase_todo_issue { rebase_todo_problem::starts_with_meld, index, todo[index].sha };
            break;
        }

        // 메시지를 받을 수 있는 줄: reword, squash가 든 묶음의 마지막 줄.
        std::vector<bool> accepts(todo.size(), false);
        for (const rebase_group& group : rebase_groups(todo))
            if (group.has_squash)
                accepts[group.last] = true;
        for (std::size_t index { 0 }; index < todo.size(); ++index)
        {
            const rebase_todo_entry& entry { todo[index] };
            if (entry.action == rebase_action::reword)
            {
                if (entry.message.has_value() == false)
                    return rebase_todo_issue { rebase_todo_problem::reword_without_message, index, entry.sha };
                if (has_meaningful_text(*entry.message) == false)
                    return rebase_todo_issue { rebase_todo_problem::empty_message, index, entry.sha };
                continue;
            }
            if (entry.message.has_value() == false)
                continue;
            if (accepts[index] == false)
                return rebase_todo_issue { rebase_todo_problem::message_not_allowed, index, entry.sha };
            if (has_meaningful_text(*entry.message) == false)
                return rebase_todo_issue { rebase_todo_problem::empty_message, index, entry.sha };
        }
        return std::nullopt;
    }

    std::u8string rebase_todo_problem_text(const rebase_todo_issue& issue)
    {
        const std::u8string where { short_sha(issue.sha) };
        switch (issue.problem)
        {
        case rebase_todo_problem::empty:
            return u8"rebase할 커밋이 없습니다.";
        case rebase_todo_problem::starts_with_meld:
            return u8"첫 커밋(" + where + u8")은 squash·fixup할 수 없습니다. 합칠 앞 커밋이 없습니다.";
        case rebase_todo_problem::unknown_commit:
            return where + u8" 커밋은 rebase 범위에 없습니다. 편집기를 다시 여세요.";
        case rebase_todo_problem::duplicate_commit:
            return where + u8" 커밋이 목록에 두 번 있습니다.";
        case rebase_todo_problem::missing_commit:
            return where + u8" 커밋이 목록에서 빠졌습니다. 지우려면 drop으로 두세요.";
        case rebase_todo_problem::reword_without_message:
            return where + u8" 커밋의 새 메시지가 없습니다.";
        case rebase_todo_problem::message_not_allowed:
            return where + u8" 줄에는 메시지를 둘 수 없습니다.";
        case rebase_todo_problem::empty_message:
            return where + u8" 커밋의 메시지가 비었습니다.";
        }
        return u8"할 일 목록이 올바르지 않습니다.";
    }

    std::string serialize_rebase_todo(const std::span<const rebase_todo_entry> todo)
    {
        std::string text {};
        for (const rebase_todo_entry& entry : todo)
        {
            text.append(rebase_action_word(entry.action));
            text.push_back(' ');
            text.append(entry.sha);
            text.push_back('\n');
        }
        return text;
    }

    std::string normalize_commit_message(const std::string_view message)
    {
        std::string text {};
        text.reserve(message.size() + 1);
        for (std::size_t index { 0 }; index < message.size(); ++index)
        {
            const char value { message[index] };
            if (value == '\r')
            {
                // CRLF와 홀로 선 CR을 모두 LF 하나로 바꾼다.
                text.push_back('\n');
                if (index + 1 < message.size() && message[index + 1] == '\n')
                    ++index;
                continue;
            }
            text.push_back(value);
        }
        while (text.empty() == false && (text.back() == '\n' || text.back() == ' ' || text.back() == '\t'))
            text.pop_back();
        // 앞쪽 빈 줄도 git이 지운다. 화면 초안과 파일이 같게 미리 지운다.
        const std::size_t begin { text.find_first_not_of('\n') };
        if (begin == std::string::npos)
            return {};
        text.erase(0, begin);
        text.push_back('\n');
        return text;
    }

    std::vector<rebase_message_file> rebase_message_files(const std::span<const rebase_todo_entry> todo)
    {
        std::vector<rebase_message_file> files {};
        for (const rebase_todo_entry& entry : todo)
            if (entry.message.has_value() && entry.action != rebase_action::drop && entry.action != rebase_action::pick)
                files.push_back({ entry.sha, normalize_commit_message(*entry.message) });
        return files;
    }

    // ------------------------------------------------------------------ 편집기 초기값

    autosquash_marker autosquash_marker_of(const std::string_view subject) noexcept
    {
        if (starts_with(subject, fixup_prefix))
            return autosquash_marker::fixup;
        if (starts_with(subject, squash_prefix))
            return autosquash_marker::squash;
        if (starts_with(subject, amend_prefix))
            return autosquash_marker::amend;
        return autosquash_marker::none;
    }

    std::vector<rebase_todo_entry> default_rebase_todo(const std::span<const rebase_commit> commits)
    {
        std::vector<rebase_todo_entry> todo {};
        todo.reserve(commits.size());
        for (const rebase_commit& commit : commits)
            todo.push_back({ rebase_action::pick, commit.sha, std::nullopt });
        return todo;
    }

    std::vector<rebase_todo_entry> autosquash_rebase_todo(const std::span<const rebase_commit> commits)
    {
        // git `todo_list_rearrange_squash`와 같다. 대상마다 뒤에 붙을 줄 목록을 모은 뒤 대상 뒤에 펼친다.
        const std::size_t count { commits.size() };
        std::vector<std::vector<std::size_t>> followers(count);
        std::vector<bool> moved(count, false);
        std::vector<rebase_action> actions(count, rebase_action::pick);
        // 표지 커밋이 다른 표지 커밋을 대상으로 삼으면 그 대상의 뿌리 뒤에 붙는다.
        std::vector<std::size_t> root(count);
        for (std::size_t index { 0 }; index < count; ++index)
            root[index] = index;

        for (std::size_t index { 0 }; index < count; ++index)
        {
            const std::string_view subject { commits[index].subject };
            const autosquash_marker marker { autosquash_marker_of(subject) };
            if (marker == autosquash_marker::none)
                continue;
            std::string_view rest { subject };
            // `fixup! fixup! X`처럼 표지가 겹치면 모두 벗긴다.
            while (true)
                if (starts_with(rest, fixup_prefix))
                    rest.remove_prefix(fixup_prefix.size());
                else if (starts_with(rest, squash_prefix))
                    rest.remove_prefix(squash_prefix.size());
                else if (starts_with(rest, amend_prefix))
                    rest.remove_prefix(amend_prefix.size());
                else
                    break;
            std::optional<std::size_t> target {};
            for (std::size_t earlier { 0 }; earlier < index && target.has_value() == false; ++earlier)
                if (commits[earlier].subject == rest)
                    target = earlier;
            if (target.has_value() == false && rest.empty() == false && rest.find(' ') == std::string_view::npos && is_hex_object_id(rest))
                for (std::size_t earlier { 0 }; earlier < index && target.has_value() == false; ++earlier)
                    if (starts_with(commits[earlier].sha, rest))
                        target = earlier;
            if (target.has_value() == false && rest.empty() == false)
                for (std::size_t earlier { 0 }; earlier < index && target.has_value() == false; ++earlier)
                    if (starts_with(commits[earlier].subject, rest))
                        target = earlier;
            if (target.has_value() == false)
                continue;

            const std::size_t anchor { root[*target] };
            followers[anchor].push_back(index);
            moved[index] = true;
            root[index] = anchor;
            actions[index] = marker == autosquash_marker::squash ? rebase_action::squash : marker == autosquash_marker::amend ? rebase_action::fixup_keep_message : rebase_action::fixup;
        }

        std::vector<rebase_todo_entry> todo {};
        todo.reserve(count);
        for (std::size_t index { 0 }; index < count; ++index)
        {
            if (moved[index])
                continue;
            todo.push_back({ actions[index], commits[index].sha, std::nullopt });
            for (const std::size_t follower : followers[index])
                todo.push_back({ actions[follower], commits[follower].sha, std::nullopt });
        }
        return todo;
    }

    std::string full_commit_message(const rebase_commit& commit)
    {
        std::string message { commit.subject };
        const std::string_view body { trim(commit.body) };
        if (body.empty() == false)
        {
            message.append("\n\n");
            message.append(body);
        }
        return message;
    }

    std::string combined_squash_message(const std::span<const rebase_todo_entry> todo, const rebase_group& group, const std::span<const std::string> messages)
    {
        std::vector<std::string_view> parts {};
        for (const std::size_t index : group.members)
        {
            if (index >= todo.size() || index >= messages.size())
                continue;
            switch (todo[index].action)
            {
            case rebase_action::fixup:
                break;
            case rebase_action::fixup_keep_message:
                // 그때까지 모은 메시지를 이 커밋의 것으로 바꾼다.
                parts.clear();
                parts.push_back(trim(messages[index]));
                break;
            case rebase_action::pick:
            case rebase_action::reword:
            case rebase_action::squash:
            case rebase_action::drop:
                parts.push_back(trim(messages[index]));
                break;
            }
        }
        std::string message {};
        for (const std::string_view part : parts)
        {
            if (part.empty())
                continue;
            if (message.empty() == false)
                message.append("\n\n");
            message.append(part);
        }
        return message;
    }

    // ------------------------------------------------------------------ 결과 미리보기

    rebase_preview preview_rebase(const std::span<const rebase_todo_entry> todo, const std::span<const rebase_commit> commits)
    {
        rebase_preview preview {};
        preview.original_count = todo.size();
        for (const rebase_todo_entry& entry : todo)
            if (entry.action == rebase_action::drop)
                ++preview.dropped_count;

        // 원래 순서에서 남는 커밋의 자리다. 줄의 상대 순서가 바뀌었는지 본다.
        std::size_t last_position { 0 };
        bool first { true };
        for (const rebase_todo_entry& entry : todo)
        {
            if (entry.action == rebase_action::drop)
                continue;
            std::size_t position { 0 };
            while (position < commits.size() && commits[position].sha != entry.sha)
                ++position;
            if (first == false && position < last_position)
                preview.reordered = true;
            last_position = position;
            first = false;
        }

        // 앞에서 무언가 바뀌면 뒤의 커밋은 모두 새 부모 위에 새로 만들어진다.
        bool changed_before { false };
        std::size_t original_position { 0 };
        for (const rebase_group& group : rebase_groups(todo))
        {
            rebase_result_commit result {};
            result.head = group.head;
            result.combined = group.members.size();
            const rebase_todo_entry& head { todo[group.head] };
            const rebase_commit* const original { find_commit(commits, head.sha) };
            std::string subject { original != nullptr ? original->subject : std::string {} };
            if (head.action == rebase_action::reword && head.message.has_value())
            {
                subject = first_line(normalize_commit_message(*head.message));
                result.message_changed = true;
            }
            for (const std::size_t member : group.members)
            {
                const rebase_todo_entry& entry { todo[member] };
                if (entry.action == rebase_action::fixup_keep_message)
                {
                    const rebase_commit* const replaced { find_commit(commits, entry.sha) };
                    if (replaced != nullptr)
                        subject = replaced->subject;
                    result.message_changed = true;
                }
                else if (entry.action == rebase_action::squash)
                    result.message_changed = true;
            }
            const rebase_todo_entry& last { todo[group.last] };
            if (group.has_squash && last.message.has_value())
                subject = first_line(normalize_commit_message(*last.message));
            result.subject = std::move(subject);

            const bool same_place { original_position < commits.size() && commits[original_position].sha == head.sha };
            result.rewritten = changed_before || same_place == false || result.combined > 1 || result.message_changed;
            changed_before = changed_before || result.rewritten;
            original_position += 1;
            preview.commits.push_back(std::move(result));
        }
        return preview;
    }

    // ------------------------------------------------------------------ git이 쓴 파일 읽기

    std::optional<rebase_todo_line> parse_rebase_todo_line(const std::string_view line)
    {
        std::string_view rest { trim(line) };
        if (rest.empty() || rest.front() == '#')
            return std::nullopt;
        const std::string command { normalized_command(take_word(rest)) };
        if (command.empty())
            return std::nullopt;
        rebase_todo_line parsed {};
        parsed.command = command;
        std::string_view word { take_word(rest) };
        if (command == "fixup" && (word == "-C" || word == "-c"))
        {
            parsed.flag = std::string { word };
            word = take_word(rest);
        }
        if (is_hex_object_id(word) == false)
            return std::nullopt;
        parsed.sha = std::string { word };
        return parsed;
    }

    std::vector<std::string> rebase_todo_commits(const std::string_view todo_text)
    {
        std::vector<std::string> shas {};
        std::string_view rest { todo_text };
        while (rest.empty() == false)
        {
            const std::size_t end { rest.find('\n') };
            if (const auto parsed { parse_rebase_todo_line(rest.substr(0, end)) })
                shas.push_back(parsed->sha);
            if (end == std::string_view::npos)
                break;
            rest.remove_prefix(end + 1);
        }
        return shas;
    }

    bool rebase_todo_matches(const std::string_view git_todo, const std::span<const std::string> expected_shas)
    {
        const std::vector<std::string> listed { rebase_todo_commits(git_todo) };
        if (listed.size() != expected_shas.size())
            return false;
        std::vector<bool> used(expected_shas.size(), false);
        for (const std::string& short_id : listed)
        {
            std::optional<std::size_t> match {};
            for (std::size_t index { 0 }; index < expected_shas.size(); ++index)
            {
                if (starts_with(expected_shas[index], short_id) == false)
                    continue;
                // 줄인 SHA가 둘 이상과 맞으면 어느 것인지 알 수 없다.
                if (match.has_value())
                    return false;
                match = index;
            }
            if (match.has_value() == false || used[*match])
                return false;
            used[*match] = true;
        }
        return true;
    }

    std::optional<rebase_todo_line> last_done_line(const std::string_view done_text)
    {
        std::optional<rebase_todo_line> last {};
        std::string_view rest { done_text };
        while (rest.empty() == false)
        {
            const std::size_t end { rest.find('\n') };
            if (auto parsed { parse_rebase_todo_line(rest.substr(0, end)) })
                last = std::move(parsed);
            if (end == std::string_view::npos)
                break;
            rest.remove_prefix(end + 1);
        }
        return last;
    }

    std::optional<std::string> message_key_for_editor(const std::string_view done_text)
    {
        const std::optional<rebase_todo_line> last { last_done_line(done_text) };
        if (last.has_value() == false)
            return std::nullopt;
        if (last->command == "reword" || last->command == "squash" || last->command == "fixup")
            return last->sha;
        return std::nullopt;
    }

    bool is_hex_object_id(const std::string_view text) noexcept
    {
        if (text.size() < 4 || text.size() > 64)
            return false;
        return std::ranges::all_of(text, [](const char value) noexcept { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
    }
} // namespace cyella
