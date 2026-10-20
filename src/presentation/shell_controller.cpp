#include "presentation/shell_controller.h"

#include "application/app_messages.h"
#include "domain/repository_path.h"
#include "presentation/build_view_snapshot.h"
#include "presentation/graph_geometry.h"

#include "infrastructure/ssh_tools.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <utility>

namespace cyella {
    namespace {
        // 끝에서 이만큼 행이 남으면 다음 쪽을 읽는다.
        constexpr std::size_t load_more_threshold_rows { 200 };
        // 첫 frame 전처럼 그래프 창 높이를 모를 때의 짐작이다 (논리 픽셀).
        constexpr float assumed_graph_viewport { 800.0f };
        // 판 폭 끌기가 멈춘 뒤 설정에 쓰기까지의 간격이다.
        constexpr std::chrono::milliseconds pane_widths_save_delay { 500 };

        std::string to_text(const std::u8string_view text)
        {
            return std::string { text.begin(), text.end() };
        }

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::u8string trimmed(std::u8string_view text)
        {
            const auto space = [](const char8_t value) { return value == u8' ' || value == u8'\t' || value == u8'\r' || value == u8'\n'; };
            while (text.empty() == false && space(text.front()))
                text.remove_prefix(1);
            while (text.empty() == false && space(text.back()))
                text.remove_suffix(1);
            return std::u8string { text };
        }

        // URL의 마지막 구성 요소에서 `.git`을 뗀 폴더 이름이다 (`git@host:a/b.git` → `b`).
        std::u8string clone_folder_name(std::u8string_view url)
        {
            while (url.empty() == false && (url.back() == u8'/' || url.back() == u8'\\'))
                url.remove_suffix(1);
            const std::size_t cut { url.find_last_of(u8"/\\:") };
            std::u8string_view name { cut == std::u8string_view::npos ? url : url.substr(cut + 1) };
            if (name.ends_with(u8".git"))
                name.remove_suffix(4);
            return std::u8string { name };
        }

        void set_text(luil::text::text_edit_state& state, std::u8string text)
        {
            state = {};
            state.caret = text.size();
            state.anchor = text.size();
            state.text = std::move(text);
        }

        bool is_single_line(const text_target target) noexcept
        {
            return target != text_target::none && target != text_target::rebase_message_body;
        }

        // 한 줄 칸이라 줄바꿈과 탭은 공백 하나로 바꾼다 (붙여넣기).
        std::u8string single_line(const std::u8string_view text)
        {
            std::u8string result {};
            result.reserve(text.size());
            for (const char8_t value : text)
            {
                if (value == u8'\r')
                    continue;
                result.push_back(value == u8'\n' || value == u8'\t' ? u8' ' : value);
            }
            return result;
        }
    } // namespace

    shell_controller::shell_controller(dependencies dependencies)
        : dependencies_ { std::move(dependencies) }
        , settings_ { settings_dependencies() }
        , rebase_ { rebase_editor_controller::dependencies { dependencies_.workspace } }
        , hosting_ { hosting_dependencies() }
    {
        if (dependencies_.clock == nullptr)
            dependencies_.clock = [] { return std::chrono::system_clock::now(); };
    }

    const app_settings& shell_controller::settings() const noexcept
    {
        return dependencies_.settings != nullptr ? dependencies_.settings->settings() : memory_settings_;
    }

    void shell_controller::update_settings(const std::function<void(app_settings&)>& change)
    {
        if (dependencies_.settings != nullptr)
            static_cast<void>(dependencies_.settings->update(change));
        else
            change(memory_settings_);
    }

    settings_controller::dependencies shell_controller::settings_dependencies()
    {
        settings_controller::dependencies result {};
        result.settings = [this]() -> const app_settings& { return settings(); };
        result.update = [this](const std::function<void(app_settings&)>& change) { update_settings(change); };
        result.read_only = [this] { return dependencies_.settings != nullptr && dependencies_.settings->is_read_only(); };
        // 읽기 전용일 때는 배너가 따로 말하므로 "저장 실패"로 겹쳐 말하지 않는다.
        result.unsaved = [this] { return dependencies_.settings != nullptr && dependencies_.settings->is_read_only() == false && dependencies_.settings->has_unsaved_changes(); };
        result.probe = dependencies_.probe;
        result.git_tool = dependencies_.git_tool;
        result.workspace = dependencies_.workspace;
        // 사용자 프로필 쪽이 보이는 선택된 저장소의 신원이다.
        result.repository_profile = [this] {
            profiles_repository_inputs inputs {};
            const repository_session* const session { selected_session() };
            if (session == nullptr)
                return inputs;
            inputs.open = true;
            inputs.name = repository_display_name(session->path());
            const git_identity* const identity { selected_identity() };
            inputs.identity_text = identity != nullptr ? describe_identity_with_source(*identity) : u8"읽는 중…";
            inputs.applied_profile = applied_profile_name();
            return inputs;
        };
        result.request_ssh_info = dependencies_.request_ssh_info;
        result.copy_text = dependencies_.copy_text;
        result.ssh_directory = dependencies_.ssh_directory;
        result.default_key_comment = dependencies_.default_key_comment;
        result.ssh_tools_found = dependencies_.ssh_tools_found;
        result.ssh_tools_origin = dependencies_.ssh_tools_origin;
        result.hosting_rows = [this](const std::vector<hosting_host_setting>& configured) { return hosting_.host_rows(configured); };
        result.hosting_sign_out = [this](const std::string& host_key) { hosting_.sign_out(host_key); };
        return result;
    }

    void shell_controller::save_tabs()
    {
        update_settings([this](app_settings& value) { capture_open_tabs(tabs_, value); });
    }

    std::string shell_controller::selected_repository_id() const
    {
        const auto found { tab_repositories_.find(tabs_.selected().id) };
        return found == tab_repositories_.end() ? std::string {} : found->second;
    }

