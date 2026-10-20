#include "presentation/rebase/rebase_editor_controller.h"

#include "presentation/rebase/rebase_editor_model.h"

#include <algorithm>
#include <utility>

namespace cyella {
    namespace {
        // 실행 전 확인에서 거절된 실패다. 저장소는 그대로라 편집기를 열어 두고 고쳐 다시 내게 한다.
        bool rejected_before_start(const operation_status& status)
        {
            if (status.state != operation_state::failed || status.failure.has_value() == false)
                return false;
            return status.failure->kind == failure_kind::invalid_request || status.failure->kind == failure_kind::refused;
        }

        // 여러 줄 본문에 넣는 글이다. CRLF·CR은 LF로 바꾸고 탭은 그대로 둔다.
        std::u8string multiline_text(const std::u8string_view text)
        {
            std::u8string result {};
            result.reserve(text.size());
            for (std::size_t index { 0 }; index < text.size(); ++index)
            {
                if (text[index] == u8'\r')
                {
                    result.push_back(u8'\n');
                    if (index + 1 < text.size() && text[index + 1] == u8'\n')
                        ++index;
                    continue;
                }
                result.push_back(text[index]);
            }
            return result;
        }

        // 편집기가 받는 메시지인가 (열기 메시지는 따로 본다).
        bool is_editor_message(const luil::app_message& message)
        {
            if (message.get<close_rebase_editor_intent>() != nullptr || message.get<rebase_select_intent>() != nullptr || message.get<rebase_set_action_intent>() != nullptr)
                return true;
            if (message.get<rebase_move_intent>() != nullptr || message.get<rebase_reorder_intent>() != nullptr || message.get<rebase_toggle_intent>() != nullptr)
                return true;
            if (message.get<rebase_reset_message_intent>() != nullptr || message.get<rebase_body_line_intent>() != nullptr || message.get<rebase_scroll_intent>() != nullptr)
                return true;
            return message.get<rebase_start_intent>() != nullptr;
        }

        std::u8string single_line_text(const std::u8string_view text)
        {
            std::u8string result {};
            result.reserve(text.size());
            for (const char8_t value : text)
                result.push_back(value == u8'\r' || value == u8'\n' || value == u8'\t' ? u8' ' : value);
            return result;
        }
    } // namespace

    rebase_editor_controller::rebase_editor_controller(dependencies dependencies)
        : dependencies_ { dependencies }
    {}

    void rebase_editor_controller::open(const open_rebase_editor_intent& intent)
    {
        rebase_editor_state state {};
        state.repository_id = intent.repository_id;
        state.onto = intent.onto;
        state.branch = intent.branch;
        state.onto_label = intent.onto_label;
        state.branch_label = intent.branch_label;
        state.worktree_dirty = intent.worktree_dirty;
        state.autostash = intent.worktree_dirty;
        state.phase = rebase_editor_phase::loading;
        if (dependencies_.workspace != nullptr)
            state.plan_operation = dependencies_.workspace->submit(intent.repository_id, load_rebase_plan_request { intent.onto, intent.branch });
        if (state.plan_operation == 0)
        {
            state.phase = rebase_editor_phase::load_failed;
            state.error = u8"저장소가 열려 있지 않아 범위를 읽지 못했습니다.";
        }
        state_ = std::move(state);
    }

    void rebase_editor_controller::close()
    {
        state_.reset();
    }

    std::string rebase_editor_controller::sha_of(const std::u8string& row_key) const
    {
        if (state_.has_value() == false)
            return {};
        if (row_key.empty())
            return state_->selected;
        return rebase_row_sha(row_key).value_or(std::string {});
    }

    bool rebase_editor_controller::handle(const luil::app_message& message)
    {
        if (const auto* const intent { message.get<open_rebase_editor_intent>() }; intent != nullptr)
        {
            open(*intent);
            return true;
        }
        if (is_editor_message(message) == false)
            return false;
        if (state_.has_value() == false)
            return true;
        rebase_editor_state& state { *state_ };

        if (message.get<close_rebase_editor_intent>() != nullptr)
        {
            // 실행 중이면 닫아도 rebase는 계속된다. 결과는 상태 줄과 충돌 화면이 보인다.
            close();
            return true;
        }
        if (const auto* const scroll { message.get<rebase_scroll_intent>() }; scroll != nullptr)
        {
            float& value { scroll->side ? state.side_scroll : state.list_scroll };
            value = std::max(0.0f, scroll->absolute ? scroll->value : value + scroll->value);
            return true;
        }
        // 편집은 목록이 있을 때만이다. 실행 중에는 목록을 바꾸지 않는다.
        if (state.phase != rebase_editor_phase::editing)
            return true;

        if (const auto* const select { message.get<rebase_select_intent>() }; select != nullptr)
        {
            if (const std::string sha { sha_of(select->row_key) }; rebase_row_index(state, sha).has_value())
                state.selected = sha;
        }
        else if (const auto* const action { message.get<rebase_set_action_intent>() }; action != nullptr)
        {
            const std::string sha { sha_of(action->row_key) };
            if (set_rebase_action(state, sha, action->action))
                state.selected = sha;
            state.error.clear();
        }
        else if (const auto* const move { message.get<rebase_move_intent>() }; move != nullptr)
        {
            const std::string sha { sha_of(move->row_key) };
            if (move_rebase_row(state, sha, move->delta))
                state.selected = sha;
            state.error.clear();
        }
        else if (const auto* const reorder { message.get<rebase_reorder_intent>() }; reorder != nullptr)
        {
            const std::string moved { sha_of(reorder->moved) };
            if (reorder_rebase_rows(state, moved, sha_of(reorder->target)))
                state.selected = moved;
            state.error.clear();
        }
        else if (const auto* const toggle { message.get<rebase_toggle_intent>() }; toggle != nullptr)
        {
            switch (toggle->option)
            {
            case rebase_option::autosquash:
                set_rebase_autosquash(state, state.autosquash == false);
                break;
            case rebase_option::autostash:
                state.autostash = state.autostash == false;
                break;
            case rebase_option::flatten_merges:
                state.flatten_confirmed = state.flatten_confirmed == false;
                break;
            }
            state.error.clear();
        }
        else if (message.get<rebase_reset_message_intent>() != nullptr)
        {
            if (const std::optional<std::string> key { rebase_draft_key(state, state.selected) }; key.has_value())
                reset_rebase_draft(state, *key);
        }
        else if (const auto* const line { message.get<rebase_body_line_intent>() }; line != nullptr)
        {
            if (rebase_message_draft* const draft { selected_draft() }; draft != nullptr)
                move_text_caret_line(draft->body, line->down, line->extend);
        }
        else if (message.get<rebase_start_intent>() != nullptr)
            start();
        return true;
    }

