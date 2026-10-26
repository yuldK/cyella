// 명령 팔레트와 레지스트리 명령 실행이다. shell_controller의 멤버 함수를 파일 하나로 나눠 둔다.
#include "presentation/shell_controller.h"

#include "presentation/commands/command_intents.h"
#include "presentation/graph_geometry.h"
#include "presentation/palette/palette_intents.h"
#include "presentation/settings/profiles_intents.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/snapshot_dialogs.h"

#include "luil/ui/text_input_state.h"

#include <algorithm>
#include <charconv>
#include <optional>
#include <utility>

namespace cyella {
    namespace {
        std::string to_text(const std::u8string_view text)
        {
            return std::string { text.begin(), text.end() };
        }

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::optional<std::u8string> key_argument(const std::u8string& key, const std::u8string_view prefix)
        {
            if (key.starts_with(prefix) == false || key.size() == prefix.size())
                return std::nullopt;
            return key.substr(prefix.size());
        }

        // 환경 설정 쪽을 여는 명령이 여는 쪽이다.
        std::optional<settings_page> settings_page_of(const std::string_view id)
        {
            if (id == "app.settings" || id == "app.settings.general")
                return settings_page::general;
            if (id == "app.settings.diff")
                return settings_page::diff_tools;
            if (id == "app.settings.ssh")
                return settings_page::ssh;
            if (id == "app.settings.profiles")
                return settings_page::profiles;
            if (id == "app.settings.shortcuts")
                return settings_page::shortcuts;
            if (id == "app.settings.hosting")
                return settings_page::hosting;
            return std::nullopt;
        }
    } // namespace

    bool shell_controller::handle_command_message(const luil::app_message& message)
    {
        if (const auto* const intent { message.get<run_command_intent>() }; intent != nullptr)
        {
            run_command(intent->id);
            return true;
        }
        if (const auto* const intent { message.get<open_palette_intent>() }; intent != nullptr)
        {
            open_palette(intent->mode);
            return true;
        }
        if (message.get<close_palette_intent>() != nullptr)
        {
            close_palette();
            return true;
        }
        if (const auto* const intent { message.get<palette_move_intent>() }; intent != nullptr)
        {
            move_palette_cursor(intent->delta);
            return true;
        }
        if (const auto* const intent { message.get<palette_cursor_intent>() }; intent != nullptr)
        {
            if (ui_.palette.has_value())
            {
                ui_.palette->cursor = intent->key;
                ui_.palette->reveal_cursor = true;
            }
            return true;
        }
        if (const auto* const intent { message.get<palette_scroll_intent>() }; intent != nullptr)
        {
            if (ui_.palette.has_value())
            {
                const float offset { intent->absolute ? intent->value : ui_.palette->scroll + intent->value };
                ui_.palette->scroll = std::max(offset, 0.0f);
                if (feedback_.palette_maximum.has_value())
                    ui_.palette->scroll = std::min(ui_.palette->scroll, *feedback_.palette_maximum);
                ui_.palette->reveal_cursor = false;
            }
            return true;
        }
        if (const auto* const intent { message.get<palette_pick_intent>() }; intent != nullptr)
        {
            pick_palette_item(intent->key);
            return true;
        }
        return handle_profile_message(message);
    }

    command_availability shell_controller::current_availability()
    {
        command_availability availability {};
        availability.profiles_exist = settings().profiles.empty() == false;
        const repository_session* const session { selected_session() };
        if (session == nullptr)
            return availability;
        const repository_summary& summary { session->snapshot().summary };
        // 툴바 단추가 켜지는 조건과 같다 (`fill_toolbar`).
        const bool ready { session->loaded() };
        availability.repository = true;
        availability.fetch = ready && summary.remotes.empty() == false;
        availability.pull = ready && summary.has_upstream;
        availability.push = ready && summary.is_detached == false && summary.current_branch.empty() == false;
        availability.branch = ready;
        availability.stash = ready && summary.changed_file_count > 0;
        availability.pop = ready && session->snapshot().stashes.empty() == false;
        availability.drawer = last_mode_ != layout_mode::wide;
        availability.selection_menu = selection_menu_target().has_value();
        const hosting_repository_view hosting { hosting_.view_of(session->id()) };
        availability.hosting_supported = hosting.support == hosting_support::supported;
        availability.hosting_signed_in = hosting.signed_in;
        availability.hosting_can_create = hosting.can_create;
        if (const tab_ui_state* const tab { selected_tab_state() }; tab != nullptr)
            availability.commit = ready && std::ranges::any_of(tab->commit_subject.text, [](const char8_t value) { return value != u8' ' && value != u8'\t' && value != u8'\r' && value != u8'\n'; });
        return availability;
    }