    void shell_controller::start(const std::u8string& initial_repository)
    {
        const pane_widths_settings& saved_widths { settings().ui.pane_widths };
        ui_.sidebar_width = clamp_sidebar_width(saved_widths.sidebar.value_or(ui_.sidebar_width));
        ui_.detail_width = clamp_detail_width(saved_widths.detail.value_or(ui_.detail_width));
        if (dependencies_.workspace != nullptr)
        {
            tabs_ = restore_tabs(settings());
            for (const repository_tab& tab : tabs_.tabs())
            {
                if (tab.kind != repository_tab_kind::repository)
                    continue;
                tab_repositories_[tab.id] = dependencies_.workspace->open_repository(tab.path);
                ui_.tabs[tab.id];
            }
        }
        if (initial_repository.empty() == false)
            open_repository(initial_repository);
    }

    tab_ui_state* shell_controller::selected_tab_state()
    {
        const repository_tab& tab { tabs_.selected() };
        if (tab.kind != repository_tab_kind::repository)
            return nullptr;
        return &ui_.tabs[tab.id];
    }

    repository_session* shell_controller::selected_session()
    {
        if (dependencies_.workspace == nullptr)
            return nullptr;
        const std::string id { selected_repository_id() };
        return id.empty() ? nullptr : dependencies_.workspace->find_session(id);
    }

    operation_id shell_controller::submit(git_operation operation)
    {
        const std::string id { selected_repository_id() };
        if (dependencies_.workspace == nullptr || id.empty())
            return 0;
        return dependencies_.workspace->submit(id, std::move(operation));
    }

    void shell_controller::show_notice(std::u8string text, const bool warning)
    {
        ui_.notice = notice_state { std::move(text), warning };
    }

    void shell_controller::open_repository(const std::u8string& path)
    {
        if (dependencies_.workspace == nullptr)
        {
            show_notice(dependencies_.git_message, true);
            return;
        }
        if (path.empty())
            return;
        const open_repository_result result { tabs_.open_repository(path) };
        const std::optional<std::size_t> index { tabs_.index_of(result.id) };
        if (index.has_value() == false)
            return;
        const repository_tab& tab { tabs_.tabs()[*index] };
        if (result.created)
        {
            tab_repositories_[tab.id] = dependencies_.workspace->open_repository(tab.path);
            ui_.tabs[tab.id];
        }
        const std::u8string opened_path { tab.path };
        const std::u8string opened_at { format_utc_timestamp(dependencies_.clock()) };
        update_settings([this, &opened_path, &opened_at](app_settings& value) {
            touch_recent_repository(value, opened_path, opened_at);
            capture_open_tabs(tabs_, value);
        });
        ui_.notice.reset();
        ui_.rail_section.reset();
        ui_.drawer_open = false;
        exists_cache_.clear();
    }

    void shell_controller::close_tab(const repository_tab_id id)
    {
        const std::optional<std::size_t> index { tabs_.index_of(id) };
        if (index.has_value() == false)
            return;
        std::string repository {};
        if (const auto found { tab_repositories_.find(id) }; found != tab_repositories_.end())
            repository = found->second;
        if (tabs_.close(*index) == false)
            return;
        tab_repositories_.erase(id);
        ui_.tabs.erase(id);
        if (rebase_.state().has_value() && rebase_.state()->repository_id == repository)
            rebase_.close();
        if (repository.empty() == false)
            hosting_.on_repository_closed(repository);
        const bool still_open { std::any_of(tab_repositories_.begin(), tab_repositories_.end(), [&repository](const auto& entry) { return entry.second == repository; }) };
        if (dependencies_.workspace != nullptr && repository.empty() == false && still_open == false)
        {
            // 탭을 닫으면 그 저장소의 남은 작업도 거둔다. 작업을 취소하고 닫기 전의 확인 상자는 아직 없다.
            for (const operation_id operation : dependencies_.workspace->operation_order())
            {
                const operation_status* const status { dependencies_.workspace->find_operation(operation) };
                if (status != nullptr && status->repository_id == repository && status->finished() == false)
                    static_cast<void>(dependencies_.workspace->cancel(operation));
            }
            static_cast<void>(dependencies_.workspace->close_repository(repository));
            graph_cache_.erase(repository);
            history_cache_.erase(repository);
        }
        save_tabs();
        ui_.notice.reset();
        exists_cache_.clear();
    }

    void shell_controller::select_tab_index(const std::size_t index)
    {
        if (tabs_.select(index) == false)
            return;
        save_tabs();
        ui_.notice.reset();
        ui_.rail_section.reset();
        ui_.drawer_open = false;
        ui_.selector.reset();
        exists_cache_.clear();
    }