    rebase_message_draft* rebase_editor_controller::selected_draft()
    {
        if (state_.has_value() == false || state_->phase != rebase_editor_phase::editing)
            return nullptr;
        const std::optional<std::string> key { rebase_draft_key(*state_, state_->selected) };
        if (key.has_value() == false)
            return nullptr;
        const auto found { state_->drafts.find(*key) };
        return found == state_->drafts.end() ? nullptr : &found->second;
    }

    luil::text::text_edit_state* rebase_editor_controller::draft(const text_target target)
    {
        rebase_message_draft* const draft { selected_draft() };
        if (draft == nullptr)
            return nullptr;
        if (target == text_target::rebase_message_subject)
            return &draft->subject;
        if (target == text_target::rebase_message_body)
            return &draft->body;
        return nullptr;
    }

    bool rebase_editor_controller::edit_text(const luil::text_edit_request& request)
    {
        const text_target target { from_luil_target(request.target) };
        if (target != text_target::rebase_message_subject && target != text_target::rebase_message_body)
            return false;
        rebase_message_draft* const draft { selected_draft() };
        if (draft == nullptr)
            return true;
        const bool body { target == text_target::rebase_message_body };
        luil::text::text_edit_state& state { body ? draft->body : draft->subject };
        const std::u8string before { state.text };
        if (body && (request.command == luil::text::text_edit_command::move_line_start || request.command == luil::text::text_edit_command::move_line_end))
            move_text_caret_line_edge(state, request.command == luil::text::text_edit_command::move_line_end, request.extend);
        else
            luil::apply_text_edit(state, request, [body](const std::u8string_view text) { return body ? multiline_text(text) : single_line_text(text); });
        if (state.text != before)
        {
            draft->edited = true;
            // reword 메시지가 바뀌면 그 커밋이 든 squash 묶음의 (고치지 않은) 합친 메시지도 따라 바뀐다.
            sync_rebase_drafts(*state_);
        }
        return true;
    }

    void rebase_editor_controller::start()
    {
        rebase_editor_state& state { *state_ };
        const rebase_editor_view view { build_rebase_editor_view(state, std::nullopt) };
        if (view.can_start == false || dependencies_.workspace == nullptr)
            return;
        interactive_rebase_request request {};
        request.onto = state.onto;
        request.branch = state.branch;
        request.todo = rebase_final_todo(state);
        request.autostash = state.worktree_dirty && state.autostash;
        request.allow_flatten_merges = state.flatten_confirmed;
        state.run_operation = dependencies_.workspace->submit(state.repository_id, std::move(request));
        if (state.run_operation == 0)
        {
            state.error = u8"저장소가 열려 있지 않아 시작하지 못했습니다.";
            return;
        }
        state.error.clear();
        state.phase = rebase_editor_phase::running;
    }

    void rebase_editor_controller::after_events()
    {
        if (state_.has_value() == false || dependencies_.workspace == nullptr)
            return;
        rebase_editor_state& state { *state_ };
        git_workspace& workspace { *dependencies_.workspace };
        if (state.phase == rebase_editor_phase::loading)
        {
            const operation_status* const status { workspace.find_operation(state.plan_operation) };
            if (status == nullptr || status->finished() == false)
                return;
            const rebase_plan* const plan { workspace.rebase_plan_of(state.plan_operation) };
            if (status->state == operation_state::succeeded && plan != nullptr)
            {
                start_rebase_editing(state, *plan);
                return;
            }
            state.phase = rebase_editor_phase::load_failed;
            state.error = status->failure.has_value() ? status->failure->summary : std::u8string { u8"rebase할 커밋을 읽지 못했습니다." };
            return;
        }
        if (state.phase != rebase_editor_phase::running)
            return;
        const operation_status* const status { workspace.find_operation(state.run_operation) };
        if (status != nullptr && status->finished() == false)
            return;
        if (status != nullptr && rejected_before_start(*status))
        {
            // 저장소를 건드리기 전에 거절됐다. 고쳐서 다시 낼 수 있게 열어 둔다.
            state.phase = rebase_editor_phase::editing;
            state.error = status->failure->summary;
            state.run_operation = 0;
            return;
        }
        // 성공, 충돌로 멈춤(충돌 화면이 이어 받는다), 그 밖의 실패(배너가 보인다)는 모두 닫는다.
        close();
    }

    std::optional<rebase_editor_view> rebase_editor_controller::make_view(const std::optional<luil::text_composition_event>& composition) const
    {
        if (state_.has_value() == false)
            return std::nullopt;
        return build_rebase_editor_view(*state_, composition);
    }
} // namespace cyella
