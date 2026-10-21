#include "presentation/rebase/rebase_editor_view.h"

#include "domain/text_catalog.h"
#include "presentation/rebase/rebase_editor_model.h"
#include "presentation/ui_state.h"

#include <algorithm>
#include <string>

namespace cyella {
    namespace {
        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::u8string number(const std::size_t value)
        {
            return to_u8(std::to_string(value));
        }

        std::u8string short_of(const std::string_view sha)
        {
            return to_u8(sha.substr(0, std::min<std::size_t>(sha.size(), 7)));
        }

        const rebase_commit* find_commit(const rebase_plan& plan, const std::string_view sha)
        {
            for (const rebase_commit& commit : plan.commits)
                if (commit.sha == sha)
                    return &commit;
            return nullptr;
        }

        std::u8string phase_status(const rebase_editor_state& state)
        {
            switch (state.phase)
            {
            case rebase_editor_phase::loading:
                return tr(u8"rebase_editor.loading");
            case rebase_editor_phase::running:
                return tr(u8"rebase_editor.running");
            case rebase_editor_phase::load_failed:
                return state.error.empty() ? tr(u8"rebase_editor.load_failed") : state.error;
            case rebase_editor_phase::editing:
                break;
            }
            return {};
        }

        std::u8string action_caption(const rebase_action action)
        {
            switch (action)
            {
            case rebase_action::pick:
                return tr(u8"rebase_editor.action_note.pick");
            case rebase_action::reword:
                return tr(u8"rebase_editor.action_note.reword");
            case rebase_action::squash:
                return tr(u8"rebase_editor.action_note.squash");
            case rebase_action::fixup:
                return tr(u8"rebase_editor.action_note.fixup");
            case rebase_action::fixup_keep_message:
                return tr(u8"rebase_editor.action_note.fixup_keep_message");
            case rebase_action::drop:
                return tr(u8"rebase_editor.action_note.drop");
            }
            return {};
        }
    } // namespace

