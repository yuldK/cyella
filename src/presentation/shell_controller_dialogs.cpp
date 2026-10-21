#include "application/app_messages.h"
#include "application/git_workspace.h"
#include "domain/text_catalog.h"
#include "infrastructure/ssh_tools.h"
#include "presentation/form_builders.h"
#include "presentation/form_extension.h"
#include "presentation/form_hosting.h"
#include "presentation/graph_geometry.h"
#include "presentation/menu_extension.h"
#include "presentation/shell_controller.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>
#include <variant>

namespace cyella {
    namespace {
        // 확인 상자에 파일을 늘어놓는 수다. 넘치면 "외 N개"다.
        constexpr std::size_t listed_path_limit { 6 };

        std::string to_text(const std::u8string_view text)
        {
            return std::string { text.begin(), text.end() };
        }

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::u8string short_sha(const std::string& sha)
        {
            return to_u8(std::string_view { sha }.substr(0, std::min<std::size_t>(sha.size(), 7)));
        }

        const ref* find_ref(const repository_snapshot& snapshot, const ref_kind kind, const std::string& name)
        {
            for (const ref& value : snapshot.refs)
                if (value.kind == kind && value.name == name)
                    return &value;
            return nullptr;
        }

        // 체크아웃 요청에 정책을 넣어 다시 만든다. 체크아웃이 아니면 빈 값이다.
        std::optional<git_operation> with_policy(const git_operation& request, const checkout_policy policy)
        {
            if (const auto* const branch { std::get_if<checkout_branch_request>(&request) }; branch != nullptr)
            {
                checkout_branch_request retry { *branch };
                retry.policy = policy;
                return retry;
            }
            if (const auto* const remote { std::get_if<checkout_remote_branch_request>(&request) }; remote != nullptr)
            {
                checkout_remote_branch_request retry { *remote };
                retry.policy = policy;
                return retry;
            }
            if (const auto* const detached { std::get_if<checkout_detached_request>(&request) }; detached != nullptr)
            {
                checkout_detached_request retry { *detached };
                retry.policy = policy;
                return retry;
            }
            return std::nullopt;
        }

        // 네트워크를 쓰는 요청에 "이번만 연결"을 넣어 다시 만든다. 받지 않는 요청이면 빈 값이다.
        std::optional<git_operation> with_ssh_override(const git_operation& request, const temporary_ssh_trust& trust)
        {
            if (const auto* const fetch { std::get_if<fetch_request>(&request) }; fetch != nullptr)
            {
                fetch_request retry { *fetch };
                retry.ssh_command_override = trust;
                return retry;
            }
            if (const auto* const pull { std::get_if<pull_request>(&request) }; pull != nullptr)
            {
                pull_request retry { *pull };
                retry.ssh_command_override = trust;
                return retry;
            }
            if (const auto* const push { std::get_if<push_execute_request>(&request) }; push != nullptr)
            {
                push_execute_request retry { *push };
                retry.ssh_command_override = trust;
                return retry;
            }
            if (const auto* const clone { std::get_if<clone_request>(&request) }; clone != nullptr)
            {
                clone_request retry { *clone };
                retry.ssh_command_override = trust;
                return retry;
            }
            if (const auto* const tags { std::get_if<refresh_remote_tags_request>(&request) }; tags != nullptr)
            {
                refresh_remote_tags_request retry { *tags };
                retry.ssh_command_override = trust;
                return retry;
            }
            return std::nullopt;
        }

        // 끝난 뒤 원격 태그 표시를 다시 읽는 작업인가 (workspace의 자동 후속과 같은 기준이다).
        bool refreshes_remote_tags(const git_operation& request)
        {
            if (std::holds_alternative<fetch_request>(request) || std::holds_alternative<push_tag_request>(request) || std::holds_alternative<push_tags_request>(request)
                || std::holds_alternative<delete_remote_tag_request>(request))
                return true;
            const auto* const push { std::get_if<push_execute_request>(&request) };
            return push != nullptr && push->follow_tags;
        }

        std::optional<operation_kind> sequence_kind(const repository_operation_state state, const conflict_action action)
        {
            switch (state)
            {
            case repository_operation_state::rebase:
                return action == conflict_action::continue_sequence ? operation_kind::rebase_continue
                    : action == conflict_action::skip_sequence      ? operation_kind::rebase_skip
                                                                    : operation_kind::rebase_abort;
            case repository_operation_state::merge:
                if (action == conflict_action::skip_sequence)
                    return std::nullopt;
                return action == conflict_action::continue_sequence ? operation_kind::merge_continue : operation_kind::merge_abort;
            case repository_operation_state::cherry_pick:
                return action == conflict_action::continue_sequence ? operation_kind::cherry_pick_continue
                    : action == conflict_action::skip_sequence      ? operation_kind::cherry_pick_skip
                                                                    : operation_kind::cherry_pick_abort;
            case repository_operation_state::revert:
                return action == conflict_action::continue_sequence ? operation_kind::revert_continue
                    : action == conflict_action::skip_sequence      ? operation_kind::revert_skip
                                                                    : operation_kind::revert_abort;
            case repository_operation_state::none:
                break;
            }
            return std::nullopt;
        }

        void list_paths(const std::vector<std::string>& paths, std::vector<std::u8string>& lines)
        {
            for (std::size_t index { 0 }; index < std::min(paths.size(), listed_path_limit); ++index)
                lines.push_back(to_u8(paths[index]));
            if (paths.size() > listed_path_limit)
                lines.push_back(tr(u8"shell.confirm.more_paths", { { u8"count", paths.size() - listed_path_limit } }));
        }

        bool awaiting_auth_status(const operation_status& status) noexcept
        {
            if (status.state != operation_state::needs_input || status.finished() || status.needs_input.has_value() == false)
                return false;
            const input_kind kind { status.needs_input->kind };
            return kind == input_kind::passphrase || kind == input_kind::host_key || kind == input_kind::credentials;
        }
    } // namespace

    std::optional<std::u8string> shell_controller::take_clipboard_text()
    {
        std::optional<std::u8string> text {};
        text.swap(clipboard_text_);
        return text;
    }

    void shell_controller::copy_text(std::u8string text)
    {
        clipboard_text_ = std::move(text);
    }

    std::shared_ptr<const graph_index> shell_controller::history_index(const repository_session& session)
    {
        history_cache_entry& entry { history_cache_[session.id()] };
        const std::size_t size { session.graph_commits().size() };
        if (entry.index != nullptr && entry.generation == session.displayed_generation() && entry.size == size)
            return entry.index;
        entry.index = std::make_shared<const graph_index>(session.graph_commits());
        entry.generation = session.displayed_generation();
        entry.size = size;
        return entry.index;
    }

    void shell_controller::collect_operations(std::vector<const operation_status*>& repository, std::vector<const operation_status*>& setup)
    {
        if (dependencies_.workspace == nullptr)
            return;
        const std::string selected { selected_repository_id() };
        for (const operation_id id : dependencies_.workspace->operation_order())
        {
            const operation_status* const status { dependencies_.workspace->find_operation(id) };
            if (status == nullptr)
                continue;
            if (status->repository_id.empty())
                setup.push_back(status);
            else if (status->repository_id == selected)
                repository.push_back(status);
        }
    }

    form_source shell_controller::current_form_source()
    {
        std::vector<const operation_status*> repository {};
        std::vector<const operation_status*> setup {};
        collect_operations(repository, setup);
        return resolve_form_source(repository, setup, ui_, selected_tab_state());
    }