    luil::text::text_edit_state* shell_controller::draft(const text_target target)
    {
        switch (target)
        {
        case text_target::start_search:
            return &ui_.start_search;
        case text_target::sidebar_filter:
            if (tab_ui_state* const tab { selected_tab_state() }; tab != nullptr)
                return &tab->sidebar_filter;
            return nullptr;
        case text_target::commit_subject:
            if (tab_ui_state* const tab { selected_tab_state() }; tab != nullptr)
                return &tab->commit_subject;
            return nullptr;
        case text_target::commit_body:
            if (tab_ui_state* const tab { selected_tab_state() }; tab != nullptr)
                return &tab->commit_body;
            return nullptr;
        case text_target::clone_url:
            return ui_.clone.has_value() ? &ui_.clone->url : nullptr;
        case text_target::clone_path:
            return ui_.clone.has_value() ? &ui_.clone->path : nullptr;
        case text_target::settings_git_path:
        case text_target::settings_tool_extensions:
        case text_target::settings_tool_executable:
        case text_target::settings_tool_arguments:
        case text_target::settings_key_name:
        case text_target::settings_key_comment:
        case text_target::settings_test_host:
        case text_target::settings_hosting_host:
        case text_target::settings_hosting_api_base:
        case text_target::settings_shortcut_input:
        case text_target::settings_profile_name:
        case text_target::settings_profile_user_name:
        case text_target::settings_profile_user_email:
        case text_target::settings_profile_signing_key:
        case text_target::settings_rule_prefix:
            return settings_.draft(target);
        case text_target::palette_filter:
            return ui_.palette.has_value() ? &ui_.palette->filter : nullptr;
        case text_target::rebase_message_subject:
        case text_target::rebase_message_body:
            return rebase_.draft(target);
        case text_target::selector_filter:
            return ui_.selector.has_value() ? &ui_.selector->filter : nullptr;
        case text_target::form_field_0:
        case text_target::form_field_1:
        case text_target::form_field_2:
            // 떠 있는 입력 상자가 없으면 늦게 온 편집은 버린다. 상자가 바뀌었으면 앞 상자의 글이 새 상자로 새지 않게 먼저 비운다.
            sync_form_inputs();
            if (current_form_source().kind == form_source_kind::none)
                return nullptr;
            return &ui_.form.fields[static_cast<std::size_t>(static_cast<std::uint32_t>(target) - static_cast<std::uint32_t>(text_target::form_field_0))];
        case text_target::none:
            break;
        }
        return nullptr;
    }

    void shell_controller::handle_text_edit(const luil::text_edit_request& request)
    {
        // 인터랙티브 rebase 편집기의 메시지 칸은 편집기가 직접 적용한다 (본문은 여러 줄이다).
        if (rebase_.edit_text(request))
            return;
        const text_target target { from_luil_target(request.target) };
        luil::text::text_edit_state* const state { draft(target) };
        if (state == nullptr)
            return;
        luil::text_insert_filter filter {};
        if (is_single_line(target))
            filter = [](const std::u8string_view text) { return single_line(text); };
        luil::apply_text_edit(*state, request, filter);
        if (target == text_target::start_search)
            ui_.start_scroll = 0.0f;
        else if (target == text_target::sidebar_filter)
        {
            if (tab_ui_state* const tab { selected_tab_state() }; tab != nullptr)
                tab->sidebar_scroll = 0.0f;
        }
        else if (target == text_target::palette_filter && ui_.palette.has_value())
            // 거르는 글이 바뀌면 커서는 맨 위(첫 활성 항목)로 돌아간다.
            ui_.palette->cursor.clear();
    }

    void shell_controller::handle_toolbar(const toolbar_action action)
    {
        repository_session* const session { selected_session() };
        if (session == nullptr)
            return;
        const repository_summary& summary { session->snapshot().summary };
        switch (action)
        {
        case toolbar_action::fetch:
            static_cast<void>(submit(fetch_request {}));
            return;
        case toolbar_action::pull:
            static_cast<void>(submit(pull_request { pull_strategy::ff_only }));
            return;
        case toolbar_action::push:
            if (summary.has_upstream)
                static_cast<void>(submit(push_execute_request {}));
            else
                open_push_dialog(summary.current_branch, true);
            return;
        case toolbar_action::branch:
            open_create_branch_dialog({});
            return;
        case toolbar_action::stash:
            static_cast<void>(submit(stash_push_request { .message = {}, .include_untracked = true }));
            return;
        case toolbar_action::pop:
            static_cast<void>(submit(stash_pop_request { 0 }));
            return;
        }
    }

    void shell_controller::maybe_load_more()
    {
        repository_session* const session { selected_session() };
        tab_ui_state* const tab { selected_tab_state() };
        if (session == nullptr || tab == nullptr || session->loaded() == false || session->history_complete() || session->log_page_loading())
            return;
        const float viewport { feedback_.graph_viewport > 0.0f ? feedback_.graph_viewport : assumed_graph_viewport };
        const auto last_visible { static_cast<std::size_t>((tab->graph_scroll + viewport) / graph_row_height) };
        if (last_visible + load_more_threshold_rows < session->graph_rows().size())
            return;
        static_cast<void>(submit(load_more_log_request {}));
    }

    void shell_controller::handle_scroll(const scroll_intent& intent)
    {
        tab_ui_state* const tab { selected_tab_state() };
        float* value { nullptr };
        std::optional<float> maximum {};
        switch (intent.target)
        {
        case scroll_target::graph:
            value = tab != nullptr ? &tab->graph_scroll : nullptr;
            maximum = feedback_.graph_maximum;
            break;
        case scroll_target::sidebar:
            value = tab != nullptr ? &tab->sidebar_scroll : nullptr;
            maximum = feedback_.sidebar_maximum;
            break;
        case scroll_target::detail:
            value = tab != nullptr ? &tab->detail_scroll : nullptr;
            maximum = feedback_.detail_maximum;
            break;
        case scroll_target::diff:
            value = tab != nullptr ? &tab->diff_scroll : nullptr;
            maximum = feedback_.diff_maximum;
            break;
        case scroll_target::start_page:
            value = &ui_.start_scroll;
            maximum = feedback_.start_maximum;
            break;
        case scroll_target::operation_log:
            value = &ui_.log_scroll;
            maximum = feedback_.log_maximum;
            break;
        }
        if (value == nullptr)
            return;
        float next { intent.absolute ? intent.value : *value + intent.value };
        if (maximum.has_value())
            next = std::min(next, *maximum);
        *value = std::max(next, 0.0f);
        if (intent.target == scroll_target::graph)
            maybe_load_more();
    }