    rebase_editor_view build_rebase_editor_view(const rebase_editor_state& state, const std::optional<luil::text_composition_event>& composition)
    {
        rebase_editor_view view {};
        view.phase = state.phase;
        view.status_text = phase_status(state);
        view.list_scroll = state.list_scroll;
        view.side_scroll = state.side_scroll;
        const std::u8string branch { state.branch_label.empty() ? std::u8string { u8"HEAD" } : state.branch_label };
        view.title = tr(u8"rebase_editor.title", { { u8"branch", branch }, { u8"onto", state.onto_label } });
        if (state.phase == rebase_editor_phase::loading || state.phase == rebase_editor_phase::load_failed)
            return view;

        const rebase_plan& plan { state.plan };
        const std::size_t commit_count { plan.commits.size() };
        if (plan.onto_sha.empty())
            view.subtitle = commit_count == 1 ? tr(u8"rebase_editor.subtitle_one", { { u8"branch", branch }, { u8"count", commit_count }, { u8"onto", state.onto_label } })
                                              : tr(u8"rebase_editor.subtitle", { { u8"branch", branch }, { u8"count", commit_count }, { u8"onto", state.onto_label } });
        else
        {
            const std::u8string onto_sha { short_of(plan.onto_sha) };
            view.subtitle = commit_count == 1
                ? tr(u8"rebase_editor.subtitle_with_sha_one", { { u8"branch", branch }, { u8"count", commit_count }, { u8"onto", state.onto_label }, { u8"sha", onto_sha } })
                : tr(u8"rebase_editor.subtitle_with_sha", { { u8"branch", branch }, { u8"count", commit_count }, { u8"onto", state.onto_label }, { u8"sha", onto_sha } });
        }

        // ---- 목록
        const std::vector<rebase_group> groups { rebase_groups(state.todo) };
        std::vector<bool> melded(state.todo.size(), false);
        for (const rebase_group& group : groups)
            for (const std::size_t member : group.members)
                if (member != group.head)
                    melded[member] = true;
        std::size_t pushed_count { 0 };
        for (std::size_t index { 0 }; index < state.todo.size(); ++index)
        {
            const rebase_todo_entry& entry { state.todo[index] };
            const rebase_commit* const commit { find_commit(plan, entry.sha) };
            rebase_row_view row {};
            row.key = rebase_row_key(entry.sha);
            row.sha = entry.sha;
            row.number = index + 1;
            row.action = entry.action;
            row.short_sha = short_of(entry.sha);
            row.subject = commit != nullptr ? to_u8(commit->subject) : std::u8string {};
            row.pushed = commit != nullptr && commit->pushed;
            row.marker = commit != nullptr ? autosquash_marker_of(commit->subject) : autosquash_marker::none;
            row.melded = melded[index];
            if (row.pushed)
                ++pushed_count;
            // 줄 이름은 숫자로 시작한다. 목록의 글자 탐색이 p·r·s·f·d 단축키를 줄 찾기로 가져가지 않는다.
            row.label = number(row.number) + u8"  " + std::u8string { rebase_action_label(entry.action) } + u8"  " + row.short_sha + u8"  " + row.subject;
            if (row.pushed)
                row.label += u8"  · " + tr(u8"rebase_editor.pushed_badge");
            view.rows.push_back(std::move(row));
        }

        // ---- 고른 줄
        if (const std::optional<std::size_t> index { rebase_row_index(state, state.selected) }; index.has_value())
        {
            const rebase_todo_entry& entry { state.todo[*index] };
            const rebase_commit* const commit { find_commit(plan, entry.sha) };
            view.has_selection = true;
            view.selected_key = rebase_row_key(entry.sha);
            view.selected_title = short_of(entry.sha) + u8"  " + (commit != nullptr ? to_u8(commit->subject) : std::u8string {});
            view.selected_action = entry.action;
            view.can_move_up = *index > 0;
            view.can_move_down = *index + 1 < state.todo.size();
            view.can_meld
                = std::any_of(state.todo.begin(), state.todo.begin() + static_cast<std::ptrdiff_t>(*index), [](const rebase_todo_entry& other) { return other.action != rebase_action::drop; });

            const std::optional<std::string> key { rebase_draft_key(state, entry.sha) };
            const auto draft { key.has_value() ? state.drafts.find(*key) : state.drafts.end() };
            if (draft != state.drafts.end())
            {
                view.message_editable = true;
                view.message_edited = draft->second.edited;
                view.subject = luil::make_text_input_view(draft->second.subject, composition, to_luil_target(text_target::rebase_message_subject));
                view.body = luil::make_text_input_view(draft->second.body, composition, to_luil_target(text_target::rebase_message_body));
                if (key->starts_with("g:"))
                {
                    std::size_t members { 0 };
                    for (const rebase_group& group : groups)
                        if (group.has_squash && state.todo[group.head].sha == key->substr(2))
                            members = group.members.size();
                    view.message_caption = tr(u8"rebase_editor.squash_message_caption", { { u8"count", members } });
                    view.message_note = tr(u8"rebase_editor.squash_message_note");
                }
                else
                {
                    view.message_caption = tr(u8"rebase_editor.reword_message_caption");
                    view.message_note = tr(u8"rebase_editor.reword_message_note");
                }
            }
            else
                view.message_note = action_caption(entry.action);
        }

        // ---- 결과 미리보기
        const std::vector<rebase_todo_entry> final_todo { rebase_final_todo(state) };
        const rebase_preview preview { preview_rebase(final_todo, plan.commits) };
        for (const rebase_result_commit& result : preview.commits)
        {
            rebase_preview_view row {};
            row.key = u8"rebase-result:" + number(result.head);
            row.subject = to_u8(result.subject);
            std::u8string detail {};
            if (result.combined > 1)
                detail = tr(u8"rebase_editor.preview_combined", { { u8"count", result.combined } });
            if (result.message_changed)
                detail += (detail.empty() ? u8"" : u8" · ") + tr(u8"rebase_editor.preview_message_changed");
            row.detail = std::move(detail);
            row.changed = result.rewritten;
            view.preview.push_back(std::move(row));
        }
        view.preview_caption = tr(u8"rebase_editor.preview_caption", { { u8"count", preview.commits.size() } });
        if (preview.dropped_count > 0)
            view.preview_caption += u8" · " + tr(u8"rebase_editor.preview_dropped", { { u8"count", preview.dropped_count } });
        if (preview.reordered)
            view.preview_caption += u8" · " + tr(u8"rebase_editor.preview_reordered");
        view.summary = tr(u8"rebase_editor.summary", { { u8"from", state.todo.size() }, { u8"to", preview.commits.size() } });

        // ---- 경고
        if (pushed_count > 0)
            view.warnings.push_back(
                pushed_count == 1 ? tr(u8"rebase_editor.pushed_warning_one", { { u8"count", pushed_count } }) : tr(u8"rebase_editor.pushed_warning", { { u8"count", pushed_count } }));
        if (plan.merge_count > 0)
        {
            view.warnings.push_back(
                plan.merge_count == 1 ? tr(u8"rebase_editor.merge_warning_one", { { u8"count", plan.merge_count } }) : tr(u8"rebase_editor.merge_warning", { { u8"count", plan.merge_count } }));
            view.needs_flatten_confirm = true;
            view.flatten_confirmed = state.flatten_confirmed;
        }
        view.show_autosquash = std::ranges::any_of(plan.commits, [](const rebase_commit& commit) { return autosquash_marker_of(commit.subject) != autosquash_marker::none; });
        view.autosquash = state.autosquash;
        view.show_autostash = state.worktree_dirty;
        view.autostash = state.autostash;

        // ---- 시작할 수 있는가
        std::vector<std::string> range {};
        range.reserve(plan.commits.size());
        for (const rebase_commit& commit : plan.commits)
            range.push_back(commit.sha);
        if (state.phase == rebase_editor_phase::running)
            view.problem.clear();
        else if (plan.commits.empty())
            view.problem = tr(u8"rebase_editor.no_commits", { { u8"onto", state.onto_label } });
        else if (const auto issue { find_rebase_todo_problem(final_todo, range) }; issue.has_value())
            view.problem = rebase_todo_problem_text(*issue);
        else if (view.needs_flatten_confirm && state.flatten_confirmed == false)
            view.problem = tr(u8"rebase_editor.confirm_flatten_problem");
        else if (state.worktree_dirty && state.autostash == false)
            view.problem = tr(u8"rebase_editor.dirty_worktree_problem");
        else if (state.error.empty() == false)
            view.problem = state.error;
        view.can_start = state.phase == rebase_editor_phase::editing && plan.commits.empty() == false && find_rebase_todo_problem(final_todo, range).has_value() == false
            && (view.needs_flatten_confirm == false || state.flatten_confirmed) && (state.worktree_dirty == false || state.autostash);
        return view;
    }
} // namespace cyella