    void shell_controller::clear_form_inputs()
    {
        for (luil::text::text_edit_state& field : ui_.form.fields)
        {
            wipe_secret_text(field.text);
            field = {};
        }
        ui_.form.reveal = false;
        ui_.form.choice = 0;
        ui_.form.checks = {};
        ui_.form.scroll = 0.0f;
        feedback_.form_maximum.reset();
    }

    void shell_controller::sync_form_inputs()
    {
        const std::string identity { form_identity(current_form_source(), ui_) };
        if (identity == ui_.form.identity)
            return;
        clear_form_inputs();
        ui_.form.identity = identity;
    }

    void shell_controller::set_form_field(const std::size_t index, std::u8string text)
    {
        if (index >= ui_.form.fields.size())
            return;
        luil::text::text_edit_state& field { ui_.form.fields[index] };
        field = {};
        field.caret = text.size();
        field.anchor = text.size();
        field.text = std::move(text);
    }

    operation_id shell_controller::submit_to(const std::string& repository_id, git_operation operation)
    {
        if (dependencies_.workspace == nullptr || repository_id.empty())
            return 0;
        return dependencies_.workspace->submit(repository_id, std::move(operation));
    }

    operation_id shell_controller::submit_sequence(std::vector<git_operation> operations)
    {
        if (operations.empty())
            return 0;
        const std::string repository { selected_repository_id() };
        git_operation first { std::move(operations.front()) };
        operations.erase(operations.begin());
        const operation_id id { submit_to(repository, std::move(first)) };
        if (id != 0 && operations.empty() == false)
            chains_[id] = chain_entry { repository, std::move(operations) };
        return id;
    }

    void shell_controller::process_chains()
    {
        git_workspace* const workspace { dependencies_.workspace };
        if (workspace == nullptr)
            return;
        std::vector<std::pair<operation_id, chain_entry>> ready {};
        for (auto it { chains_.begin() }; it != chains_.end();)
        {
            const operation_status* const status { workspace->find_operation(it->first) };
            if (status != nullptr && status->finished() == false)
            {
                ++it;
                continue;
            }
            // 앞이 성공해야 이어 낸다. 실패·취소·충돌이면 남은 것은 버린다 (원격 태그 삭제가 실패하면 로컬 태그는 남긴다).
            if (status != nullptr && status->state == operation_state::succeeded)
                ready.emplace_back(it->first, std::move(it->second));
            it = chains_.erase(it);
        }
        for (auto& [id, entry] : ready)
        {
            static_cast<void>(id);
            git_operation next { std::move(entry.rest.front()) };
            entry.rest.erase(entry.rest.begin());
            const operation_id next_id { submit_to(entry.repository_id, std::move(next)) };
            if (next_id != 0 && entry.rest.empty() == false)
                chains_[next_id] = std::move(entry);
        }
    }

    void shell_controller::cancel_automatic_prompts()
    {
        git_workspace* const workspace { dependencies_.workspace };
        if (workspace == nullptr)
            return;
        // 자동 후속 작업(쓰기 뒤 원격 태그 갱신)이 인증을 물으면 사용자는 영문을 모른다. 조용히 취소한다 — 원격 태그 표시는
        // 이번 갱신을 건너뛰고 다음 fetch가 고친다. "이번만 연결"로 다시 낸 작업은 새 임시 신뢰로 직접 갱신한다 (`after_events`).
        for (const operation_id id : std::vector<operation_id> { workspace->operation_order().begin(), workspace->operation_order().end() })
            if (const operation_status* status { workspace->find_operation(id) }; status != nullptr && status->automatic && awaiting_auth_status(*status))
                static_cast<void>(workspace->cancel(id));
    }

    // ---------------------------------------------------------------- 메시지

    bool shell_controller::handle_dialog_message(const luil::app_message& message)
    {
        if (const auto* const intent { message.get<open_ref_list_intent>() }; intent != nullptr)
        {
            const repository_session* const session { selected_session() };
            if (session != nullptr && ui_.menu.has_value() == false && ui_.dialog.has_value() == false)
            {
                const auto& commits { session->graph_commits() };
                if (std::any_of(commits.begin(), commits.end(), [intent](const commit_info& commit) { return commit.sha == intent->sha; }))
                {
                    ui_.selector.reset();
                    ui_.ref_list = ref_list_state { intent->sha, intent->x, intent->y };
                }
            }
            return true;
        }
        if (message.get<close_ref_list_intent>() != nullptr)
        {
            ui_.ref_list.reset();
            return true;
        }
        if (const auto* const intent { message.get<scroll_ref_list_intent>() }; intent != nullptr)
        {
            if (ui_.ref_list.has_value())
                ui_.ref_list->scroll = std::max(intent->absolute ? intent->value : ui_.ref_list->scroll + intent->value, 0.0f);
            return true;
        }
        if (const auto* const intent { message.get<open_context_menu_intent>() }; intent != nullptr)
        {
            open_context_menu(intent->target, intent->x, intent->y);
            return true;
        }
        if (message.get<close_menu_intent>() != nullptr)
        {
            // 메뉴 항목 탐색의 Esc는 선택 상자 목록에서도 같은 메시지로 온다.
            ui_.menu.reset();
            ui_.selector.reset();
            ui_.ref_list.reset();
            return true;
        }
        if (const auto* const intent { message.get<menu_select_intent>() }; intent != nullptr)
        {
            handle_menu_select(intent->key);
            return true;
        }
        if (message.get<open_menu_for_selection_intent>() != nullptr)
        {
            open_menu_for_selection();
            return true;
        }
        if (const auto* const intent { message.get<drop_intent>() }; intent != nullptr)
        {
            handle_drop(*intent);
            return true;
        }
        if (const auto* const intent { message.get<activate_commit_intent>() }; intent != nullptr)
        {
            handle_activate_commit(intent->sha);
            return true;
        }
        if (const auto* const intent { message.get<form_button_intent>() }; intent != nullptr)
        {
            handle_form_button(intent->key);
            return true;
        }
        if (const auto* const intent { message.get<form_check_intent>() }; intent != nullptr)
        {
            handle_form_check(intent->index);
            return true;
        }
        if (const auto* const intent { message.get<form_choose_intent>() }; intent != nullptr)
        {
            handle_form_choose(intent->index);
            return true;
        }
        if (message.get<form_enter_intent>() != nullptr)
        {
            handle_form_enter();
            return true;
        }
        if (const auto* const intent { message.get<form_scroll_intent>() }; intent != nullptr)
        {
            if (ui_.form.identity.empty())
                return true;
            const float offset { intent->absolute ? intent->value : ui_.form.scroll + intent->value };
            ui_.form.scroll = std::max(0.0f, offset);
            if (feedback_.form_maximum.has_value())
                ui_.form.scroll = std::min(ui_.form.scroll, *feedback_.form_maximum);
            return true;
        }
        if (const auto* const intent { message.get<open_confirm_intent>() }; intent != nullptr)
        {
            dialog_state dialog {};
            dialog.title = intent->title;
            dialog.lines = intent->lines;
            dialog.confirm_label = intent->confirm_label;
            dialog.danger = intent->danger;
            dialog.plan.primary = intent->operations;
            open_confirm(std::move(dialog));
            return true;
        }
        if (const auto* const intent { message.get<discard_files_intent>() }; intent != nullptr)
        {
            if (intent->paths.empty())
                return true;
            if (intent->untracked)
            {
                dialog_state trash {};
                trash.title = intent->paths.size() == 1 ? tr(u8"shell.trash.title_one") : tr(u8"shell.trash.title", { { u8"count", intent->paths.size() } });
                trash.lines.push_back(tr(u8"shell.trash.description"));
                list_paths(intent->paths, trash.lines);
                trash.confirm_label = tr(u8"shell.trash.confirm");
                trash.danger = true;
                trash.plan.primary.push_back(trash_files_request { intent->paths });
                open_confirm(std::move(trash));
                return true;
            }
            dialog_state dialog {};
            dialog.title = intent->paths.size() == 1 ? tr(u8"shell.discard.title_one") : tr(u8"shell.discard.title", { { u8"count", intent->paths.size() } });
            dialog.lines.push_back(tr(u8"shell.discard.description"));
            list_paths(intent->paths, dialog.lines);
            dialog.confirm_label = tr(u8"shell.discard.confirm");
            dialog.danger = true;
            dialog.plan.primary.push_back(discard_files_request { intent->paths });
            open_confirm(std::move(dialog));
            return true;
        }
        if (const auto* const intent { message.get<open_branch_dialog_intent>() }; intent != nullptr)
        {
            open_create_branch_dialog(intent->at);
            return true;
        }
        if (const auto* const intent { message.get<banner_action_intent>() }; intent != nullptr)
        {
            handle_banner_action(*intent);
            return true;
        }
        if (const auto* const intent { message.get<conflict_action_intent>() }; intent != nullptr)
        {
            handle_conflict_action(intent->action);
            return true;
        }
        return false;
    }

