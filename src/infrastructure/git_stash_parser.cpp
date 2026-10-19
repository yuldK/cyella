#include "infrastructure/git_stash_parser.h"

#include "infrastructure/git_text.h"

#include <cstdint>
#include <utility>

namespace cyella {
    namespace {
        constexpr char field_separator { '\x1F' };

        // `stash@{N}`에서 N을 읽는다.
        bool parse_stash_index(const std::string_view name, std::uint32_t& index) noexcept
        {
            constexpr std::string_view prefix { "stash@{" };
            if (git_text::starts_with(name, prefix) == false || name.back() != '}')
                return false;
            const auto value { git_text::parse_integer<std::uint32_t>(name.substr(prefix.size(), name.size() - prefix.size() - 1)) };
            if (value.has_value() == false)
                return false;
            index = *value;
            return true;
        }

        // `WIP on main: abc1234 subject` / `On main: custom message`을 나눈다.
        void split_subject(const std::string_view subject, stash_entry& entry)
        {
            std::string_view after_prefix {};
            if (git_text::starts_with(subject, "WIP on "))
            {
                entry.is_automatic_message = true;
                after_prefix = subject.substr(7);
            }
            else if (git_text::starts_with(subject, "On "))
            {
                after_prefix = subject.substr(3);
            }
            else
            {
                entry.message = std::string { subject };
                return;
            }

            // 브랜치 이름에는 ':'를 쓸 수 없으므로 첫 ": "가 경계다.
            const std::size_t colon { after_prefix.find(": ") };
            if (colon == std::string_view::npos)
            {
                entry.message = std::string { subject };
                return;
            }
            entry.branch = std::string { after_prefix.substr(0, colon) };
            entry.message = std::string { after_prefix.substr(colon + 2) };
        }
    } // namespace

    git_stash_list parse_git_stash_list(const std::string_view bytes)
    {
        git_stash_list result {};
        std::string_view rest { bytes };

        while (rest.empty() == false)
        {
            std::string_view record {};
            git_text::take_field(rest, '\0', record);
            while (record.empty() == false && (record.front() == '\n' || record.front() == '\r'))
                record.remove_prefix(1);
            if (record.empty())
                continue;

            std::string_view name {};
            std::string_view sha {};
            std::string_view parents {};
            std::string_view time {};
            const bool complete = git_text::take_field(record, field_separator, name) && git_text::take_field(record, field_separator, sha) && git_text::take_field(record, field_separator, parents)
                && git_text::take_field(record, field_separator, time);

            stash_entry entry {};
            const auto commit_time { git_text::parse_integer<std::int64_t>(time) };
            if (complete == false || parse_stash_index(name, entry.index) == false || commit_time.has_value() == false || sha.empty())
            {
                ++result.skipped_count;
                continue;
            }

            entry.name = std::string { name };
            entry.sha = std::string { sha };
            entry.commit_time = *commit_time;

            // 부모는 [base, index, (untracked)] 순서다.
            std::size_t parent_count { 0 };
            while (parents.empty() == false)
            {
                std::string_view parent {};
                git_text::take_field(parents, ' ', parent);
                if (parent.empty())
                    continue;
                if (parent_count == 0)
                    entry.base_sha = std::string { parent };
                ++parent_count;
            }
            entry.has_untracked = parent_count >= 3;

            split_subject(record, entry);
            result.entries.push_back(std::move(entry));
        }
        return result;
    }
} // namespace cyella