    void shell_controller::handle_select_commit(const select_commit_intent& intent)
    {
        repository_session* const session { selected_session() };
        tab_ui_state* const tab { selected_tab_state() };
        if (session == nullptr || tab == nullptr)
            return;
        const std::string key { to_text(intent.key) };
        const std::vector<commit_info>& commits { session->graph_commits() };
        const auto found { std::find_if(commits.begin(), commits.end(), [&key](const commit_info& commit) { return commit.sha.compare(0, key.size(), key) == 0; }) };
        if (found == commits.end())
            return;
        // 커밋을 고르면 열려 있던 PR 상세는 닫힌다 (이미 고른 커밋을 다시 눌러도 커밋 상세로 돌아온다).
        hosting_.clear_selection(session->id());
        if (tab->selected_sha == found->sha)
            return;
        tab->selected_sha = found->sha;
        tab->diff.reset();
        tab->detail_scroll = 0.0f;
        // 로그인한 호스트면 이 커밋의 CI 상태를 읽는다 (조용한 로그인은 하지 않는다).
        if (found->sha != graph_wip_sha)
        {
            static_cast<void>(submit(show_commit_request { found->sha }));
            hosting_.on_commit_selected(session->id(), found->sha);
        }
        // medium·narrow에서는 상세가 서랍이라 고르면 연다.
        if (last_mode_ != layout_mode::wide)
            ui_.drawer_open = true;
    }

    void shell_controller::handle_activate_ref(const activate_ref_intent& intent)
    {
        repository_session* const session { selected_session() };
        if (session == nullptr)
            return;
        switch (intent.kind)
        {
        case ref_kind::local:
            if (intent.name != session->snapshot().summary.current_branch)
            {
                // 다른 worktree가 쓰는 브랜치는 git도 거절한다. 같은 이유를 먼저 알리고 operation은 내지 않는다.
                const auto found { std::ranges::find_if(session->snapshot().refs, [&intent](const ref& value) { return value.kind == ref_kind::local && value.name == intent.name; }) };
                if (found != session->snapshot().refs.end() && found->worktree_path.empty() == false)
                {
                    show_notice(u8"'" + std::u8string { intent.name.begin(), intent.name.end() } + u8"' 브랜치는 다른 worktree("
                            + std::u8string { found->worktree_path.begin(), found->worktree_path.end() } + u8")에서 체크아웃되어 있어 옮길 수 없습니다. 사이드바 WORKTREES에서 그 worktree를 여세요.",
                        true);
                    return;
                }
                static_cast<void>(submit(checkout_branch_request { intent.name }));
            }
            return;
        case ref_kind::remote:
            static_cast<void>(submit(checkout_remote_branch_request { intent.name }));
            return;
        case ref_kind::tag:
            request_detached_checkout(intent.name, to_u8(intent.name));
            return;
        case ref_kind::head:
            return;
        case ref_kind::stash:
            // 두 번 누름은 stash를 남겨 두는 apply다. 지우는 pop·삭제는 우클릭 메뉴에 있다.
            for (const stash_entry& stash : session->snapshot().stashes)
                if (stash.name == intent.name)
                {
                    static_cast<void>(submit(stash_apply_request { stash.index }));
                    return;
                }
            return;
        }
    }

    void shell_controller::handle_commit()
    {
        tab_ui_state* const tab { selected_tab_state() };
        if (tab == nullptr || tab->pending_commit != 0 || trimmed(tab->commit_subject.text).empty())
            return;
        // 작성자 신원이 없거나 규칙의 프로필과 다르면 저장소마다 한 번 경고하고 커밋하지 않는다.
        if (warn_before_commit())
            return;
        commit_now();
    }

    void shell_controller::commit_now()
    {
        tab_ui_state* const tab { selected_tab_state() };
        if (tab == nullptr || tab->pending_commit != 0)
            return;
        const std::u8string subject { trimmed(tab->commit_subject.text) };
        const std::u8string body { trimmed(tab->commit_body.text) };
        if (subject.empty())
            return;
        std::u8string message { subject };
        if (body.empty() == false)
            message += u8"\n\n" + body;
        tab->pending_commit = submit(commit_request { .message = to_text(message), .amend = tab->amend, .stage_all = false });
    }

    void shell_controller::handle_folder(const folder_chosen_intent& intent)
    {
        // 선택 상자의 "폴더에서 저장소 열기…"가 이 대화 상자를 열었다. 고르고 나면 목록은 닫는다 (팔레트의 같은 항목도 마찬가지다).
        ui_.selector.reset();
        ui_.palette.reset();
        if (dependencies_.workspace == nullptr)
        {
            show_notice(dependencies_.git_message, true);
            return;
        }
        switch (intent.purpose)
        {
        case folder_purpose::open: {
            const operation_id id { dependencies_.workspace->submit_setup(inspect_path_request { intent.path }) };
            if (id != 0)
                pending_inspections_[id] = intent.path;
            return;
        }
        case folder_purpose::init: {
            const operation_id id { dependencies_.workspace->submit_setup(init_request { .target_path = intent.path, .initial_branch = {} }) };
            if (id != 0)
                pending_setup_[id] = intent.path;
            return;
        }
        case folder_purpose::locate: {
            // 고른 폴더가 저장소 루트(또는 그 안쪽)인지 확인한 뒤에 최근 항목의 경로를 바꾼다.
            const operation_id id { dependencies_.workspace->submit_setup(inspect_path_request { intent.path }) };
            if (id != 0)
                pending_locates_[id] = locate_request { intent.origin, intent.path };
            return;
        }
        case folder_purpose::clone_target:
            if (ui_.clone.has_value())
            {
                std::u8string path { intent.path };
                const std::u8string name { clone_folder_name(ui_.clone->url.text) };
                if (name.empty() == false)
                    path += (path.ends_with(u8"\\") ? u8"" : u8"\\") + name;
                set_text(ui_.clone->path, std::move(path));
            }
            return;
        }
    }

