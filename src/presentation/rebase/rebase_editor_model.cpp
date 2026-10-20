#include "presentation/rebase/rebase_editor_model.h"

#include <algorithm>
#include <utility>

namespace cyella {
    namespace {
        constexpr std::u8string_view row_prefix { u8"rebase-row:" };

        const rebase_commit* find_commit(const rebase_plan& plan, const std::string_view sha)
        {
            for (const rebase_commit& commit : plan.commits)
                if (commit.sha == sha)
                    return &commit;
            return nullptr;
        }

        std::string original_message(const rebase_editor_state& state, const std::string_view sha)
        {
            const rebase_commit* const commit { find_commit(state.plan, sha) };
            return commit != nullptr ? full_commit_message(*commit) : std::string {};
        }

        bool has_markers(const rebase_plan& plan)
        {
            return std::ranges::any_of(plan.commits, [](const rebase_commit& commit) { return autosquash_marker_of(commit.subject) != autosquash_marker::none; });
        }

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::string to_text(const std::u8string_view text)
        {
            return std::string { text.begin(), text.end() };
        }

        luil::text::text_edit_state edit_state_of(std::string text)
        {
            luil::text::text_edit_state state {};
            state.text = to_u8(text);
            state.caret = state.text.size();
            state.anchor = state.caret;
            return state;
        }

        // 묶음 하나의 첫 줄 SHA다. squash가 든 묶음에 속하지 않으면 빈 값이다.
        std::optional<std::string> squash_group_head(const rebase_editor_state& state, const std::size_t index)
        {
            for (const rebase_group& group : rebase_groups(state.todo))
                if (group.has_squash && std::ranges::find(group.members, index) != group.members.end())
                    return state.todo[group.head].sha;
            return std::nullopt;
        }

        void place_caret(luil::text::text_edit_state& state, const std::size_t offset, const bool extend)
        {
            state.caret = offset;
            if (extend == false)
                state.anchor = offset;
            state.group = luil::text::text_edit_group::none;
        }

        // UTF-8에서 앞의 코드 포인트 수다.
        std::size_t code_points(const std::u8string_view text)
        {
            return static_cast<std::size_t>(std::ranges::count_if(text, [](const char8_t value) { return (static_cast<unsigned>(value) & 0xC0u) != 0x80u; }));
        }

        // 줄 `line`(UTF-8) 안에서 코드 포인트 `column`개를 지난 byte 자리다. 줄보다 길면 줄 끝이다.
        std::size_t offset_at_column(const std::u8string_view line, const std::size_t column)
        {
            std::size_t seen { 0 };
            for (std::size_t offset { 0 }; offset < line.size(); ++offset)
            {
                if ((static_cast<unsigned>(line[offset]) & 0xC0u) == 0x80u)
                    continue;
                if (seen == column)
                    return offset;
                ++seen;
            }
            return line.size();
        }
    } // namespace

    std::u8string rebase_row_key(const std::string_view sha)
    {
        return std::u8string { row_prefix } + to_u8(sha);
    }

    std::optional<std::string> rebase_row_sha(const std::u8string_view key)
    {
        if (key.size() <= row_prefix.size() || key.substr(0, row_prefix.size()) != row_prefix)
            return std::nullopt;
        return to_text(key.substr(row_prefix.size()));
    }

    void start_rebase_editing(rebase_editor_state& state, rebase_plan plan)
    {
        state.plan = std::move(plan);
        state.phase = rebase_editor_phase::editing;
        state.error.clear();
        state.autosquash = has_markers(state.plan);
        state.todo = state.autosquash ? autosquash_rebase_todo(state.plan.commits) : default_rebase_todo(state.plan.commits);
        state.drafts.clear();
        state.selected = state.todo.empty() ? std::string {} : state.todo.front().sha;
        state.list_scroll = 0.0f;
        state.side_scroll = 0.0f;
        sync_rebase_drafts(state);
    }

    void set_rebase_autosquash(rebase_editor_state& state, const bool enabled)
    {
        state.autosquash = enabled;
        state.todo = enabled ? autosquash_rebase_todo(state.plan.commits) : default_rebase_todo(state.plan.commits);
        sync_rebase_drafts(state);
    }

