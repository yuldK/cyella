#include "presentation/snapshot_dialogs.h"

#include "presentation/build_view_snapshot.h"
#include "presentation/form_builders.h"
#include "presentation/form_extension.h"
#include "presentation/form_hosting.h"

#include "luil/generated/codicons.h"

#include <algorithm>
#include <utility>

namespace cyella {
    namespace {
        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::u8string number(const std::uint64_t value)
        {
            return to_u8(std::to_string(value));
        }

        bool is_auth_kind(const input_kind kind) noexcept
        {
            return kind == input_kind::passphrase || kind == input_kind::host_key || kind == input_kind::credentials;
        }

        // 아직 끝나지 않고 질문을 기다리는 operation이다.
        bool awaiting_auth(const operation_status& status) noexcept
        {
            return status.state == operation_state::needs_input && status.finished() == false && status.needs_input.has_value() && is_auth_kind(status.needs_input->kind);
        }

        const operation_status* latest_finished_user_operation(std::span<const operation_status* const> operations)
        {
            for (auto it { operations.rbegin() }; it != operations.rend(); ++it)
                if (is_user_operation_status(**it) && (*it)->finished())
                    return *it;
            return nullptr;
        }

        std::u8string conflict_meaning(const std::string& code)
        {
            if (code == "UU")
                return u8"양쪽에서 수정했습니다";
            if (code == "AA")
                return u8"양쪽에서 추가했습니다";
            if (code == "DD")
                return u8"양쪽에서 삭제했습니다";
            if (code == "DU")
                return u8"우리 쪽에서 삭제했습니다";
            if (code == "UD")
                return u8"그들 쪽에서 삭제했습니다";
            if (code == "AU")
                return u8"우리 쪽에서 추가했습니다";
            if (code == "UA")
                return u8"그들 쪽에서 추가했습니다";
            return {};
        }

        std::u8string branch_of_head_name(const std::string& head_name)
        {
            constexpr std::string_view prefix { "refs/heads/" };
            if (std::string_view { head_name }.substr(0, prefix.size()) == prefix)
                return to_u8(std::string_view { head_name }.substr(prefix.size()));
            return to_u8(head_name);
        }
    } // namespace

    form_source resolve_form_source(
        std::span<const operation_status* const> repository_operations, std::span<const operation_status* const> setup_operations, const ui_state& ui, const tab_ui_state* tab)
    {
        const operation_status* auth { nullptr };
        for (const auto& group : { repository_operations, setup_operations })
            for (const operation_status* status : group)
                if (status != nullptr && awaiting_auth(*status) && (auth == nullptr || status->id < auth->id))
                    auth = status;
        if (auth != nullptr)
            return { form_source_kind::auth, auth };
        if (ui.dialog.has_value())
            return { form_source_kind::dialog, nullptr };
        if (tab != nullptr)
            if (const operation_status* last { latest_finished_user_operation(repository_operations) }; last != nullptr && last->state == operation_state::needs_input && last->needs_input.has_value()
                && last->needs_input->kind == input_kind::dirty_worktree && last->id > tab->dismissed_dialog)
                return { form_source_kind::dirty_checkout, last };
        return {};
    }

    std::string form_identity(const form_source& source, const ui_state& ui)
    {
        switch (source.kind)
        {
        case form_source_kind::auth:
            return auth_identity(*source.status);
        case form_source_kind::dirty_checkout:
            return dirty_identity(*source.status);
        case form_source_kind::dialog:
            return ui.dialog.has_value() ? dialog_identity(*ui.dialog) : std::string {};
        case form_source_kind::none:
            break;
        }
        return {};
    }