    bool shell_controller::handle(const luil::app_message& message)
    {
        if (const auto* const intent { message.get<select_tab_intent>() }; intent != nullptr)
        {
            if (const std::optional<std::size_t> index { tabs_.index_of(intent->id) }; index.has_value())
                select_tab_index(*index);
            return true;
        }
        if (const auto* const intent { message.get<close_tab_intent>() }; intent != nullptr)
        {
            close_tab(intent->id);
            return true;
        }
        if (const auto* const intent { message.get<reorder_tab_intent>() }; intent != nullptr)
        {
            const std::optional<std::size_t> from { tabs_.index_of(intent->moved) };
            const std::optional<std::size_t> to { tabs_.index_of(intent->target) };
            if (from.has_value() && to.has_value() && tabs_.reorder(*from, *to))
                save_tabs();
            return true;
        }
        if (message.get<new_tab_intent>() != nullptr)
        {
            static_cast<void>(tabs_.new_tab());
            save_tabs();
            ui_.notice.reset();
            exists_cache_.clear();
            return true;
        }
        if (const auto* const intent { message.get<cycle_tab_intent>() }; intent != nullptr)
        {
            const std::size_t count { tabs_.tabs().size() };
            if (count > 1)
                select_tab_index((tabs_.selected_index() + (intent->forward ? 1 : count - 1)) % count);
            return true;
        }
        if (message.get<close_selected_tab_intent>() != nullptr)
        {
            close_tab(tabs_.selected().id);
            return true;
        }
        if (message.get<refresh_intent>() != nullptr)
        {
            static_cast<void>(submit(refresh_request {}));
            return true;
        }
        if (const auto* const intent { message.get<scroll_tabs_intent>() }; intent != nullptr)
        {
            ui_.tab_scroll = std::max(ui_.tab_scroll + intent->delta, 0.0f);
            return true;
        }
        if (message.get<cycle_theme_intent>() != nullptr)
        {
            update_settings([](app_settings& value) {
                switch (value.appearance.theme)
                {
                case theme_preference::system:
                    value.appearance.theme = theme_preference::light;
                    break;
                case theme_preference::light:
                    value.appearance.theme = theme_preference::dark;
                    break;
                case theme_preference::dark:
                    value.appearance.theme = theme_preference::system;
                    break;
                }
            });
            return true;
        }
        if (const auto* const intent { message.get<show_notice_intent>() }; intent != nullptr)
        {
            show_notice(intent->text, intent->warning);
            return true;
        }
        if (message.get<dismiss_notice_intent>() != nullptr)
        {
            ui_.notice.reset();
            return true;
        }
        if (message.get<toggle_operation_log_intent>() != nullptr)
        {
            ui_.operation_log_open = ui_.operation_log_open == false;
            return true;
        }
        if (message.get<toggle_banner_details_intent>() != nullptr)
        {
            if (tab_ui_state* const tab { selected_tab_state() }; tab != nullptr)
                tab->banner_expanded = tab->banner_expanded == false;
            return true;
        }
        if (const auto* const intent { message.get<dismiss_banner_intent>() }; intent != nullptr)
        {
            if (tab_ui_state* const tab { selected_tab_state() }; tab != nullptr)
            {
                tab->dismissed_banner = std::max(tab->dismissed_banner, intent->id);
                tab->banner_expanded = false;
            }
            else
                ui_.dismissed_start_banner = std::max(ui_.dismissed_start_banner, intent->id);
            return true;
        }
        if (const auto* const intent { message.get<toolbar_intent>() }; intent != nullptr)
        {
            handle_toolbar(intent->action);
            return true;
        }
        if (const auto* const intent { message.get<resize_pane_intent>() }; intent != nullptr)
        {
            if (intent->sidebar)
                ui_.sidebar_width = clamp_sidebar_width(ui_.sidebar_width + intent->delta);
            else
                ui_.detail_width = clamp_detail_width(ui_.detail_width + intent->delta);
            pane_widths_flush_at_ = std::chrono::steady_clock::now() + pane_widths_save_delay;
            return true;
        }
        if (const auto* const intent { message.get<toggle_drawer_intent>() }; intent != nullptr)
        {
            ui_.drawer_open = intent->open;
            return true;
        }
        if (const auto* const intent { message.get<open_rail_section_intent>() }; intent != nullptr)
        {
            if (ui_.rail_section == intent->section)
                ui_.rail_section.reset();
            else
                ui_.rail_section = intent->section;
            return true;
        }
        if (message.get<close_overlays_intent>() != nullptr)
        {
            // Esc는 가장 위에 떠 있는 것 하나만 닫는다.
            tab_ui_state* const tab { selected_tab_state() };
            if (ui_.palette.has_value())
                ui_.palette.reset();
            else if (ui_.selector.has_value())
                ui_.selector.reset();
            else if (ui_.rail_section.has_value())
                ui_.rail_section.reset();
            else if (ui_.drawer_open)
                ui_.drawer_open = false;
            else if (tab != nullptr && tab->diff.has_value())
                tab->diff.reset();
            else
                ui_.notice.reset();
            return true;
        }
        if (const auto* const intent { message.get<open_recent_intent>() }; intent != nullptr)
        {
            if (repository_exists(intent->path))
                open_repository(intent->path);
            else
                show_notice(u8"'" + intent->path + u8"' 경로를 찾을 수 없습니다. 항목의 위치 찾기로 옮긴 폴더를 고르거나 목록에서 지울 수 있습니다.", true);
            return true;
        }
        if (const auto* const intent { message.get<pin_recent_intent>() }; intent != nullptr)
        {
            update_settings([intent](app_settings& value) { static_cast<void>(set_recent_repository_pinned(value, intent->path, intent->pinned)); });
            return true;
        }
        if (const auto* const intent { message.get<remove_recent_intent>() }; intent != nullptr)
        {
            update_settings([intent](app_settings& value) { static_cast<void>(remove_recent_repository(value, intent->path)); });
            exists_cache_.erase(intent->path);
            return true;
        }
        if (message.get<open_clone_dialog_intent>() != nullptr)
        {
            if (ui_.clone.has_value() == false)
                ui_.clone = clone_dialog_state {};
            return true;
        }
        if (message.get<close_clone_dialog_intent>() != nullptr)
        {
            // 닫아도 clone은 계속된다. 끝나면 새 탭이 열린다.
            ui_.clone.reset();
            return true;
        }
        if (message.get<submit_clone_intent>() != nullptr)
        {
            if (ui_.clone.has_value() && dependencies_.workspace != nullptr)
            {
                const std::u8string url { trimmed(ui_.clone->url.text) };
                const std::u8string path { trimmed(ui_.clone->path.text) };
                if (url.empty() == false && path.empty() == false)
                {
                    ui_.clone->error.clear();
                    ui_.clone->operation = dependencies_.workspace->submit_setup(clone_request { .url = to_text(url), .target_path = path });
                    if (ui_.clone->operation != 0)
                        pending_setup_[ui_.clone->operation] = path;
                }
            }
            return true;
        }
        if (const auto* const intent { message.get<scroll_intent>() }; intent != nullptr)
        {
            handle_scroll(*intent);
            return true;
        }
        if (const auto* const intent { message.get<toggle_sidebar_section_intent>() }; intent != nullptr)
        {
            if (tab_ui_state* const tab { selected_tab_state() }; tab != nullptr)
                tab->collapsed_sections ^= static_cast<std::uint8_t>(1u << static_cast<unsigned>(intent->section));
            return true;
        }
        if (const auto* const intent { message.get<select_sidebar_item_intent>() }; intent != nullptr)
        {
            if (tab_ui_state* const tab { selected_tab_state() }; tab != nullptr)
            {
                tab->selected_sidebar_key = intent->key;
                if (intent->target_sha.empty() == false)
                    handle_select_commit(select_commit_intent { graph_item_key(intent->target_sha) });
            }
            return true;
        }
        if (const auto* const intent { message.get<activate_ref_intent>() }; intent != nullptr)
        {
            handle_activate_ref(*intent);
            return true;
        }
        if (const auto* const intent { message.get<select_commit_intent>() }; intent != nullptr)
        {
            handle_select_commit(*intent);
            return true;
        }
        if (const auto* const intent { message.get<select_file_intent>() }; intent != nullptr)
        {
            if (tab_ui_state* const tab { selected_tab_state() }; tab != nullptr)
            {
                // 문맥 줄 수는 환경 설정이 정한다. 행이 만든 요청에는 기본값이 들어 있다.
                diff_file_request request { intent->request };
                request.context_lines = settings().diff.context_lines;
                tab->diff = request;
                tab->diff_scroll = 0.0f;
                static_cast<void>(submit(std::move(request)));
            }
            return true;
        }
        if (message.get<close_diff_intent>() != nullptr)
        {
            if (tab_ui_state* const tab { selected_tab_state() }; tab != nullptr)
                tab->diff.reset();
            return true;
        }
        if (message.get<toggle_amend_intent>() != nullptr)
        {
            tab_ui_state* const tab { selected_tab_state() };
            repository_session* const session { selected_session() };
            if (tab != nullptr)
            {
                tab->amend = tab->amend == false;
                // amend를 켜면 HEAD 커밋의 제목을 채운다 (비어 있을 때만).
                if (tab->amend && tab->commit_subject.text.empty() && session != nullptr)
                    for (const commit_info& commit : session->graph_commits())
                        if (commit.sha == session->snapshot().summary.head_sha)
                        {
                            set_text(tab->commit_subject, to_u8(commit.subject));
                            break;
                        }
            }
            return true;
        }
        if (message.get<commit_intent>() != nullptr)
        {
            handle_commit();
            return true;
        }
        if (const auto* const intent { message.get<edit_text_intent>() }; intent != nullptr)
        {
            handle_text_edit(intent->request);
            return true;
        }
        if (const auto* const intent { message.get<compose_text_intent>() }; intent != nullptr)
        {
            if (intent->event.composing)
                ui_.composition = intent->event;
            else
                ui_.composition.reset();
            return true;
        }
        if (const auto* const intent { message.get<folder_chosen_intent>() }; intent != nullptr)
        {
            handle_folder(*intent);
            return true;
        }
        // 툴바 저장소·브랜치 선택 상자의 목록이다.
        if (handle_selector_message(message))
            return true;
        // 명령 팔레트와 레지스트리 명령이다.
        if (handle_command_message(message))
            return true;
        // 환경 설정 화면의 메시지다. 화면이 닫혀 있으면 늦게 온 것이라 삼킨다.
        if (settings_.handle(message))
            return true;
        // submodule·worktree·LFS와 인터랙티브 rebase 편집기의 메시지다.
        if (handle_extension_message(message) || handle_rebase_entry(message) || rebase_.handle(message) || hosting_.handle(message))
            return true;
        return handle_dialog_message(message);
    }