    // ---------------------------------------------------------------- 대화 상자 열기

    std::u8string shell_controller::commit_label(const repository_session& session, const std::string& sha) const
    {
        std::u8string label { short_sha(sha) };
        for (const commit_info& commit : session.graph_commits())
            if (commit.sha == sha)
            {
                label += u8"  " + to_u8(commit.subject);
                break;
            }
        return label;
    }

    void shell_controller::open_create_branch_dialog(const std::string& at)
    {
        repository_session* const session { selected_session() };
        if (session == nullptr)
            return;
        const repository_summary& summary { session->snapshot().summary };
        dialog_state dialog {};
        dialog.kind = dialog_kind::create_branch;
        dialog.serial = ++ui_.dialog_serial;
        dialog.subject = at;
        if (at.empty())
            dialog.subject_label = summary.is_detached ? u8"HEAD (detached " + short_sha(summary.head_sha) + u8")" : u8"HEAD (" + to_u8(summary.current_branch) + u8")";
        else
            dialog.subject_label = commit_label(*session, at);
        dialog.flag_a = true;
        clear_form_inputs();
        ui_.form.identity = dialog_identity(dialog);
        ui_.dialog = std::move(dialog);
        ui_.menu.reset();
    }

    void shell_controller::open_create_tag_dialog(const std::string& sha)
    {
        repository_session* const session { selected_session() };
        if (session == nullptr)
            return;
        const repository_summary& summary { session->snapshot().summary };
        dialog_state dialog {};
        dialog.kind = dialog_kind::create_tag;
        dialog.serial = ++ui_.dialog_serial;
        dialog.subject = sha.empty() ? summary.head_sha : sha;
        dialog.subject_label = commit_label(*session, dialog.subject);
        // 서명 토글의 기본값은 설정 `tag.gpgsign`이다. 서명한 태그는 항상 annotated라 켜져 있으면 종류도 annotated다.
        dialog.flag_b = session->snapshot().signing.tag_signing();
        dialog.choice = dialog.flag_b ? 1 : 0;
        clear_form_inputs();
        ui_.form.identity = dialog_identity(dialog);
        ui_.dialog = std::move(dialog);
        ui_.menu.reset();
    }

    void shell_controller::open_push_dialog(const std::string& branch, const bool set_upstream)
    {
        repository_session* const session { selected_session() };
        if (session == nullptr)
            return;
        const repository_snapshot& snapshot { session->snapshot() };
        if (snapshot.summary.remotes.empty())
        {
            show_notice(tr(u8"shell.push.no_remote"), true);
            return;
        }
        dialog_state dialog {};
        dialog.kind = dialog_kind::push;
        dialog.serial = ++ui_.dialog_serial;
        dialog.subject = branch;
        for (const remote_info& remote : snapshot.summary.remotes)
            dialog.choices.push_back(remote.name);
        const ref* const value { find_ref(snapshot, ref_kind::local, branch) };
        const bool has_upstream { value != nullptr && value->upstream.empty() == false };
        std::string remote_branch { branch };
        std::string remote { "origin" };
        if (has_upstream)
        {
            const std::size_t slash { value->upstream.find('/') };
            if (slash != std::string::npos)
            {
                remote = value->upstream.substr(0, slash);
                remote_branch = value->upstream.substr(slash + 1);
            }
        }
        const auto found { std::find(dialog.choices.begin(), dialog.choices.end(), remote) };
        dialog.choice = found == dialog.choices.end() ? 0 : static_cast<std::size_t>(found - dialog.choices.begin());
        // upstream이 없으면 켜 둔다 (없는 채로 push하면 git이 upstream 설정을 요구한다).
        dialog.flag_a = set_upstream || has_upstream == false;
        clear_form_inputs();
        set_form_field(0, to_u8(remote_branch));
        ui_.form.identity = dialog_identity(dialog);
        ui_.dialog = std::move(dialog);
        ui_.menu.reset();
    }

    void shell_controller::open_set_upstream_dialog(const std::string& branch)
    {
        repository_session* const session { selected_session() };
        if (session == nullptr)
            return;
        const repository_snapshot& snapshot { session->snapshot() };
        if (snapshot.summary.remotes.empty())
        {
            show_notice(tr(u8"shell.set_upstream.no_remote"), true);
            return;
        }
        dialog_state dialog {};
        dialog.kind = dialog_kind::set_upstream;
        dialog.serial = ++ui_.dialog_serial;
        dialog.subject = branch;
        for (const remote_info& remote : snapshot.summary.remotes)
            dialog.choices.push_back(remote.name);
        // 이미 upstream이 있으면 그 원격·브랜치에서 시작하고, 없으면 같은 이름의 원격 브랜치를 가진 첫 원격을 고른다.
        const ref* const value { find_ref(snapshot, ref_kind::local, branch) };
        std::string remote_branch { branch };
        std::string remote {};
        if (value != nullptr && value->upstream.empty() == false)
        {
            if (const std::size_t slash { value->upstream.find('/') }; slash != std::string::npos)
            {
                remote = value->upstream.substr(0, slash);
                remote_branch = value->upstream.substr(slash + 1);
            }
        }
        else
        {
            for (const std::string& candidate : dialog.choices)
                if (find_ref(snapshot, ref_kind::remote, candidate + "/" + branch) != nullptr)
                {
                    remote = candidate;
                    break;
                }
        }
        const auto found { std::find(dialog.choices.begin(), dialog.choices.end(), remote) };
        dialog.choice = found == dialog.choices.end() ? 0 : static_cast<std::size_t>(found - dialog.choices.begin());
        clear_form_inputs();
        set_form_field(0, to_u8(remote_branch));
        ui_.form.identity = dialog_identity(dialog);
        ui_.dialog = std::move(dialog);
        ui_.menu.reset();
    }

    void shell_controller::open_rename_dialog(const std::string& branch)
    {
        dialog_state dialog {};
        dialog.kind = dialog_kind::rename_branch;
        dialog.serial = ++ui_.dialog_serial;
        dialog.subject = branch;
        clear_form_inputs();
        set_form_field(0, to_u8(branch));
        ui_.form.identity = dialog_identity(dialog);
        ui_.dialog = std::move(dialog);
        ui_.menu.reset();
    }