    std::optional<form_view> build_form_view(const form_source& source, const ui_state& ui, const tab_ui_state* tab, const repository_snapshot* snapshot)
    {
        static const repository_snapshot empty_snapshot {};
        form_build_context context {};
        context.snapshot = snapshot != nullptr ? snapshot : &empty_snapshot;
        context.inputs = &ui.form;
        context.composition = &ui.composition;
        switch (source.kind)
        {
        case form_source_kind::auth:
            return build_auth_form(*source.status, context);
        case form_source_kind::dirty_checkout: {
            const bool has_matching_dirty { tab != nullptr && tab->dirty.has_value() && tab->dirty->operation == source.status->id };
            const dirty_checkout_state state { has_matching_dirty ? *tab->dirty : dirty_checkout_state { source.status->id, false } };
            return build_dirty_checkout_form(*source.status, state, context);
        }
        case form_source_kind::dialog:
            if (ui.dialog.has_value() == false)
                return std::nullopt;
            switch (ui.dialog->kind)
            {
            case dialog_kind::create_branch:
                return build_create_branch_form(*ui.dialog, context);
            case dialog_kind::create_tag:
                return build_create_tag_form(*ui.dialog, context);
            case dialog_kind::push:
                return build_push_form(*ui.dialog, context);
            case dialog_kind::rename_branch:
                return build_rename_branch_form(*ui.dialog, context);
            case dialog_kind::confirm:
                return build_confirm_form(*ui.dialog, context);
            case dialog_kind::add_worktree:
                return build_add_worktree_form(*ui.dialog, context);
            case dialog_kind::lfs_track:
                return build_lfs_track_form(*ui.dialog, context);
            case dialog_kind::set_upstream:
                return build_set_upstream_form(*ui.dialog, context);
            case dialog_kind::create_pull_request:
                return build_create_pull_request_form(*ui.dialog, context);
            case dialog_kind::none:
                break;
            }
            return std::nullopt;
        case form_source_kind::none:
            break;
        }
        return std::nullopt;
    }

    menu_context build_menu_context(const repository_session& session, const menu_target& target, const graph_index* index)
    {
        const repository_snapshot& snapshot { session.snapshot() };
        menu_context context {};
        context.current_branch = snapshot.summary.current_branch;
        context.detached = snapshot.summary.is_detached;
        context.head_sha = snapshot.summary.head_sha;
        context.refs = snapshot.refs;
        for (const remote_info& remote : snapshot.summary.remotes)
            context.remotes.push_back(remote.name);
        context.worktree_dirty = snapshot.summary.changed_file_count > 0;
        context.submodules = snapshot.submodules;
        context.worktrees = snapshot.worktrees;
        context.lfs = snapshot.lfs;
        if (target.kind == menu_target_kind::commit && index != nullptr)
        {
            // 병합 커밋은 부모를 골라야 cherry-pick·revert를 할 수 있다.
            const std::vector<commit_info>& commits { session.graph_commits() };
            if (const std::optional<std::size_t> position { index->find(target.sha) }; position.has_value() && *position < commits.size())
                context.commit_is_merge = commits[*position].parents.size() > 1;
        }
        if (target.kind == menu_target_kind::drop && target.destination.is_commit == false)
        {
            drop_facts& facts { context.facts };
            const std::string& source { target.source.sha };
            const std::string& destination { target.destination.sha };
            if (index != nullptr)
            {
                facts.destination_is_ancestor = index->is_ancestor(destination, source);
                facts.source_is_ancestor = index->is_ancestor(source, destination);
                facts.only_in_source = index->count(destination, source);
                facts.only_in_destination = index->count(source, destination);
            }
            // 읽어 둔 이력으로 모르는 것은 backend가 답한 값으로 채운다.
            if (facts.destination_is_ancestor.has_value() == false)
                facts.destination_is_ancestor = session.ancestry(destination, source);
            if (facts.source_is_ancestor.has_value() == false)
                facts.source_is_ancestor = session.ancestry(source, destination);
            if (facts.only_in_source.has_value() == false)
                facts.only_in_source = session.commit_count(destination, source);
            if (facts.only_in_destination.has_value() == false)
                facts.only_in_destination = session.commit_count(source, destination);
        }
        return context;
    }

    std::optional<menu_view> build_menu_view(
        const ui_state& ui, const repository_tabs& tabs, const repository_session* session, const graph_index* index, const float scale)
    {
        if (ui.menu.has_value() == false)
            return std::nullopt;
        const menu_state& state { *ui.menu };
        menu_view view {};
        view.x = state.x / scale;
        view.y = state.y / scale;
        menu_model model {};
        // 탭 메뉴는 저장소 상태 없이 탭 목록으로 정한다. 시작 탭에서도 열린다.
        if (state.target.kind == menu_target_kind::tab)
            model = build_tab_menu(tabs, state.target.tab);
        else if (session != nullptr)
            model = build_context_menu(state.target, build_menu_context(*session, state.target, index));
        if (model.entries.empty())
            return std::nullopt;
        view.title = std::move(model.title);
        view.entries = std::move(model.entries);
        return view;
    }