    std::optional<std::size_t> rebase_row_index(const rebase_editor_state& state, const std::string_view sha)
    {
        for (std::size_t index { 0 }; index < state.todo.size(); ++index)
            if (state.todo[index].sha == sha)
                return index;
        return std::nullopt;
    }

    bool set_rebase_action(rebase_editor_state& state, const std::string_view sha, const rebase_action action)
    {
        const std::optional<std::size_t> index { rebase_row_index(state, sha) };
        if (index.has_value() == false)
            return false;
        if (is_meld_action(action))
        {
            // 앞에 남는 커밋(drop이 아닌 줄)이 있어야 합칠 수 있다.
            const auto kept = [](const rebase_todo_entry& entry) { return entry.action != rebase_action::drop; };
            const bool has_previous { std::any_of(state.todo.begin(), state.todo.begin() + static_cast<std::ptrdiff_t>(*index), kept) };
            if (has_previous == false)
                return false;
        }
        state.todo[*index].action = action;
        sync_rebase_drafts(state);
        return true;
    }

    bool move_rebase_row(rebase_editor_state& state, const std::string_view sha, const int delta)
    {
        const std::optional<std::size_t> index { rebase_row_index(state, sha) };
        if (index.has_value() == false || delta == 0)
            return false;
        const std::ptrdiff_t target { static_cast<std::ptrdiff_t>(*index) + delta };
        if (target < 0 || target >= static_cast<std::ptrdiff_t>(state.todo.size()))
            return false;
        rebase_todo_entry moved { std::move(state.todo[*index]) };
        state.todo.erase(state.todo.begin() + static_cast<std::ptrdiff_t>(*index));
        state.todo.insert(state.todo.begin() + target, std::move(moved));
        sync_rebase_drafts(state);
        return true;
    }

    bool reorder_rebase_rows(rebase_editor_state& state, const std::string_view moved, const std::string_view target)
    {
        const std::optional<std::size_t> from { rebase_row_index(state, moved) };
        const std::optional<std::size_t> to { rebase_row_index(state, target) };
        if (from.has_value() == false || to.has_value() == false || *from == *to)
            return false;
        return move_rebase_row(state, moved, static_cast<int>(static_cast<std::ptrdiff_t>(*to) - static_cast<std::ptrdiff_t>(*from)));
    }

    std::optional<std::string> rebase_draft_key(const rebase_editor_state& state, const std::string_view sha)
    {
        const std::optional<std::size_t> index { rebase_row_index(state, sha) };
        if (index.has_value() == false)
            return std::nullopt;
        // squash 묶음은 묶음의 합친 메시지 하나로 끝난다 (git이 묶음 끝에서 편집기를 연다). 첫 줄이 reword여도 마지막에
        // 묶음 메시지로 덮이므로 묶음 초안이 먼저다.
        if (const std::optional<std::string> head { squash_group_head(state, *index) }; head.has_value())
            return "g:" + *head;
        if (state.todo[*index].action == rebase_action::reword)
            return "r:" + std::string { sha };
        return std::nullopt;
    }

    void sync_rebase_drafts(rebase_editor_state& state)
    {
        std::vector<std::string> messages(state.todo.size());
        for (std::size_t index { 0 }; index < state.todo.size(); ++index)
        {
            const rebase_todo_entry& entry { state.todo[index] };
            if (entry.action == rebase_action::reword)
            {
                const std::string key { "r:" + entry.sha };
                if (state.drafts.contains(key) == false)
                    state.drafts.emplace(key, make_rebase_draft(original_message(state, entry.sha)));
                messages[index] = rebase_draft_message(state.drafts.at(key));
            }
            else
                messages[index] = original_message(state, entry.sha);
        }
        for (const rebase_group& group : rebase_groups(state.todo))
        {
            if (group.has_squash == false)
                continue;
            const std::string key { "g:" + state.todo[group.head].sha };
            const std::string combined { combined_squash_message(state.todo, group, messages) };
            const auto found { state.drafts.find(key) };
            if (found == state.drafts.end())
                state.drafts.emplace(key, make_rebase_draft(combined));
            else if (found->second.edited == false)
                found->second = make_rebase_draft(combined);
        }
    }