    void shell_controller::after_events()
    {
        git_workspace* const workspace { dependencies_.workspace };
        if (workspace == nullptr)
            return;
        hosting_.after_events();

        for (auto it { pending_inspections_.begin() }; it != pending_inspections_.end();)
        {
            const operation_status* const status { workspace->find_operation(it->first) };
            if (status != nullptr && status->finished() == false)
            {
                ++it;
                continue;
            }
            const std::u8string path { it->second };
            it = pending_inspections_.erase(it);
            if (status == nullptr || status->state != operation_state::succeeded)
                continue;
            const std::optional<path_inspected> inspected { workspace->inspection(path) };
            if (inspected.has_value() == false)
                continue;
            switch (inspected->kind)
            {
            case path_inspection_kind::repository:
            case path_inspection_kind::inside_repository:
                open_repository(inspected->root.empty() ? path : inspected->root);
                break;
            case path_inspection_kind::directory:
                show_notice(u8"'" + path + u8"'은(는) Git 저장소가 아닙니다. 시작 페이지의 Init으로 여기에 저장소를 만들 수 있습니다.", true);
                break;
            case path_inspection_kind::missing:
                show_notice(u8"'" + path + u8"' 경로가 없습니다.", true);
                break;
            }
        }

        for (auto it { pending_locates_.begin() }; it != pending_locates_.end();)
        {
            const operation_status* const status { workspace->find_operation(it->first) };
            if (status != nullptr && status->finished() == false)
            {
                ++it;
                continue;
            }
            const locate_request request { it->second };
            it = pending_locates_.erase(it);
            if (status == nullptr || status->state != operation_state::succeeded)
                continue;
            const std::optional<path_inspected> inspected { workspace->inspection(request.chosen) };
            if (inspected.has_value() == false)
                continue;
            switch (inspected->kind)
            {
            case path_inspection_kind::repository:
            case path_inspection_kind::inside_repository: {
                // 저장소 안쪽 폴더를 골랐으면 그 루트로 잇는다.
                const std::u8string root { inspected->root.empty() ? request.chosen : inspected->root };
                bool relocated { false };
                update_settings([&](app_settings& value) { relocated = relocate_recent_repository(value, request.origin, root); });
                exists_cache_.erase(request.origin);
                exists_cache_.erase(root);
                if (relocated)
                    show_notice(u8"'" + root + u8"'(으)로 위치를 바꿨습니다.", false);
                else
                    show_notice(u8"최근 목록에서 '" + request.origin + u8"' 항목을 찾지 못했습니다.", true);
                break;
            }
            case path_inspection_kind::directory:
                show_notice(u8"'" + request.chosen + u8"'은(는) Git 저장소가 아니라서 위치를 바꾸지 않았습니다.", true);
                break;
            case path_inspection_kind::missing:
                show_notice(u8"'" + request.chosen + u8"' 경로가 없습니다.", true);
                break;
            }
        }

        for (auto it { pending_setup_.begin() }; it != pending_setup_.end();)
        {
            const operation_status* const status { workspace->find_operation(it->first) };
            if (status != nullptr && status->finished() == false)
            {
                ++it;
                continue;
            }
            const operation_id id { it->first };
            const std::u8string path { it->second };
            it = pending_setup_.erase(it);
            if (status == nullptr || status->state != operation_state::succeeded)
                continue;
            if (ui_.clone.has_value() && ui_.clone->operation == id)
                ui_.clone.reset();
            open_repository(path);
        }

        for (auto& [id, tab] : ui_.tabs)
        {
            static_cast<void>(id);
            if (tab.pending_commit == 0)
                continue;
            const operation_status* const status { workspace->find_operation(tab.pending_commit) };
            if (status != nullptr && status->finished() == false)
                continue;
            if (status != nullptr && status->state == operation_state::succeeded)
            {
                tab.commit_subject = {};
                tab.commit_body = {};
                tab.amend = false;
            }
            tab.pending_commit = 0;
        }
        settings_.after_events();
        rebase_.after_events();
        process_chains();
        cancel_automatic_prompts();
        // "이번만 연결"로 다시 낸 작업이 성공하면 같은 임시 신뢰로 원격 태그 표시를 한 번 갱신한다. 쓰기 뒤 자동으로 나가는
        // 갱신에는 임시 known_hosts가 이미 지워져 있어 같은 호스트를 다시 묻게 되는데 (위에서 조용히 취소한다), 사용자가 이미 이
        // 연결을 믿기로 했으므로 그 몫을 여기서 한 번 채운다.
        for (auto it { trusted_retries_.begin() }; it != trusted_retries_.end();)
        {
            const operation_status* const status { workspace->find_operation(it->first) };
            if (status != nullptr && status->finished() == false)
            {
                ++it;
                continue;
            }
            if (status != nullptr && status->state == operation_state::succeeded)
            {
                temporary_ssh_trust trust { it->second.trust };
                std::error_code error {};
                const std::filesystem::path directory { std::filesystem::path { trust.known_hosts_path }.parent_path() };
                trust.known_hosts_path = (directory / ("cyella-known-hosts-follow-" + std::to_string(++temporary_counter_) + ".tmp")).u8string();
                trust.command = make_temporary_known_hosts_ssh_command(
                    dependencies_.ssh_executable.empty() ? std::u8string_view { u8"ssh" } : std::u8string_view { dependencies_.ssh_executable }, std::filesystem::path { trust.known_hosts_path });
                static_cast<void>(workspace->submit(it->second.repository_id, refresh_remote_tags_request { .remotes = {}, .ssh_command_override = trust }, true));
            }
            it = trusted_retries_.erase(it);
        }
        maybe_load_more();
        // 선택된 저장소의 작성자 신원을 읽고, 규칙에 맞는 프로필이 있으면 제안한다.
        sync_identity();
    }