    void shell_controller::open_confirm(dialog_state dialog)
    {
        dialog.kind = dialog_kind::confirm;
        dialog.serial = ++ui_.dialog_serial;
        clear_form_inputs();
        ui_.form.identity = dialog_identity(dialog);
        ui_.dialog = std::move(dialog);
        ui_.menu.reset();
    }

    void shell_controller::request_detached_checkout(const std::string& revision, std::u8string label)
    {
        if (ui_.memory.detached_warning_suppressed)
        {
            static_cast<void>(submit(checkout_detached_request { revision }));
            return;
        }
        dialog_state dialog {};
        dialog.title = tr(u8"shell.detached.title");
        dialog.lines.push_back(tr(u8"shell.detached.description", { { u8"target", label } }));
        dialog.lines.push_back(tr(u8"shell.detached.warning"));
        dialog.confirm_label = tr(u8"shell.detached.confirm");
        dialog.check_label = tr(u8"shell.detached.dont_show_again");
        dialog.remember_detached = true;
        dialog.plan.primary.push_back(checkout_detached_request { revision });
        open_confirm(std::move(dialog));
    }

    void shell_controller::request_rebase(const std::string& branch, const std::string& onto)
    {
        repository_session* const session { selected_session() };
        if (session == nullptr)
            return;
        if (session->snapshot().summary.changed_file_count == 0)
        {
            static_cast<void>(submit(rebase_request { .branch = branch, .onto = onto, .autostash = false }));
            return;
        }
        // 작업 트리가 더러우면 rebase가 거절된다. --autostash로 하기를 확인받는다.
        dialog_state dialog {};
        dialog.title = tr(u8"shell.autostash.title");
        dialog.lines.push_back(tr(u8"shell.autostash.description", { { u8"branch", branch }, { u8"onto", onto } }));
        dialog.confirm_label = tr(u8"shell.autostash.confirm");
        dialog.plan.primary.push_back(rebase_request { .branch = branch, .onto = onto, .autostash = true });
        open_confirm(std::move(dialog));
    }

    // ---------------------------------------------------------------- 메뉴

    void shell_controller::open_context_menu(menu_target target, const float x, const float y)
    {
        if (target.kind == menu_target_kind::commit && target.sha == graph_wip_sha)
            return;
        ui_.selector.reset();
        ui_.ref_list.reset();
        ui_.menu = menu_state { .target = std::move(target), .x = x, .y = y, .asked_facts = false };
        if (ui_.menu->target.kind == menu_target_kind::drop)
            request_drop_facts();
    }

    void shell_controller::request_drop_facts()
    {
        repository_session* const session { selected_session() };
        if (session == nullptr || ui_.menu.has_value() == false || ui_.menu->asked_facts)
            return;
        ui_.menu->asked_facts = true;
        const menu_target& target { ui_.menu->target };
        if (target.destination.is_commit)
            return;
        const menu_context context { build_menu_context(*session, target, history_index(*session).get()) };
        const drop_facts& facts { context.facts };
        const std::string& source { target.source.sha };
        const std::string& destination { target.destination.sha };
        // 읽어 둔 이력으로 알 수 없는 관계만 backend에 묻는다. 답이 오면 다음 frame의 메뉴가 채워진다.
        if (facts.destination_is_ancestor.has_value() == false)
            static_cast<void>(submit(is_ancestor_request { .ancestor = destination, .descendant = source }));
        if (facts.source_is_ancestor.has_value() == false)
            static_cast<void>(submit(is_ancestor_request { .ancestor = source, .descendant = destination }));
        if (facts.only_in_source.has_value() == false)
            static_cast<void>(submit(count_commits_request { .from = destination, .to = source }));
        if (facts.only_in_destination.has_value() == false)
            static_cast<void>(submit(count_commits_request { .from = source, .to = destination }));
    }

    void shell_controller::handle_drop(const drop_intent& intent)
    {
        repository_session* const session { selected_session() };
        if (session == nullptr)
            return;
        const repository_snapshot& snapshot { session->snapshot() };
        menu_target target {};
        target.kind = menu_target_kind::drop;
        target.source = intent.source;
        target.destination = intent.destination;
        if (const ref* source { find_ref(snapshot, target.source.kind, target.source.name) }; source != nullptr)
            target.source.sha = source->target_sha;
        if (target.destination.is_commit == false)
            if (const ref* destination { find_ref(snapshot, target.destination.kind, target.destination.name) }; destination != nullptr)
                target.destination.sha = destination->target_sha;
        if (target.source.sha.empty() || target.destination.sha.empty() || can_drop_on(target.source, target.destination) == false)
            return;
        open_context_menu(std::move(target), intent.x, intent.y);
    }

    void shell_controller::handle_activate_commit(const std::string& sha)
    {
        repository_session* const session { selected_session() };
        if (session == nullptr || sha == graph_wip_sha)
            return;
        const repository_snapshot& snapshot { session->snapshot() };
        // 브랜치가 붙은 커밋을 두 번 누르면 그 브랜치로 옮긴다. 이미 그 브랜치에 있으면 할 일이 없다.
        const ref* local { nullptr };
        const ref* remote { nullptr };
        bool on_current { false };
        for (const ref& value : snapshot.refs)
        {
            if (value.target_sha != sha)
                continue;
            if (value.kind == ref_kind::local)
            {
                on_current = on_current || value.is_current;
                if (local == nullptr && value.is_current == false)
                    local = &value;
            }
            else if (value.kind == ref_kind::remote && value.name.ends_with("/HEAD") == false && remote == nullptr)
                remote = &value;
        }
        if (on_current)
            return;
        if (local != nullptr)
            static_cast<void>(submit(checkout_branch_request { local->name }));
        else if (remote != nullptr)
            static_cast<void>(submit(checkout_remote_branch_request { remote->name }));
        else
            request_detached_checkout(sha, commit_label(*session, sha));
    }

    void shell_controller::open_menu_for_selection()
    {
        std::optional<menu_target> target { selection_menu_target() };
        if (target.has_value() == false)
            return;
        const float x { (ui_.sidebar_width + 48.0f) * last_scale_ };
        const float y { 140.0f * last_scale_ };
        open_context_menu(std::move(*target), x, y);
    }

    std::optional<menu_target> shell_controller::selection_menu_target()
    {
        repository_session* const session { selected_session() };
        tab_ui_state* const tab { selected_tab_state() };
        if (session == nullptr || tab == nullptr)
            return std::nullopt;
        const repository_snapshot& snapshot { session->snapshot() };
        const std::string key { to_text(tab->selected_sidebar_key) };
        menu_target target {};
        if (key.starts_with("l:") || key.starts_with("rb:") || key.starts_with("t:"))
        {
            const ref_kind kind { key.starts_with("l:") ? ref_kind::local : key.starts_with("rb:") ? ref_kind::remote : ref_kind::tag };
            const std::string name { key.substr(key.find(':') + 1) };
            const ref* const value { find_ref(snapshot, kind, name) };
            if (value == nullptr)
                return std::nullopt;
            target.kind = kind == ref_kind::local ? menu_target_kind::local_branch : kind == ref_kind::remote ? menu_target_kind::remote_branch : menu_target_kind::tag;
            target.name = name;
            target.sha = value->target_sha;
        }
        else if (key.starts_with("s:"))
        {
            target.kind = menu_target_kind::stash;
            target.index = static_cast<std::uint32_t>(std::strtoul(key.c_str() + 2, nullptr, 10));
            target.name = "stash@{" + std::to_string(target.index) + "}";
        }
        else if (tab->selected_sha.empty() == false && tab->selected_sha != graph_wip_sha)
        {
            target.kind = menu_target_kind::commit;
            target.sha = tab->selected_sha;
        }
        else
            return std::nullopt;
        return target;
    }