    void shell_controller::run_command(const std::string_view id)
    {
        if (find_command(id) == nullptr || command_available(id, current_availability()) == false)
            return;
        if (id == "palette.open")
        {
            // 명령 모드로 열려 있으면 닫는다 (같은 키로 여닫는다). 다른 모드면 명령 모드로 돌아간다.
            if (ui_.palette.has_value() && ui_.palette->mode == palette_mode::commands)
                close_palette();
            else
                open_palette(palette_mode::commands);
            return;
        }
        // 팔레트의 모드를 여는 명령 말고는 열려 있던 팔레트를 닫고 실행한다.
        const bool opens_palette { id == "repo.open" || id == "git.branch.checkout" || id == "profile.apply" };
        if (opens_palette == false)
            close_palette();
        if (id != "view.close")
            update_settings([id](app_settings& value) { record_command_use(value, id); });

        const auto send = [this](luil::app_message message) { static_cast<void>(handle(message)); };
        if (const std::optional<settings_page> page { settings_page_of(id) }; page.has_value())
            send(luil::app_message { open_settings_intent { *page } });
        else if (id == "app.theme")
            send(luil::app_message { cycle_theme_intent {} });
        else if (id == "app.log")
            send(luil::app_message { toggle_operation_log_intent {} });
        else if (id == "tab.new")
            send(luil::app_message { new_tab_intent {} });
        else if (id == "tab.close")
            send(luil::app_message { close_selected_tab_intent {} });
        else if (id == "tab.next")
            send(luil::app_message { cycle_tab_intent { true } });
        else if (id == "tab.previous")
            send(luil::app_message { cycle_tab_intent { false } });
        else if (id == "repo.open")
            open_palette(palette_mode::repositories);
        else if (id == "repo.clone")
            send(luil::app_message { open_clone_dialog_intent {} });
        else if (id == "repo.refresh")
            send(luil::app_message { refresh_intent {} });
        // `repository_open_apps`의 앞 둘은 늘 탐색기와 VS Code다.
        else if (id == "repo.reveal")
            open_repository_in_app(0);
        else if (id == "repo.open_vscode")
            open_repository_in_app(1);
        else if (id == "view.changes" || id == "view.history")
            send(luil::app_message { select_workspace_intent { id == "view.changes" } });
        else if (id == "git.fetch")
            handle_toolbar(toolbar_action::fetch);
        else if (id == "git.pull")
            handle_toolbar(toolbar_action::pull);
        else if (id == "git.push")
            handle_toolbar(toolbar_action::push);
        else if (id == "git.remote.add")
        {
            const repository_session* const session { selected_session() };
            if (session != nullptr)
                open_add_remote_dialog(session->snapshot().summary.is_detached ? std::string {} : session->snapshot().summary.current_branch);
        }
        else if (id == "git.branch.new")
            handle_toolbar(toolbar_action::branch);
        else if (id == "git.branch.checkout")
            open_palette(palette_mode::branches);
        else if (id == "git.tag.new")
            open_create_tag_dialog({});
        else if (id == "git.stash")
            handle_toolbar(toolbar_action::stash);
        else if (id == "git.stash.pop")
            handle_toolbar(toolbar_action::pop);
        else if (id == "git.commit")
            handle_commit();
        else if (id == "hosting.sign_in")
            send(luil::app_message { hosting_sign_in_intent {} });
        else if (id == "hosting.refresh")
            hosting_.activate_row(selected_repository_id(), hosting_row_refresh);
        else if (id == "hosting.pull_request.new")
            open_create_pull_request_dialog();
        else if (id == "git.amend")
            send(luil::app_message { toggle_amend_intent {} });
        else if (id == "view.drawer")
            send(luil::app_message { toggle_drawer_intent { ui_.drawer_open == false } });
        else if (id == "view.menu")
            open_menu_for_selection();
        else if (id == "view.close")
            send(luil::app_message { close_overlays_intent {} });
        else if (id == "profile.apply")
            open_palette(palette_mode::profiles);
        // repo.open_folder·repo.init은 UI thread의 폴더 대화 상자라 여기서 실행하지 않는다 (`ui::action_for_command`가 UI 명령을 낸다).
    }

    void shell_controller::open_palette(const palette_mode mode)
    {
        ui_.selector.reset();
        ui_.menu.reset();
        ui_.palette = palette_state { .mode = mode, .filter = {}, .cursor = {} };
    }

    void shell_controller::close_palette()
    {
        ui_.palette.reset();
    }

    std::u8string shell_controller::palette_query() const
    {
        if (ui_.palette.has_value() == false)
            return {};
        const luil::text_input_view view { luil::make_text_input_view(ui_.palette->filter, ui_.composition, to_luil_target(text_target::palette_filter)) };
        return std::u8string { view.displayed_text() };
    }

    palette_context shell_controller::make_palette_context(const palette_mode mode)
    {
        palette_context context {};
        context.mode = mode;
        context.settings = &settings();
        context.availability = current_availability();
        repository_session* const session { selected_session() };
        switch (mode)
        {
        case palette_mode::commands:
            if (session != nullptr)
                if (const std::optional<menu_target> target { selection_menu_target() }; target.has_value())
                {
                    const menu_context menu { build_menu_context(*session, *target, history_index(*session).get()) };
                    context.selection_menu = build_context_menu(*target, menu);
                }
            break;
        case palette_mode::branches:
            if (session != nullptr)
            {
                context.refs = session->snapshot().refs;
                context.current_branch = session->snapshot().summary.current_branch;
                context.detached = session->snapshot().summary.is_detached;
            }
            break;
        case palette_mode::repositories:
            context.repositories = make_repository_choices(tabs_, settings(), [this](const std::u8string_view path) { return repository_exists(path); }, tabs_.selected().id);
            break;
        case palette_mode::profiles:
            context.current_profile = applied_profile_name();
            context.suggested_profile = suggested_profile_name();
            break;
        }
        return context;
    }