    std::shared_ptr<const std::vector<luil::virtual_list_item>> shell_controller::graph_items(const repository_session& session)
    {
        graph_cache_entry& entry { graph_cache_[session.id()] };
        const std::vector<commit_info>& commits { session.graph_commits() };
        const bool same_generation { entry.items != nullptr && entry.generation == session.displayed_generation() };
        if (same_generation && entry.size == commits.size())
            return entry.items;
        // 같은 generation에서 행이 늘기만 했으면 앞부분을 그대로 두고 뒤만 잇는다. 레이아웃은 이미 내보낸
        // 행을 바꾸지 않는다.
        auto items { std::make_shared<std::vector<luil::virtual_list_item>>() };
        if (same_generation && entry.size < commits.size())
        {
            items->reserve(commits.size());
            *items = *entry.items;
            for (std::size_t index { entry.size }; index < commits.size(); ++index)
                items->push_back({ .key = graph_item_key(commits[index].sha) });
        }
        else
            *items = build_graph_items(commits);
        entry.generation = session.displayed_generation();
        entry.size = commits.size();
        entry.items = std::move(items);
        return entry.items;
    }

    bool shell_controller::repository_exists(const std::u8string_view path)
    {
        if (dependencies_.probe == nullptr)
            return true;
        const std::u8string key { path.begin(), path.end() };
        if (const auto found { exists_cache_.find(key) }; found != exists_cache_.end())
            return found->second;
        const bool exists { dependencies_.probe->probe(path) == path_kind::directory };
        exists_cache_[key] = exists;
        return exists;
    }