    void shell_controller::handle_menu_select(const std::u8string& key)
    {
        if (ui_.menu.has_value() == false)
            return;
        const menu_state state { *ui_.menu };
        ui_.menu.reset();
        if (state.target.kind == menu_target_kind::tab)
        {
            const menu_model tab_model { build_tab_menu(tabs_, state.target.tab) };
            const auto chosen { std::find_if(tab_model.entries.begin(), tab_model.entries.end(), [&key](const menu_entry& entry) { return entry.key == key && entry.enabled; }) };
            if (chosen != tab_model.entries.end())
                run_tab_menu_action(state.target, key);
            return;
        }
        repository_session* const session { selected_session() };
        if (session == nullptr)
            return;
        const menu_context context { build_menu_context(*session, state.target, history_index(*session).get()) };
        const menu_model model { build_context_menu(state.target, context) };
        const auto found { std::find_if(model.entries.begin(), model.entries.end(), [&key](const menu_entry& entry) { return entry.key == key && entry.enabled; }) };
        if (found == model.entries.end())
            return;
        run_menu_action(state.target, key, context);
    }

    void shell_controller::run_tab_menu_action(const menu_target& target, const std::u8string& key)
    {
        const std::optional<std::size_t> index { tabs_.index_of(target.tab) };
        if (index.has_value() == false)
            return;
        const std::vector<repository_tab>& list { tabs_.tabs() };
        const std::u8string path { list[*index].path };
        if (key == u8"close" || key == u8"close_others" || key == u8"close_right")
        {
            // 닫는 동안 목록이 바뀌므로 닫을 id를 먼저 모은다. 하나씩 닫는 길은 "×"와 같다.
            std::vector<repository_tab_id> closing {};
            for (std::size_t position { 0 }; position < list.size(); ++position)
            {
                const bool picked { key == u8"close" ? position == *index : (key == u8"close_others" ? position != *index : position > *index) };
                if (picked)
                    closing.push_back(list[position].id);
            }
            for (const repository_tab_id id : closing)
                close_tab(id);
        }
        else if (key == u8"copy_path")
            copy_text(path);
        else if (key == u8"reveal")
            open_folder_in_app(0, path);
        else if (key == u8"open_vscode")
            open_folder_in_app(1, path);
    }