    std::optional<selector_view> build_selector_view(
        const ui_state& ui, const repository_tabs& tabs, const app_settings& settings, const repository_exists_function& exists, const repository_session* session, const float scale)
    {
        if (ui.selector.has_value() == false)
            return std::nullopt;
        const selector_state& state { *ui.selector };
        selector_view view {};
        view.kind = state.kind;
        view.x = state.x / scale;
        view.y = state.y / scale;
        view.width = state.width / scale;
        view.filter = luil::make_text_input_view(state.filter, ui.composition, to_luil_target(text_target::selector_filter));
        const std::u8string query { view.filter.displayed_text() };
        if (state.kind == selector_kind::file_app)
        {
            view.placeholder = u8"파일을 열 앱 찾기";
            std::size_t matched { 0 };
            for (std::size_t index { 0 }; index < settings.external_file_apps.size(); ++index)
            {
                const external_file_app& app { settings.external_file_apps[index] };
                if (selector_matches(app.name, query))
                {
                    if (++matched <= selector_visible_limit)
                        view.entries.push_back({ u8"file-app:" + to_u8(std::to_string(index)), app.name });
                }
            }
            if (matched > selector_visible_limit)
                view.entries.push_back({ .key = u8"more", .label = u8"이름을 입력해 앱을 찾으세요", .enabled = false });
            view.entries.push_back({ .key = u8"file-app-settings", .label = u8"앱 추가·편집…", .separator_above = true });
            return view;
        }
        if (state.kind == selector_kind::repository_app)
        {
            view.placeholder = u8"저장소를 열 앱 찾기";
            const std::vector<external_file_app> apps { repository_open_apps(settings.external_file_apps) };
            std::size_t matched { 0 };
            for (std::size_t index { 0 }; index < apps.size(); ++index)
            {
                if (selector_matches(apps[index].name, query) == false)
                    continue;
                if (++matched > selector_visible_limit)
                    continue;
                // 앞의 둘은 `repository_open_apps`가 늘 세우는 탐색기와 VS Code다.
                const char32_t icon { index == 0 ? luil::codicons::icon_folder_opened : (index == 1 ? luil::codicons::icon_vscode : luil::codicons::icon_link_external) };
                view.entries.push_back({ .key = std::u8string { selector_repository_app_key_prefix } + to_u8(std::to_string(index)), .label = apps[index].name, .icon = icon });
            }
            if (matched > selector_visible_limit)
                view.entries.push_back({ .key = u8"more", .label = u8"이름을 입력해 앱을 찾으세요", .enabled = false });
            view.entries.push_back({ .key = u8"file-app-settings", .label = u8"앱 추가·편집…", .separator_above = true });
            return view;
        }
        if (state.kind == selector_kind::repository)
        {
            view.placeholder = u8"저장소 이름이나 경로로 거르기";
            const std::vector<repository_choice> choices { make_repository_choices(tabs, settings, exists, tabs.selected().id) };
            view.entries = build_repository_selector(choices, query).entries;
            return view;
        }
        if (session == nullptr)
            return std::nullopt;
        view.placeholder = u8"브랜치 이름으로 거르기";
        view.entries = build_branch_selector(session->snapshot().refs, query, session->snapshot().summary.is_detached).entries;
        return view;
    }

