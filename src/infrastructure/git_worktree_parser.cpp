#include "infrastructure/git_worktree_parser.h"

#include "domain/path_syntax.h"
#include "infrastructure/git_text.h"

#include <string>
#include <utility>

namespace cyella {
    namespace {
        constexpr std::string_view branch_prefix { "refs/heads/" };

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        // 줄이 `<키> <값>` 모양이면 값을, 키만 있으면 빈 값을 돌려준다. 키가 다르면 값이 없다.
        bool match_key(const std::string_view line, const std::string_view key, std::string_view& value) noexcept
        {
            if (git_text::starts_with(line, key) == false)
                return false;
            if (line.size() == key.size())
            {
                value = std::string_view {};
                return true;
            }
            if (line[key.size()] != ' ')
                return false;
            value = line.substr(key.size() + 1);
            return true;
        }

        void finish(std::vector<worktree_info>& result, worktree_info& current, bool& open)
        {
            if (open == false)
                return;
            if (current.branch_ref.starts_with(branch_prefix))
                current.branch = current.branch_ref.substr(branch_prefix.size());
            current.is_main = result.empty();
            result.push_back(std::move(current));
            current = {};
            open = false;
        }
    } // namespace

    std::vector<worktree_info> parse_git_worktree_list(const std::string_view bytes)
    {
        std::vector<worktree_info> result {};
        worktree_info current {};
        bool open { false };
        std::string_view rest { bytes };
        while (rest.empty() == false)
        {
            std::string_view line {};
            git_text::take_field(rest, '\n', line);
            if (line.empty() == false && line.back() == '\r')
                line.remove_suffix(1);
            if (line.empty())
            {
                finish(result, current, open);
                continue;
            }

            std::string_view value {};
            if (match_key(line, "worktree", value))
            {
                // 앞 항목이 빈 줄 없이 끝났어도 새 항목으로 넘어간다.
                finish(result, current, open);
                current.path = std::string { value };
                open = true;
            }
            else if (open == false)
            {
                continue;
            }
            else if (match_key(line, "HEAD", value))
            {
                current.head_sha = std::string { value };
            }
            else if (match_key(line, "branch", value))
            {
                current.branch_ref = std::string { value };
            }
            else if (line == "detached")
            {
                current.detached = true;
            }
            else if (line == "bare")
            {
                current.bare = true;
            }
            else if (match_key(line, "locked", value))
            {
                current.locked = true;
                current.lock_reason = std::string { value };
            }
            else if (match_key(line, "prunable", value))
            {
                current.prunable = true;
                current.prunable_reason = std::string { value };
            }
        }
        finish(result, current, open);
        return result;
    }

    void apply_worktree_branches(const std::span<ref> refs, const std::span<const worktree_info> worktrees, const std::u8string_view current_path_key)
    {
        for (const worktree_info& worktree : worktrees)
        {
            if (worktree.branch_ref.empty() || normalize_path_key(to_u8(worktree.path)) == current_path_key)
                continue;
            for (ref& value : refs)
                if (value.kind == ref_kind::local && value.full_name == worktree.branch_ref)
                    value.worktree_path = worktree.path;
        }
    }
} // namespace cyella
