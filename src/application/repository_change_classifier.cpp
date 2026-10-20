#include "application/repository_change_classifier.h"

#include "domain/path_syntax.h"
#include "domain/repository_path.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

namespace cyella {
    namespace {
        constexpr bool is_separator(const char8_t value) noexcept
        {
            return value == u8'\\' || value == u8'/';
        }

        constexpr char8_t ascii_lower(const char8_t value) noexcept
        {
            if (value >= u8'A' && value <= u8'Z')
                return static_cast<char8_t>(value - u8'A' + u8'a');
            return value;
        }

        // 구성 요소가 소문자 이름과 같은지 ASCII 대소문자를 무시하고 비교한다.
        bool equals_name(const std::u8string_view component, const std::u8string_view lowercase_name) noexcept
        {
            if (component.size() != lowercase_name.size())
                return false;
            for (std::size_t index = 0; index < component.size(); ++index)
                if (ascii_lower(component[index]) != lowercase_name[index])
                    return false;
            return true;
        }

        bool ends_with_lock(const std::u8string_view component) noexcept
        {
            constexpr std::u8string_view suffix { u8".lock" };
            if (component.size() < suffix.size())
                return false;
            return equals_name(component.substr(component.size() - suffix.size()), suffix);
        }

        // 구분자로 나눈 구성 요소다. 빈 요소(연속 구분자, 앞뒤 구분자)는 버린다.
        std::vector<std::u8string_view> split_components(const std::u8string_view path)
        {
            std::vector<std::u8string_view> components {};
            std::size_t start { 0 };
            for (std::size_t index = 0; index <= path.size(); ++index)
            {
                if (index != path.size() && is_separator(path[index]) == false)
                    continue;
                if (index > start)
                    components.push_back(path.substr(start, index - start));
                start = index + 1;
            }
            return components;
        }

        // 구성 요소 목록은 이미 `.git` 안쪽의 상대 경로다.
        change_classification classify_git_directory(const std::vector<std::u8string_view>& components, const bool common_directory) noexcept
        {
            if (components.empty())
                return change_classification::ignored;
            if (ends_with_lock(components.back()))
                return change_classification::ignored;

            const std::u8string_view first { components.front() };
            if (equals_name(first, u8"refs"))
                return change_classification::repository_state;
            if (components.size() == 1 && equals_name(first, u8"packed-refs"))
                return change_classification::repository_state;
            // 아래 항목은 worktree마다 따로 있다. 공용 디렉터리의 것은 다른 worktree의 상태다.
            if (common_directory)
                return change_classification::ignored;

            if (equals_name(first, u8"objects") || equals_name(first, u8"logs"))
                return change_classification::ignored;
            if (equals_name(first, u8"rebase-merge") || equals_name(first, u8"rebase-apply"))
                return change_classification::repository_state;

            // `config`는 브랜치의 upstream(`branch.<name>.remote`·`merge`)이 사는 곳이라 터미널의 `git branch -u`도 화면에 보여야 한다.
            static constexpr std::array<std::u8string_view, 8> single_files {
                u8"config",
                u8"head",
                u8"index",
                u8"fetch_head",
                u8"orig_head",
                u8"merge_head",
                u8"cherry_pick_head",
                u8"revert_head",
            };
            if (components.size() == 1)
                for (const std::u8string_view name : single_files)
                    if (equals_name(first, name))
                        return change_classification::repository_state;
            return change_classification::ignored;
        }

        // `gitdir: path`의 앞부분과 끝 공백을 걷어 낸다. git은 첫 줄만 본다.
        std::u8string_view first_line_value(std::u8string_view text) noexcept
        {
            const std::size_t line_end { text.find_first_of(u8"\r\n") };
            if (line_end != std::u8string_view::npos)
                text = text.substr(0, line_end);
            while (text.empty() == false && (text.back() == u8' ' || text.back() == u8'\t'))
                text.remove_suffix(1);
            while (text.empty() == false && (text.front() == u8' ' || text.front() == u8'\t'))
                text.remove_prefix(1);
            return text;
        }

        std::optional<std::u8string> join_directory(const std::u8string_view base, const std::u8string_view target)
        {
            if (target.empty())
                return std::nullopt;
            if (is_absolute_windows_path(target))
                return normalize_repository_path(target);
            // `/c/Users/..` 같은 MSYS 경로와 `\rooted`는 어느 드라이브인지 알 수 없어 거부한다.
            if (is_separator(target.front()))
                return std::nullopt;

            std::u8string joined { base };
            if (joined.empty() == false && is_separator(joined.back()) == false)
                joined.push_back(u8'\\');
            joined.append(target);
            return normalize_repository_path(joined);
        }
    } // namespace

    change_classification classify_repository_change(const watched_root_kind root, const std::u8string_view relative_path) noexcept
    {
        try
        {
            const std::vector<std::u8string_view> components { split_components(relative_path) };
            if (components.empty())
                return change_classification::ignored;

            switch (root)
            {
            case watched_root_kind::git_directory:
                return classify_git_directory(components, false);
            case watched_root_kind::common_directory:
                return classify_git_directory(components, true);
            case watched_root_kind::working_tree:
                break;
            }

            if (equals_name(components.front(), u8".git"))
                return classify_git_directory(std::vector<std::u8string_view>(components.begin() + 1, components.end()), false);
            // 중첩 저장소의 `.git` 내부는 이 저장소의 상태가 아니다.
            for (const std::u8string_view component : components)
                if (equals_name(component, u8".git"))
                    return change_classification::ignored;
            return change_classification::working_tree;
        }
        catch (...)
        {
            // 분류하지 못하면 놓치는 것보다 한 번 더 읽는 편이 안전하다.
            return change_classification::working_tree;
        }
    }

    change_debouncer::change_debouncer(const debounce_options& options) noexcept
        : options_ { options }
    {
        if (options_.maximum_wait < options_.window)
            options_.maximum_wait = options_.window;
    }

    void change_debouncer::note(const change_classification classification, const clock::time_point now) noexcept
    {
        if (classification == change_classification::ignored)
            return;

        if (pending_.any() == false)
            first_change_ = now;
        last_change_ = now;
        if (classification == change_classification::working_tree)
            pending_.working_tree = true;
        else
            pending_.repository_state = true;
    }

    void change_debouncer::note_overflow(const clock::time_point now) noexcept
    {
        note(change_classification::working_tree, now);
        note(change_classification::repository_state, now);
    }

    std::optional<change_debouncer::clock::time_point> change_debouncer::due_time() const noexcept
    {
        if (pending_.any() == false)
            return std::nullopt;
        return std::min(last_change_ + options_.window, first_change_ + options_.maximum_wait);
    }

    pending_changes change_debouncer::take_ready(const clock::time_point now) noexcept
    {
        const std::optional<clock::time_point> due { due_time() };
        if (due.has_value() == false || now < *due)
            return {};

        const pending_changes ready { pending_ };
        pending_ = {};
        return ready;
    }

    std::optional<std::u8string> resolve_gitdir_pointer(const std::u8string_view dot_git_content, const std::u8string_view dot_git_directory)
    {
        constexpr std::u8string_view prefix { u8"gitdir:" };
        const std::u8string_view line { first_line_value(dot_git_content) };
        if (line.substr(0, prefix.size()) != prefix)
            return std::nullopt;
        return join_directory(dot_git_directory, first_line_value(line.substr(prefix.size())));
    }

    std::optional<std::u8string> resolve_commondir_pointer(const std::u8string_view commondir_content, const std::u8string_view git_directory)
    {
        return join_directory(git_directory, first_line_value(commondir_content));
    }
} // namespace cyella