    std::optional<conflict_view> build_conflict_view(const repository_session& session, std::span<const operation_status* const> operations)
    {
        const repository_snapshot& snapshot { session.snapshot() };
        const in_progress_operation& progress { snapshot.summary.in_progress };
        if (progress.kind == repository_operation_state::none)
            return std::nullopt;
        conflict_view view {};
        view.kind = progress.kind;
        const std::u8string kind_name { to_u8(repository_operation_state_name(progress.kind)) };
        view.title = kind_name + u8" 중 · 충돌 해결";
        switch (progress.kind)
        {
        case repository_operation_state::rebase:
            view.title = u8"리베이스 · 충돌 해결";
            if (progress.total > 0)
                view.progress = u8"커밋 " + number(progress.step) + u8"/" + number(progress.total) + u8" 적용 중";
            else
                view.progress = u8"rebase 진행 중";
            if (progress.current_summary.empty() == false)
                view.progress += u8": " + to_u8(progress.current_summary);
            view.detail = progress.head_name.empty() ? std::u8string {} : branch_of_head_name(progress.head_name) + u8"를 ";
            if (progress.onto.empty() == false)
                view.detail += to_u8(std::string_view { progress.onto }.substr(0, std::min<std::size_t>(progress.onto.size(), 7))) + u8" 위로 옮기는 중";
            if (progress.interactive)
            {
                // 인터랙티브 rebase는 멈춘 할 일과, 이어서 적용할 메시지가 남았는지를 알린다.
                view.todo_note = u8"할 일 " + number(progress.step) + u8"/" + number(progress.total);
                if (progress.current_action.empty() == false)
                    view.todo_note
                        += u8" · " + to_u8(progress.current_action) + u8" " + to_u8(std::string_view { progress.current_sha }.substr(0, std::min<std::size_t>(progress.current_sha.size(), 7)));
                view.todo_note += progress.cyella_messages ? std::u8string { u8" · 저장한 커밋 메시지가 이어서 적용됩니다." } : std::u8string { u8" · 커밋 메시지는 Git 설정을 따릅니다." };
            }
            break;
        case repository_operation_state::merge:
            view.title = u8"병합 · 충돌 해결";
            view.progress = u8"병합 진행 중";
            if (progress.current_summary.empty() == false)
                view.progress += u8": " + to_u8(progress.current_summary);
            break;
        case repository_operation_state::cherry_pick:
            view.title = u8"Cherry-pick · 충돌 해결";
            view.progress = u8"cherry-pick 진행 중";
            if (progress.current_summary.empty() == false)
                view.progress += u8": " + to_u8(progress.current_summary);
            break;
        case repository_operation_state::revert:
            view.title = u8"되돌리기 · 충돌 해결";
            view.progress = u8"revert 진행 중";
            if (progress.current_summary.empty() == false)
                view.progress += u8": " + to_u8(progress.current_summary);
            break;
        case repository_operation_state::none:
            break;
        }
        for (const file_change& change : snapshot.conflicts)
        {
            const file_row_view row { make_file_row(change, u8"c:") };
            view.files.push_back({ .path = change.path, .name = row.name, .folder = row.folder, .code = to_u8(change.conflict_code), .meaning = conflict_meaning(change.conflict_code) });
        }
        view.can_skip = progress.kind != repository_operation_state::merge;
        for (const operation_status* status : operations)
            if (status != nullptr && is_user_operation_status(*status) && (status->state == operation_state::queued || status->state == operation_state::running))
                view.busy = true;
        view.can_continue = view.files.empty() && view.busy == false;
        if (const operation_status* last { latest_finished_user_operation(operations) }; last != nullptr)
        {
            if (last->state == operation_state::needs_input && last->needs_input.has_value() && last->needs_input->kind == input_kind::conflict)
                view.message = view.files.empty() ? std::u8string {} : u8"아직 풀지 않은 충돌이 있습니다.";
            else if (last->state == operation_state::failed && last->failure.has_value())
                view.message = last->failure->summary;
        }
        return view;
    }

    std::vector<banner_action_view> banner_actions_for(const operation_status& status)
    {
        std::vector<banner_action_view> actions {};
        if (status.state != operation_state::failed || status.failure.has_value() == false)
            return actions;
        const operation_failure& failure { *status.failure };
        switch (status.kind)
        {
        case operation_kind::push_execute:
            if (failure.kind == failure_kind::rejected_non_ff || failure.kind == failure_kind::rejected_fetch_first)
                actions.push_back({ u8"pull_then_push", u8"Pull 후 다시 push" });
            else if (failure.kind == failure_kind::stale_info)
                actions.push_back({ u8"fetch", u8"Fetch로 원격 상태 갱신" });
            break;
        case operation_kind::pull:
            if (failure.kind == failure_kind::not_possible_to_fast_forward)
                actions.push_back({ u8"pull_rebase", u8"rebase로 Pull" });
            break;
        case operation_kind::delete_branch:
            if (failure.detail.find(u8"not fully merged") != std::u8string::npos)
                actions.push_back({ u8"force_delete", u8"강제 삭제…" });
            break;
        default:
            break;
        }
        if (failure.kind == failure_kind::index_lock)
            actions.push_back({ u8"remove_index_lock", u8"index.lock 지우기" });
        return actions;
    }

    std::shared_ptr<const drop_hint_data> build_drop_hints(const repository_snapshot& snapshot, std::shared_ptr<const graph_index> index)
    {
        auto data { std::make_shared<drop_hint_data>() };
        data->index = std::move(index);
        data->current_branch = snapshot.summary.is_detached ? std::string {} : snapshot.summary.current_branch;
        for (const ref& value : snapshot.refs)
        {
            if (value.kind != ref_kind::local && value.kind != ref_kind::remote)
                continue;
            if (value.kind == ref_kind::remote && value.name.ends_with("/HEAD"))
                continue;
            data->branches.emplace(branch_drag_key(value.kind, value.name), drop_party { .kind = value.kind, .name = value.name, .sha = value.target_sha, .is_commit = false });
        }
        return data;
    }
} // namespace cyella