    void shell_controller::run_menu_action(const menu_target& target, const std::u8string& key, const menu_context& context)
    {
        repository_session* const session { selected_session() };
        if (session == nullptr)
            return;
        const std::string current { context.detached ? std::string {} : context.current_branch };
        const auto confirm_reset = [this](const reset_mode mode, const std::string& to, const std::u8string& label) {
            if (mode != reset_mode::hard)
            {
                static_cast<void>(submit(reset_request { .mode = mode, .to = to }));
                return;
            }
            dialog_state dialog {};
            dialog.title = tr(u8"shell.reset_hard.title");
            dialog.lines.push_back(tr(u8"shell.reset_hard.description", { { u8"target", label } }));
            dialog.confirm_label = u8"hard reset";
            dialog.danger = true;
            dialog.plan.primary.push_back(reset_request { .mode = reset_mode::hard, .to = to });
            open_confirm(std::move(dialog));
        };
        const auto reset_mode_of = [&key]() -> std::optional<reset_mode> {
            if (key == u8"reset_soft")
                return reset_mode::soft;
            if (key == u8"reset_mixed")
                return reset_mode::mixed;
            if (key == u8"reset_hard")
                return reset_mode::hard;
            return std::nullopt;
        };

        switch (target.kind)
        {
        case menu_target_kind::commit:
            if (key == u8"checkout_detached")
                request_detached_checkout(target.sha, commit_label(*session, target.sha));
            else if (key == u8"create_branch")
                open_create_branch_dialog(target.sha);
            else if (key == u8"create_tag")
                open_create_tag_dialog(target.sha);
            else if (key == u8"cherry_pick")
                static_cast<void>(submit(cherry_pick_request { target.sha }));
            else if (key == u8"revert")
                static_cast<void>(submit(revert_request { target.sha }));
            else if (key == u8"copy_sha")
                copy_text(to_u8(target.sha));
            else if (key == u8"add_worktree")
                open_add_worktree_dialog(target.sha, 0, {});
            else if (key == u8"interactive_rebase_here")
                open_rebase_editor(target.sha, current, short_sha(target.sha), current.empty() ? std::u8string { u8"HEAD" } : to_u8(current));
            else if (const std::optional<reset_mode> mode { reset_mode_of() }; mode.has_value())
                confirm_reset(*mode, target.sha, short_sha(target.sha));
            return;
        case menu_target_kind::local_branch:
            if (key == u8"checkout")
                static_cast<void>(submit(checkout_branch_request { target.name }));
            else if (key == u8"merge_into_current")
                static_cast<void>(submit(merge_request { .into = current, .from = target.name }));
            else if (key == u8"rebase_current_onto")
                request_rebase(current, target.name);
            else if (key == u8"interactive_rebase_current_onto")
                open_rebase_editor(target.name, current, to_u8(target.name), to_u8(current));
            else if (key == u8"push")
                open_push_dialog(target.name, false);
            else if (key == u8"set_upstream")
                open_set_upstream_dialog(target.name);
            else if (key == u8"unset_upstream")
                static_cast<void>(submit(unset_upstream_request { target.name }));
            else if (key == u8"rename")
                open_rename_dialog(target.name);
            else if (key == u8"add_worktree")
                open_add_worktree_dialog({}, 1, target.name);
            else if (key == u8"open_worktree")
            {
                if (const ref* const branch { find_ref(session->snapshot(), ref_kind::local, target.name) }; branch != nullptr && branch->worktree_path.empty() == false)
                    open_repository(to_u8(branch->worktree_path));
            }
            else if (key == u8"copy_name")
                copy_text(to_u8(target.name));
            else if (key == u8"delete")
            {
                dialog_state dialog {};
                dialog.title = tr(u8"shell.delete_branch.title");
                dialog.lines.push_back(tr(u8"shell.delete_branch.description", { { u8"branch", target.name } }));
                dialog.confirm_label = tr(u8"shell.confirm.delete");
                dialog.danger = true;
                dialog.check_label = tr(u8"shell.delete_branch.force");
                dialog.check_warning = false;
                dialog.plan.primary.push_back(delete_branch_request { .name = target.name, .force = false });
                dialog.plan.alternate.push_back(delete_branch_request { .name = target.name, .force = true });
                open_confirm(std::move(dialog));
            }
            return;
        case menu_target_kind::remote_branch:
            if (key == u8"checkout")
                static_cast<void>(submit(checkout_remote_branch_request { target.name }));
            else if (key == u8"merge_into_current")
                static_cast<void>(submit(merge_request { .into = current, .from = target.name }));
            else if (key == u8"rebase_current_onto")
                request_rebase(current, target.name);
            else if (key == u8"interactive_rebase_current_onto")
                open_rebase_editor(target.name, current, to_u8(target.name), to_u8(current));
            else if (key == u8"copy_name")
                copy_text(to_u8(target.name));
            return;
        case menu_target_kind::tag: {
            if (key == u8"checkout_detached")
                request_detached_checkout(target.name, to_u8(target.name));
            else if (key == u8"copy_name")
                copy_text(to_u8(target.name));
            else if (const auto push_remote { menu_key_argument(key, u8"push:") }; push_remote.has_value())
                static_cast<void>(submit(push_tag_request { .remote = *push_remote, .name = target.name }));
            else if (const auto remote_delete { menu_key_argument(key, u8"delete_remote:") }; remote_delete.has_value())
            {
                dialog_state dialog {};
                dialog.title = tr(u8"shell.delete_remote_tag.title");
                dialog.lines.push_back(tr(u8"shell.delete_remote_tag.description", { { u8"remote", *remote_delete }, { u8"tag", target.name } }));
                dialog.confirm_label = tr(u8"shell.delete_remote_tag.confirm");
                dialog.danger = true;
                dialog.plan.primary.push_back(delete_remote_tag_request { .remote = *remote_delete, .name = target.name });
                open_confirm(std::move(dialog));
            }
            else if (key == u8"delete_local")
            {
                dialog_state dialog {};
                dialog.title = tr(u8"shell.delete_tag.title");
                dialog.lines.push_back(tr(u8"shell.delete_tag.description", { { u8"tag", target.name } }));
                dialog.confirm_label = tr(u8"shell.confirm.delete");
                dialog.danger = true;
                dialog.plan.primary.push_back(delete_tag_request { target.name });
                const ref* const tag { find_ref(session->snapshot(), ref_kind::tag, target.name) };
                if (tag != nullptr && tag->remotes.empty() == false)
                {
                    // 원격을 먼저 지우고, 실패하면 로컬은 남긴다.
                    dialog.check_label = tag->remotes.size() > 1 ? tr(u8"shell.delete_tag.also_remotes", { { u8"remote", tag->remotes.front() } })
                                                                 : tr(u8"shell.delete_tag.also_remote", { { u8"remote", tag->remotes.front() } });
                    dialog.check_warning = true;
                    for (const std::string& remote : tag->remotes)
                        dialog.plan.alternate.push_back(delete_remote_tag_request { .remote = remote, .name = target.name });
                    dialog.plan.alternate.push_back(delete_tag_request { target.name });
                }
                open_confirm(std::move(dialog));
            }
            return;
        }
        case menu_target_kind::stash:
            if (key == u8"apply")
                static_cast<void>(submit(stash_apply_request { target.index }));
            else if (key == u8"pop")
                static_cast<void>(submit(stash_pop_request { target.index }));
            else if (key == u8"drop")
            {
                dialog_state dialog {};
                dialog.title = tr(u8"shell.drop_stash.title");
                dialog.lines.push_back(tr(u8"shell.drop_stash.description", { { u8"stash", target.name } }));
                dialog.confirm_label = tr(u8"shell.confirm.delete");
                dialog.danger = true;
                dialog.plan.primary.push_back(stash_drop_request { target.index });
                open_confirm(std::move(dialog));
            }
            return;
        case menu_target_kind::tags_section:
            if (const auto remote { menu_key_argument(key, u8"push_all:") }; remote.has_value())
                static_cast<void>(submit(push_tags_request { *remote }));
            else if (key == u8"create_tag_head")
                open_create_tag_dialog(context.head_sha);
            return;
        case menu_target_kind::file:
            if (key == u8"stage")
                static_cast<void>(submit(stage_files_request { { target.name } }));
            else if (key == u8"unstage")
                static_cast<void>(submit(unstage_files_request { { target.name } }));
            else if (key == u8"discard")
                handle_dialog_message(luil::app_message { discard_files_intent { { target.name }, target.untracked } });
            else if (key == u8"ignore")
                static_cast<void>(submit(ignore_path_request { "/" + target.name }));
            else if (key == u8"copy_path")
                copy_text(to_u8(target.name));
            else if (key == u8"open_external_diff")
                show_notice(tr(u8"shell.external_diff.unavailable"));
            else if (key == u8"lfs_track")
                open_lfs_track_dialog(lfs_pattern_for_path(target.name));
            else if (key == u8"lfs_pull")
                static_cast<void>(submit(lfs_pull_request {}));
            return;
        case menu_target_kind::tab:
            run_tab_menu_action(target, key);
            return;
        case menu_target_kind::commit_file:
            if (key == u8"open_diff")
                static_cast<void>(handle(luil::app_message { select_file_intent { diff_file_request { .path = target.name, .view = diff_view::commit, .sha = target.sha } } }));
            else if (key == u8"copy_path")
                copy_text(to_u8(target.name));
            else if (key == u8"copy_name")
            {
                const std::size_t slash { target.name.find_last_of('/') };
                copy_text(to_u8(slash == std::string::npos ? target.name : target.name.substr(slash + 1)));
            }
            else if (key == u8"copy_revision_path")
                copy_text(short_sha(target.sha) + u8":" + to_u8(target.name));
            return;
        case menu_target_kind::submodule:
        case menu_target_kind::submodules_section:
        case menu_target_kind::worktree:
        case menu_target_kind::worktrees_section:
        case menu_target_kind::lfs_section:
        case menu_target_kind::lfs_file:
            run_extension_menu_action(target, key, context);
            return;
        case menu_target_kind::drop: {
            const drop_party& source { target.source };
            const drop_party& destination { target.destination };
            if (key == u8"create_branch_here")
                open_create_branch_dialog(destination.sha);
            else if (key == u8"fast_forward")
                static_cast<void>(submit(fast_forward_request { .target = destination.name, .to = source.name }));
            else if (key == u8"merge")
                static_cast<void>(submit(merge_request { .into = destination.name, .from = source.name }));
            else if (key == u8"merge_remote_into_source")
                static_cast<void>(submit(merge_request { .into = source.name, .from = destination.name }));
            else if (key == u8"rebase")
                request_rebase(source.name, destination.name);
            else if (key == u8"interactive_rebase")
                open_rebase_editor(destination.name, source.name, to_u8(destination.name), to_u8(source.name));
            else if (const std::optional<reset_mode> mode { reset_mode_of() }; mode.has_value())
            {
                // 커밋에 놓았으면 끈 브랜치(현재)를 그 커밋으로, 브랜치에 놓았으면 놓은 브랜치(현재)를 끈 브랜치 위치로 옮긴다.
                if (destination.is_commit)
                    confirm_reset(*mode, destination.sha, short_sha(destination.sha));
                else
                    confirm_reset(*mode, source.name, to_u8(source.name));
            }
            return;
        }
        }
    }

    // ---------------------------------------------------------------- 모달 입력 상자

    void shell_controller::handle_form_button(const std::u8string& key)
    {
        sync_form_inputs();
        const form_source source { current_form_source() };
        switch (source.kind)
        {
        case form_source_kind::auth:
            handle_auth_button(*source.status, key);
            return;
        case form_source_kind::dialog:
            handle_dialog_button(key);
            return;
        case form_source_kind::dirty_checkout:
            handle_dirty_button(*source.status, key);
            return;
        case form_source_kind::none:
            break;
        }
    }