    std::optional<palette_view> shell_controller::make_palette_view()
    {
        if (ui_.palette.has_value() == false)
            return std::nullopt;
        const palette_state& state { *ui_.palette };
        palette_view view {};
        view.mode = state.mode;
        view.scroll = state.scroll;
        view.reveal_cursor = state.reveal_cursor;
        view.title = std::u8string { palette_mode_title(state.mode) };
        view.placeholder = std::u8string { palette_mode_placeholder(state.mode) };
        view.filter = luil::make_text_input_view(state.filter, ui_.composition, to_luil_target(text_target::palette_filter));
        const palette_list list { build_palette_list(make_palette_context(state.mode), view.filter.displayed_text()) };
        view.matched = list.matched;
        std::u8string first_enabled {};
        bool cursor_found { false };
        for (const palette_item& item : list.items)
        {
            view.items.push_back(palette_item_view { .key = item.key, .title = item.title, .detail = item.detail, .enabled = item.enabled, .ui_thread_command = item.ui_thread_command });
            if (item.enabled)
            {
                if (first_enabled.empty())
                    first_enabled = item.key;
                cursor_found = cursor_found || item.key == state.cursor;
            }
        }
        // 커서가 비었거나 목록에서 사라졌으면 첫 활성 항목이다 (Enter가 곧바로 맨 위를 실행한다).
        view.cursor = cursor_found ? state.cursor : first_enabled;
        return view;
    }

    void shell_controller::move_palette_cursor(const int delta)
    {
        if (ui_.palette.has_value() == false)
            return;
        const palette_list list { build_palette_list(make_palette_context(ui_.palette->mode), palette_query()) };
        std::vector<std::u8string> keys {};
        for (const palette_item& item : list.items)
            if (item.enabled)
                keys.push_back(item.key);
        if (keys.empty())
            return;
        const auto found { std::ranges::find(keys, ui_.palette->cursor) };
        const int current { found == keys.end() ? 0 : static_cast<int>(found - keys.begin()) };
        const int next { std::clamp(current + delta, 0, static_cast<int>(keys.size()) - 1) };
        ui_.palette->cursor = keys[static_cast<std::size_t>(next)];
        ui_.palette->reveal_cursor = true;
    }

    void shell_controller::pick_palette_item(const std::u8string& key)
    {
        if (ui_.palette.has_value() == false)
            return;
        if (const std::optional<std::u8string> command { key_argument(key, palette_command_key_prefix) }; command.has_value())
        {
            // 모드를 여는 명령이면 run_command가 팔레트를 새 모드로 다시 연다. 아니면 팔레트를 닫고 실행한다.
            run_command(to_text(*command));
            return;
        }
        if (const std::optional<std::u8string> branch { key_argument(key, palette_branch_key_prefix) }; branch.has_value())
        {
            close_palette();
            static_cast<void>(handle(luil::app_message { activate_ref_intent { ref_kind::local, to_text(*branch) } }));
            return;
        }
        if (const std::optional<std::u8string> branch { key_argument(key, palette_remote_branch_key_prefix) }; branch.has_value())
        {
            close_palette();
            static_cast<void>(handle(luil::app_message { activate_ref_intent { ref_kind::remote, to_text(*branch) } }));
            return;
        }
        // 열린 탭과 최근 저장소의 키는 툴바 선택 상자와 같은 접두(`tab:`·`recent:`)라 같은 실행기를 쓴다.
        if (key.starts_with(palette_tab_key_prefix) || key.starts_with(palette_recent_key_prefix))
        {
            close_palette();
            select_from_selector(key);
            return;
        }
        if (const std::optional<std::u8string> profile { key_argument(key, palette_profile_key_prefix) }; profile.has_value())
        {
            close_palette();
            apply_profile(*profile);
            return;
        }
        if (key == palette_manage_profiles_key)
        {
            close_palette();
            static_cast<void>(handle(luil::app_message { open_settings_intent { settings_page::profiles } }));
            return;
        }
        if (const std::optional<std::u8string> entry { key_argument(key, palette_menu_key_prefix) }; entry.has_value())
        {
            repository_session* const session { selected_session() };
            const std::optional<menu_target> target { selection_menu_target() };
            close_palette();
            if (session == nullptr || target.has_value() == false)
                return;
            const menu_context context { build_menu_context(*session, *target, history_index(*session).get()) };
            const menu_model model { build_context_menu(*target, context) };
            const auto found { std::ranges::find_if(model.entries, [&entry](const menu_entry& value) { return value.key == *entry && value.enabled; }) };
            if (found != model.entries.end())
                run_menu_action(*target, *entry, context);
        }
    }
} // namespace cyella