    view_snapshot shell_controller::make_view(const float width, const float height, const float scale)
    {
        last_scale_ = scale;
        sync_form_inputs();
        view_inputs inputs {};
        inputs.width = width;
        inputs.height = height;
        inputs.scale = scale;
        inputs.tabs = &tabs_;
        inputs.settings = &settings();
        inputs.ui = &ui_;
        inputs.tab_repositories = &tab_repositories_;
        inputs.git_message = dependencies_.git_message;
        inputs.git_version = dependencies_.git_version;
        inputs.now = dependencies_.clock();
        inputs.exists = [this](const std::u8string_view path) { return repository_exists(path); };
        inputs.hosting_view = [this](const std::string& repository_id) { return hosting_.view_of(repository_id); };
        inputs.commit_ci = [this](const std::string& repository_id, const std::string& sha) { return hosting_.ci_mark(repository_id, sha); };
        if (git_workspace* const workspace { dependencies_.workspace }; workspace != nullptr)
        {
            inputs.find_session = [workspace](const std::string& id) -> const repository_session* { return workspace->find_session(id); };
            for (const operation_id id : workspace->operation_order())
                if (const operation_status* const status { workspace->find_operation(id) }; status != nullptr)
                    inputs.operations.push_back(status);
            inputs.graph_items = [this](const repository_session& session) { return graph_items(session); };
            inputs.history_index = [this](const repository_session& session) { return history_index(session); };
        }
        view_snapshot view { build_view_snapshot(inputs) };
        view.settings = settings_.make_view(ui_.composition);
        view.rebase_editor = rebase_.make_view(ui_.composition);
        last_mode_ = view.mode;
        view.palette = make_palette_view();
        view.shortcuts = resolve_shortcut_bindings(settings());
        // 선택된 저장소의 작성자 신원을 상태 줄에 보인다 (출처와 함께).
        if (view.repository.has_value() && dependencies_.request_identity != nullptr)
        {
            status_view& status { view.status };
            status.identity_visible = true;
            if (const git_identity* const identity { selected_identity() }; identity != nullptr)
            {
                status.identity_text = u8"작성자: " + describe_identity_with_source(*identity);
                status.identity_warning = identity->complete() == false;
                status.identity_tooltip = u8"이 저장소가 커밋에 쓰는 작성자와 그 값이 든 설정 파일이다. 눌러서 프로필을 적용한다.";
            }
            else if (const auto entry { identities_.find(selected_repository_id()) }; entry != identities_.end() && entry->second.requested && entry->second.error.empty() == false)
            {
                // 읽지 못했다 (git이 오래되어 --show-scope를 모르거나 일시적인 실패). 15초 뒤 다시 읽는다.
                status.identity_text = u8"작성자: 읽지 못함 · " + entry->second.error;
                status.identity_warning = true;
                status.identity_tooltip = u8"저장소의 user.name·user.email을 읽지 못했다. 잠시 뒤 다시 읽는다.";
            }
            else
            {
                status.identity_text = u8"작성자: 읽는 중…";
                status.identity_tooltip = u8"저장소의 user.name·user.email을 읽고 있다.";
            }
        }
        // 상세 서랍은 medium·narrow에서 커밋을 고르면 열린다. 고른 것이 없으면 닫는다.
        if (view.repository.has_value() && view.repository->detail == detail_mode::none)
            view.drawer_open = false;
        return view;
    }

    void shell_controller::apply_feedback(const frame_feedback& feedback)
    {
        feedback_ = feedback;
        settings_.apply_scroll_maximum(feedback.settings_maximum);
    }

    std::optional<diff_target> shell_controller::current_diff_target() const
    {
        if (dependencies_.workspace == nullptr)
            return std::nullopt;
        const repository_tab& tab { tabs_.selected() };
        if (tab.kind != repository_tab_kind::repository)
            return std::nullopt;
        const auto state { ui_.tabs.find(tab.id) };
        const auto repository { tab_repositories_.find(tab.id) };
        if (state == ui_.tabs.end() || state->second.diff.has_value() == false || repository == tab_repositories_.end())
            return std::nullopt;
        const repository_session* const session { std::as_const(*dependencies_.workspace).find_session(repository->second) };
        if (session == nullptr)
            return std::nullopt;
        return diff_target { repository->second, session->path(), *state->second.diff };
    }

    void shell_controller::set_diff_surface(const diff_surface_state& surface)
    {
        ui_.diff_surface = surface;
    }

    std::optional<std::chrono::steady_clock::time_point> shell_controller::next_tick() const noexcept
    {
        return pane_widths_flush_at_;
    }

    void shell_controller::tick(const std::chrono::steady_clock::time_point now)
    {
        if (pane_widths_flush_at_.has_value() && now >= *pane_widths_flush_at_)
            flush();
    }

    void shell_controller::flush()
    {
        if (pane_widths_flush_at_.has_value() == false)
            return;
        pane_widths_flush_at_.reset();
        const float sidebar { ui_.sidebar_width };
        const float detail { ui_.detail_width };
        update_settings([sidebar, detail](app_settings& value) {
            value.ui.pane_widths.sidebar = sidebar;
            value.ui.pane_widths.detail = detail;
        });
    }
} // namespace cyella