    void shell_controller::handle_form_check(const std::size_t index)
    {
        const form_source source { current_form_source() };
        switch (source.kind)
        {
        case form_source_kind::auth:
            if (index == 0)
                ui_.form.reveal = ui_.form.reveal == false;
            return;
        case form_source_kind::dialog:
            if (ui_.dialog.has_value() == false)
                return;
            switch (ui_.dialog->kind)
            {
            case dialog_kind::create_branch:
            case dialog_kind::confirm:
                if (index == 0)
                    ui_.dialog->flag_a = ui_.dialog->flag_a == false;
                return;
            case dialog_kind::push:
                if (index == 0)
                    ui_.dialog->flag_a = ui_.dialog->flag_a == false;
                else if (index == 1)
                    ui_.dialog->flag_b = ui_.dialog->flag_b == false;
                else if (index == 2)
                    ui_.dialog->flag_c = ui_.dialog->flag_c == false;
                return;
            case dialog_kind::create_tag:
                // 서명 토글이다. 서명한 태그는 annotated뿐이라 켜면 종류도 annotated로 옮긴다.
                if (index == 0)
                {
                    ui_.dialog->flag_b = ui_.dialog->flag_b == false;
                    if (ui_.dialog->flag_b)
                        ui_.dialog->choice = 1;
                }
                return;
            case dialog_kind::rename_branch:
            case dialog_kind::create_pull_request:
                if (index == 0)
                    ui_.dialog->flag_a = ui_.dialog->flag_a == false;
                else if (index == 1 && ui_.dialog->flag_c)
                    ui_.dialog->flag_b = ui_.dialog->flag_b == false;
                return;
            case dialog_kind::add_worktree:
            case dialog_kind::lfs_track:
            case dialog_kind::set_upstream:
            case dialog_kind::none:
                return;
            }
            return;
        case form_source_kind::dirty_checkout:
        case form_source_kind::none:
            break;
        }
    }

    void shell_controller::handle_form_choose(const std::size_t index)
    {
        const form_source source { current_form_source() };
        if (source.kind == form_source_kind::dirty_checkout)
        {
            ui_.form.choice = std::min<std::size_t>(index, 2);
            return;
        }
        if (source.kind != form_source_kind::dialog || ui_.dialog.has_value() == false)
            return;
        if ((ui_.dialog->kind == dialog_kind::push || ui_.dialog->kind == dialog_kind::set_upstream) && index < ui_.dialog->choices.size())
            ui_.dialog->choice = index;
        else if (ui_.dialog->kind == dialog_kind::create_tag && index < 2)
        {
            ui_.dialog->choice = index;
            // lightweight 태그는 서명할 수 없다.
            if (index == 0)
                ui_.dialog->flag_b = false;
        }
        else if (ui_.dialog->kind == dialog_kind::add_worktree && index < 3)
            ui_.dialog->choice = index;
    }

    void shell_controller::handle_form_enter()
    {
        sync_form_inputs();
        const form_source source { current_form_source() };
        const repository_session* const session { selected_session() };
        const std::optional<form_view> form { build_form_view(source, ui_, selected_tab_state(), session != nullptr ? &session->snapshot() : nullptr) };
        if (form.has_value() == false)
            return;
        for (const form_button& button : form->buttons)
            if (button.default_button && button.enabled)
            {
                handle_form_button(button.key);
                return;
            }
    }

    void shell_controller::handle_dialog_button(const std::u8string& key)
    {
        if (ui_.dialog.has_value() == false)
            return;
        const dialog_state dialog { *ui_.dialog };
        const auto close = [this]() {
            clear_form_inputs();
            ui_.form.identity.clear();
            ui_.dialog.reset();
        };
        if (key == u8"cancel")
        {
            close();
            return;
        }
        if (key != u8"submit")
            return;
        repository_session* const session { selected_session() };
        if (session == nullptr)
        {
            close();
            return;
        }
        const repository_snapshot& snapshot { session->snapshot() };
        switch (dialog.kind)
        {
        case dialog_kind::create_branch:
            if (create_branch_ready(dialog, ui_.form, snapshot) == false)
                return;
            static_cast<void>(submit(create_branch_request { .name = trimmed_text(ui_.form.fields[0].text), .at = dialog.subject, .checkout = dialog.flag_a }));
            break;
        case dialog_kind::create_tag:
            if (create_tag_ready(dialog, ui_.form, snapshot) == false)
                return;
            {
                create_tag_request request {};
                request.name = trimmed_text(ui_.form.fields[0].text);
                request.sha = dialog.subject;
                request.annotated = dialog.choice == 1;
                if (request.annotated)
                    request.message = trimmed_text(ui_.form.fields[1].text);
                // 서명: 켜면 true, 설정(`tag.gpgsign`)이 켜져 있는데 껐으면 false(설정을 덮는다), 설정을 따르면 값 없음.
                if (dialog.flag_b)
                    request.sign = true;
                else if (snapshot.signing.tag_signing())
                    request.sign = false;
                static_cast<void>(submit(std::move(request)));
                break;
            }
        case dialog_kind::push:
            if (push_ready(dialog, ui_.form, snapshot) == false)
                return;
            static_cast<void>(submit(make_push_request(dialog, ui_.form)));
            break;
        case dialog_kind::set_upstream:
            if (set_upstream_ready(dialog, ui_.form, snapshot) == false)
                return;
            static_cast<void>(submit(make_set_upstream_request(dialog, ui_.form)));
            break;
        case dialog_kind::rename_branch:
            if (rename_branch_ready(dialog, ui_.form, snapshot) == false)
                return;
            static_cast<void>(submit(rename_branch_request { .old_name = dialog.subject, .new_name = trimmed_text(ui_.form.fields[0].text) }));
            break;
        case dialog_kind::confirm: {
            if (dialog.remember_detached && dialog.flag_a)
                ui_.memory.detached_warning_suppressed = true;
            const bool use_alternate { dialog.remember_detached == false && dialog.flag_a && dialog.plan.alternate.empty() == false };
            static_cast<void>(submit_sequence(use_alternate ? dialog.plan.alternate : dialog.plan.primary));
            break;
        }
        case dialog_kind::add_worktree:
            if (add_worktree_ready(dialog, ui_.form, snapshot) == false)
                return;
            static_cast<void>(submit(make_worktree_add_request(dialog, ui_.form)));
            break;
        case dialog_kind::lfs_track:
            if (lfs_track_ready(dialog, ui_.form) == false)
                return;
            static_cast<void>(submit(make_lfs_track_request(ui_.form)));
            break;
        case dialog_kind::create_pull_request:
            if (create_pull_request_ready(dialog, ui_.form) == false)
                return;
            submit_create_pull_request(dialog);
            break;
        case dialog_kind::none:
            break;
        }
        close();
    }

    void shell_controller::handle_dirty_button(const operation_status& status, const std::u8string& key)
    {
        tab_ui_state* const tab { selected_tab_state() };
        if (tab == nullptr || dependencies_.workspace == nullptr)
            return;
        const auto dismiss = [&]() {
            tab->dismissed_dialog = std::max(tab->dismissed_dialog, status.id);
            tab->dismissed_banner = std::max(tab->dismissed_banner, status.id);
            tab->dirty.reset();
            clear_form_inputs();
        };
        if (key == u8"cancel")
        {
            dismiss();
            return;
        }
        const dirty_checkout_state state { tab->dirty.has_value() && tab->dirty->operation == status.id ? *tab->dirty : dirty_checkout_state { status.id, false } };
        if (key == u8"back")
        {
            tab->dirty = dirty_checkout_state { status.id, false };
            return;
        }
        if (key != u8"submit")
            return;
        const std::size_t choice { std::min<std::size_t>(ui_.form.choice, 2) };
        if (choice == 2 && state.confirming_discard == false)
        {
            tab->dirty = dirty_checkout_state { status.id, true };
            return;
        }
        const git_operation* const request { dependencies_.workspace->request_of(status.id) };
        if (request == nullptr)
            return;
        const checkout_policy policy { choice == 0 ? checkout_policy::stash_and_reapply : choice == 1 ? checkout_policy::carry : checkout_policy::discard };
        std::optional<git_operation> retry { with_policy(*request, policy) };
        if (retry.has_value() == false)
            return;
        dismiss();
        static_cast<void>(submit(std::move(*retry)));
    }