    void reset_rebase_draft(rebase_editor_state& state, const std::string& key)
    {
        state.drafts.erase(key);
        sync_rebase_drafts(state);
    }

    std::string rebase_draft_message(const rebase_message_draft& draft)
    {
        std::string message { to_text(draft.subject.text) };
        const std::string body { to_text(draft.body.text) };
        const std::size_t begin { body.find_first_not_of("\r\n") };
        if (begin != std::string::npos)
        {
            message.append("\n\n");
            message.append(body.substr(begin));
        }
        return message;
    }

    rebase_message_draft make_rebase_draft(const std::string_view message)
    {
        const std::string normalized { normalize_commit_message(message) };
        const std::size_t line_end { normalized.find('\n') };
        std::string subject { normalized.substr(0, line_end) };
        std::string body {};
        if (line_end != std::string::npos)
        {
            const std::size_t begin { normalized.find_first_not_of('\n', line_end) };
            if (begin != std::string::npos)
                body = normalized.substr(begin);
        }
        while (body.empty() == false && body.back() == '\n')
            body.pop_back();
        rebase_message_draft draft {};
        draft.subject = edit_state_of(std::move(subject));
        draft.body = edit_state_of(std::move(body));
        return draft;
    }

    std::vector<rebase_todo_entry> rebase_final_todo(const rebase_editor_state& state)
    {
        std::vector<rebase_todo_entry> todo { state.todo };
        for (rebase_todo_entry& entry : todo)
        {
            entry.message.reset();
            if (entry.action != rebase_action::reword)
                continue;
            const auto found { state.drafts.find("r:" + entry.sha) };
            entry.message = found != state.drafts.end() ? rebase_draft_message(found->second) : original_message(state, entry.sha);
        }
        for (const rebase_group& group : rebase_groups(todo))
        {
            if (group.has_squash == false)
                continue;
            const auto found { state.drafts.find("g:" + todo[group.head].sha) };
            if (found != state.drafts.end())
                todo[group.last].message = rebase_draft_message(found->second);
        }
        return todo;
    }

    void move_text_caret_line(luil::text::text_edit_state& state, const bool down, const bool extend)
    {
        const std::u8string_view text { state.text };
        const std::size_t caret { std::min(state.caret, text.size()) };
        const std::size_t line_begin { caret == 0 ? 0 : (text.rfind(u8'\n', caret - 1) == std::u8string_view::npos ? 0 : text.rfind(u8'\n', caret - 1) + 1) };
        const std::size_t column { code_points(text.substr(line_begin, caret - line_begin)) };
        if (down)
        {
            const std::size_t line_end { text.find(u8'\n', caret) };
            if (line_end == std::u8string_view::npos)
            {
                place_caret(state, text.size(), extend);
                return;
            }
            const std::size_t next_begin { line_end + 1 };
            const std::size_t next_end { std::min(text.find(u8'\n', next_begin), text.size()) };
            place_caret(state, next_begin + offset_at_column(text.substr(next_begin, next_end - next_begin), column), extend);
            return;
        }
        if (line_begin == 0)
        {
            place_caret(state, 0, extend);
            return;
        }
        const std::size_t previous_end { line_begin - 1 };
        const std::size_t previous_begin { previous_end == 0 ? 0 : (text.rfind(u8'\n', previous_end - 1) == std::u8string_view::npos ? 0 : text.rfind(u8'\n', previous_end - 1) + 1) };
        place_caret(state, previous_begin + offset_at_column(text.substr(previous_begin, previous_end - previous_begin), column), extend);
    }

    void move_text_caret_line_edge(luil::text::text_edit_state& state, const bool end, const bool extend)
    {
        const std::u8string_view text { state.text };
        const std::size_t caret { std::min(state.caret, text.size()) };
        if (end)
        {
            place_caret(state, std::min(text.find(u8'\n', caret), text.size()), extend);
            return;
        }
        const std::size_t previous { caret == 0 ? std::u8string_view::npos : text.rfind(u8'\n', caret - 1) };
        place_caret(state, previous == std::u8string_view::npos ? 0 : previous + 1, extend);
    }
} // namespace cyella