    void shell_controller::handle_auth_button(const operation_status& status, const std::u8string& key)
    {
        git_workspace* const workspace { dependencies_.workspace };
        if (workspace == nullptr || status.needs_input.has_value() == false)
            return;
        const operation_id id { status.id };
        const std::uint64_t prompt { status.needs_input->prompt_id };
        if (key == u8"cancel")
        {
            // 답 없이 닫는다. operation은 `auth_cancelled`로 끝나고 이후 질문도 취소로 풀린다.
            send_backend_intent(luil::app_message { cancel_input_intent { .id = id, .prompt_id = prompt } });
            clear_form_inputs();
            return;
        }
        if (key == u8"once")
        {
            trust_host_once(status);
            clear_form_inputs();
            return;
        }
        std::u8string answer {};
        if (key == u8"yes" || key == u8"no")
            answer = key;
        else if (key == u8"answer")
            answer = ui_.form.fields[0].text;
        else
            return;
        // 답은 비밀일 수 있어 메시지가 쥔 사본을 처리기가 쓰고 바로 지운다. 이쪽 사본과 입력 상자의 글도 함께 비운다.
        send_backend_intent(luil::app_message { answer_input_intent { .id = id, .prompt_id = prompt, .answer = answer } });
        wipe_secret_text(answer);
        clear_form_inputs();
    }

    void shell_controller::send_backend_intent(const luil::app_message& message)
    {
        if (dependencies_.send_backend_intent != nullptr)
        {
            dependencies_.send_backend_intent(message);
            return;
        }
        // driver 없이 쓰는 조립(test)에서는 driver의 처리기와 같은 일을 workspace에 바로 한다.
        git_workspace* const workspace { dependencies_.workspace };
        if (workspace == nullptr)
            return;
        if (const auto* const answer { message.get<answer_input_intent>() }; answer != nullptr)
        {
            std::u8string text { answer->answer };
            wipe_secret_text(answer->answer);
            static_cast<void>(workspace->answer_input(answer->id, answer->prompt_id, text));
            wipe_secret_text(text);
        }
        else if (const auto* const cancel { message.get<cancel_input_intent>() }; cancel != nullptr)
            static_cast<void>(workspace->cancel_input(cancel->id, cancel->prompt_id));
    }

    void shell_controller::trust_host_once(const operation_status& status)
    {
        git_workspace* const workspace { dependencies_.workspace };
        const git_operation* const request { workspace->request_of(status.id) };
        if (request == nullptr)
            return;
        std::error_code error {};
        std::filesystem::path directory { dependencies_.temporary_directory.empty() ? std::filesystem::temp_directory_path(error) : std::filesystem::path { dependencies_.temporary_directory } };
        if (error)
            directory = ".";
        const std::filesystem::path known_hosts { directory / ("cyella-known-hosts-" + std::to_string(++temporary_counter_) + "-" + std::to_string(status.id) + ".tmp") };
        temporary_ssh_trust trust {};
        trust.command
            = make_temporary_known_hosts_ssh_command(dependencies_.ssh_executable.empty() ? std::u8string_view { u8"ssh" } : std::u8string_view { dependencies_.ssh_executable }, known_hosts);
        trust.known_hosts_path = known_hosts.u8string();
        std::optional<git_operation> retry { with_ssh_override(*request, trust) };
        // 질문 중인 operation을 거둔다. 취소는 알림 없이 끝나고 "답 없이 닫기"의 실패 배너도 남기지 않는다.
        static_cast<void>(workspace->cancel(status.id));
        if (retry.has_value() == false)
            return;
        const std::string repository { status.repository_id };
        const bool refreshes_tags { refreshes_remote_tags(*retry) };
        const operation_id retried { is_setup_operation(operation_kind_of(*retry)) ? workspace->submit_setup(*retry) : workspace->submit(repository, *retry) };
        if (retried == 0)
            return;
        if (refreshes_tags)
            trusted_retries_[retried] = trusted_retry { repository, trust };
        if (status.kind == operation_kind::clone)
        {
            // Clone 대화 상자와 열기 예약을 새 operation으로 옮긴다.
            if (const auto pending { pending_setup_.find(status.id) }; pending != pending_setup_.end())
            {
                pending_setup_[retried] = pending->second;
                pending_setup_.erase(pending);
            }
            if (ui_.clone.has_value() && ui_.clone->operation == status.id)
                ui_.clone->operation = retried;
        }
    }

    void shell_controller::handle_banner_action(const banner_action_intent& intent)
    {
        // 프로필 제안과 커밋 전 경고의 조치는 operation이 아니라 안내 배너에 달려 있다.
        if (handle_profile_banner_action(intent.key))
            return;
        git_workspace* const workspace { dependencies_.workspace };
        tab_ui_state* const tab { selected_tab_state() };
        if (workspace == nullptr)
            return;
        const git_operation* const request { workspace->request_of(intent.id) };
        if (request == nullptr)
            return;
        const git_operation original { *request };
        const auto dismiss = [&]() {
            if (tab != nullptr)
            {
                tab->dismissed_banner = std::max(tab->dismissed_banner, intent.id);
                tab->banner_expanded = false;
            }
        };
        if (intent.key == u8"pull_then_push")
        {
            // rebase로 원격 커밋을 가져온 뒤 같은 push를 다시 한다. pull이 실패하면 push는 나가지 않는다.
            if (std::holds_alternative<push_execute_request>(original))
            {
                dismiss();
                static_cast<void>(submit_sequence({ pull_request { pull_strategy::rebase }, original }));
            }
        }
        else if (intent.key == u8"fetch")
        {
            fetch_request fetch {};
            if (const auto* const push { std::get_if<push_execute_request>(&original) }; push != nullptr)
                fetch.remote = push->remote;
            dismiss();
            static_cast<void>(submit(fetch));
        }
        else if (intent.key == u8"pull_rebase")
        {
            dismiss();
            static_cast<void>(submit(pull_request { pull_strategy::rebase }));
        }
        else if (intent.key == u8"force_delete")
        {
            if (const auto* const deleted { std::get_if<delete_branch_request>(&original) }; deleted != nullptr)
            {
                dismiss();
                dialog_state dialog {};
                dialog.title = tr(u8"shell.force_delete_branch.title");
                dialog.lines.push_back(tr(u8"shell.force_delete_branch.description", { { u8"branch", deleted->name } }));
                dialog.confirm_label = tr(u8"shell.force_delete_branch.confirm");
                dialog.danger = true;
                dialog.plan.primary.push_back(delete_branch_request { .name = deleted->name, .force = true });
                open_confirm(std::move(dialog));
            }
        }
        else if (intent.key == u8"remove_index_lock")
        {
            dismiss();
            static_cast<void>(submit(remove_stale_index_lock_request {}));
        }
    }

    void shell_controller::handle_conflict_action(const conflict_action action)
    {
        repository_session* const session { selected_session() };
        if (session == nullptr)
            return;
        const repository_operation_state state { session->snapshot().summary.in_progress.kind };
        const std::optional<operation_kind> kind { sequence_kind(state, action) };
        if (kind.has_value() == false)
            return;
        if (action != conflict_action::abort_sequence)
        {
            static_cast<void>(submit(sequence_control_request { *kind }));
            return;
        }
        dialog_state dialog {};
        dialog.title = tr(u8"shell.abort_sequence.title", { { u8"operation", repository_operation_state_name(state) } });
        dialog.lines.push_back(tr(u8"shell.abort_sequence.description"));
        dialog.confirm_label = tr(u8"shell.abort_sequence.confirm");
        dialog.danger = true;
        dialog.plan.primary.push_back(sequence_control_request { *kind });
        open_confirm(std::move(dialog));
    }
} // namespace cyella
