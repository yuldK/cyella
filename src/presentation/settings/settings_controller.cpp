#include "presentation/settings/settings_controller.h"

#include "domain/ssh_connection.h"
#include "domain/text_catalog.h"
#include "presentation/settings/profiles_page.h"
#include "presentation/settings/shortcuts_page.h"

#include <algorithm>
#include <utility>

namespace cyella {
    namespace {
        constexpr std::u8string_view default_key_name { u8"id_ed25519" };
        constexpr std::u8string_view default_test_host { u8"github.com" };
        constexpr std::u8string_view default_tool_arguments { u8"{left} {right}" };

        void set_text(luil::text::text_edit_state& state, std::u8string text)
        {
            state = {};
            state.caret = text.size();
            state.anchor = text.size();
            state.text = std::move(text);
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

        std::string to_text(const std::u8string_view text)
        {
            return std::string { text.begin(), text.end() };
        }

        // 이미 있는 키와 겹치지 않는 첫 이름이다 (`id_ed25519`, `id_ed25519_2`, …).
        std::u8string free_key_name(const ssh_info_snapshot& info)
        {
            const auto taken = [&info](const std::u8string& name) { return std::ranges::any_of(info.keys, [&name](const ssh_key_entry& key) { return key.name == name; }); };
            std::u8string name { default_key_name };
            for (int suffix { 2 }; taken(name) && suffix < 100; ++suffix)
            {
                const std::string digits { std::to_string(suffix) };
                name = std::u8string { default_key_name } + u8"_" + std::u8string { digits.begin(), digits.end() };
            }
            return name;
        }

        std::u8string join_path(std::u8string directory, const std::u8string_view name)
        {
            if (directory.empty() == false && directory.back() != u8'\\' && directory.back() != u8'/')
                directory.push_back(u8'\\');
            directory.append(name);
            return directory;
        }

        // 아직 끝나지 않은 작업이다 (줄을 서 있거나 돌거나 입력을 기다린다).
        bool is_busy(const ssh_task_input& task) noexcept
        {
            return task.id != 0 && task.state.has_value() && (*task.state == operation_state::queued || *task.state == operation_state::running || *task.state == operation_state::needs_input);
        }

        bool asks_for_input(const input_kind kind) noexcept
        {
            return kind == input_kind::passphrase || kind == input_kind::host_key || kind == input_kind::credentials;
        }

        // 서명 쪽이 빌리는 길이다.
        signing_page_controller::dependencies signing_dependencies(const settings_controller::dependencies& source)
        {
            signing_page_controller::dependencies result {};
            result.request_info = source.request_signing_info;
            result.write = source.write_signing_config;
            result.repository_path = source.selected_repository_path;
            result.changed = source.signing_changed;
            return result;
        }
    } // namespace

    settings_controller::settings_controller(dependencies dependencies)
        : dependencies_ { std::move(dependencies) }
        , signing_ { signing_dependencies(dependencies_) }
    {}

    void settings_controller::apply_scroll_maximum(const std::optional<float> maximum)
    {
        scroll_maximum_ = maximum;
        if (state_.has_value() && maximum.has_value())
            state_->scroll = std::min(state_->scroll, *maximum);
    }

    luil::text::text_edit_state* settings_controller::draft(const text_target target)
    {
        if (state_.has_value() == false)
            return nullptr;
        settings_state& state { *state_ };
        switch (target)
        {
        case text_target::settings_git_path:
            return &state.git_path;
        case text_target::settings_ui_font:
            return &state.ui_font;
        case text_target::settings_code_font:
            return &state.code_font;
        case text_target::settings_tool_extensions:
            return state.tool_edit.has_value() ? &state.tool_edit->extensions : nullptr;
        case text_target::settings_tool_executable:
            return state.tool_edit.has_value() ? &state.tool_edit->executable : nullptr;
        case text_target::settings_tool_arguments:
            return state.tool_edit.has_value() ? &state.tool_edit->arguments : nullptr;
        case text_target::settings_key_name:
            return &state.key_name;
        case text_target::settings_key_comment:
            return &state.key_comment;
        case text_target::settings_test_host:
            return &state.test_host;
        case text_target::settings_signing_gpg_program:
        case text_target::settings_signing_ssh_program:
        case text_target::settings_signing_allowed_signers:
            return signing_.draft(target);
        case text_target::settings_hosting_host:
            return &state.hosting_host;
        case text_target::settings_hosting_api_base:
            return &state.hosting_api_base;
        case text_target::settings_web_link_host:
            return &state.web_link_host;
        case text_target::settings_web_link_base:
            return &state.web_link_base;
        case text_target::settings_web_link_icon:
            return &state.web_link_icon;
        case text_target::settings_shortcut_input:
            return state.shortcut_edit.has_value() ? &state.shortcut_edit->text : nullptr;
        case text_target::settings_profile_name:
            return state.profile_edit.has_value() ? &state.profile_edit->name : nullptr;
        case text_target::settings_profile_user_name:
            return state.profile_edit.has_value() ? &state.profile_edit->user_name : nullptr;
        case text_target::settings_profile_user_email:
            return state.profile_edit.has_value() ? &state.profile_edit->user_email : nullptr;
        case text_target::settings_profile_signing_key:
            return state.profile_edit.has_value() ? &state.profile_edit->signing_key : nullptr;
        case text_target::settings_rule_prefix:
            return &state.rule_draft.prefix;
        case text_target::none:
        case text_target::start_search:
        case text_target::sidebar_filter:
        case text_target::commit_subject:
        case text_target::commit_body:
        case text_target::clone_url:
        case text_target::clone_path:
            break;
        }
        return nullptr;
    }

    void settings_controller::update_settings(const std::function<void(app_settings&)>& change)
    {
        if (dependencies_.update != nullptr)
            dependencies_.update(change);
    }

    void settings_controller::open(const settings_page page)
    {
        settings_state state {};
        state.page = page;
        set_text(state.git_path, dependencies_.settings().git_executable);
        set_text(state.ui_font, dependencies_.settings().appearance.ui_font);
        set_text(state.code_font, dependencies_.settings().appearance.code_font);
        state.suggested_key_name = std::u8string { default_key_name };
        set_text(state.key_name, state.suggested_key_name);
        set_text(state.key_comment, dependencies_.default_key_comment);
        set_text(state.test_host, std::u8string { default_test_host });
        state_ = std::move(state);
        scroll_maximum_.reset();
        request_ssh_info();
        signing_.on_closed();
        if (page == settings_page::signing)
            signing_.on_page_opened();
    }

    void settings_controller::request_ssh_info()
    {
        if (state_.has_value() == false || dependencies_.request_ssh_info == nullptr)
            return;
        state_->ssh_loading = true;
        dependencies_.request_ssh_info();
    }

    void settings_controller::apply_git_path()
    {
        if (state_.has_value() == false)
            return;
        const std::u8string path { trimmed(state_->git_path.text) };
        if (check_git_path(path, dependencies_.probe).empty() == false)
            return;
        update_settings([&path](app_settings& settings) { settings.git_executable = path; });
        state_->message = path.empty() ? tr(u8"settings_view.git.path_reset_to_auto") : tr(u8"settings_view.git.path_saved");
    }

    void settings_controller::start_tool_edit(const std::optional<std::size_t> index, const bool file_app)
    {
        if (state_.has_value() == false)
            return;
        tool_edit_state edit {};
        edit.index = index;
        edit.file_app = file_app;
        if (file_app)
        {
            const std::vector<external_file_app>& apps { dependencies_.settings().external_file_apps };
            if (index.has_value())
            {
                if (*index >= apps.size())
                    return;
                set_text(edit.extensions, apps[*index].name);
                set_text(edit.executable, apps[*index].executable);
                set_text(edit.arguments, apps[*index].arguments);
            }
            else
                set_text(edit.arguments, u8"{file}");
            state_->tool_edit = std::move(edit);
            state_->message.clear();
            return;
        }
        const std::vector<external_diff_tool>& tools { dependencies_.settings().external_diff_tools };
        if (index.has_value())
        {
            if (*index >= tools.size())
                return;
            set_text(edit.extensions, format_extension_list(tools[*index].extensions));
            set_text(edit.executable, tools[*index].executable);
            set_text(edit.arguments, tools[*index].arguments);
        }
        else
            set_text(edit.arguments, std::u8string { default_tool_arguments });
        state_->tool_edit = std::move(edit);
        state_->message.clear();
    }

    void settings_controller::save_tool()
    {
        if (state_.has_value() == false || state_->tool_edit.has_value() == false)
            return;
        const tool_edit_state& edit { *state_->tool_edit };
        if (edit.file_app)
        {
            if (check_file_app_draft(edit.extensions.text, edit.executable.text, edit.arguments.text, dependencies_.probe).ok == false)
                return;
            const external_file_app app { trimmed(edit.extensions.text), trimmed(edit.executable.text), trimmed(edit.arguments.text) };
            const std::optional<std::size_t> index { edit.index };
            update_settings([&app, index](app_settings& settings) {
                if (index.has_value() && *index < settings.external_file_apps.size())
                    settings.external_file_apps[*index] = app;
                else
                    settings.external_file_apps.push_back(app);
            });
            state_->tool_edit.reset();
            state_->message = tr(u8"settings_view.file_app.saved");
            return;
        }
        if (check_tool_draft(edit.extensions.text, edit.executable.text, edit.arguments.text, dependencies_.probe).ok == false)
            return;
        const external_diff_tool tool { make_external_tool(edit.extensions.text, edit.executable.text, edit.arguments.text) };
        const std::optional<std::size_t> index { edit.index };
        update_settings([&tool, index](app_settings& settings) {
            if (index.has_value() && *index < settings.external_diff_tools.size())
                settings.external_diff_tools[*index] = tool;
            else
                settings.external_diff_tools.push_back(tool);
        });
        state_->tool_edit.reset();
        state_->message = tr(u8"settings_view.tool.saved");
    }

    void settings_controller::remove_tool(const std::size_t index, const bool file_app)
    {
        if (file_app)
        {
            if (state_.has_value() == false || index >= dependencies_.settings().external_file_apps.size())
                return;
            update_settings([index](app_settings& settings) { settings.external_file_apps.erase(settings.external_file_apps.begin() + static_cast<std::ptrdiff_t>(index)); });
            state_->tool_edit.reset();
            state_->message = tr(u8"settings_view.file_app.removed");
            return;
        }
        if (state_.has_value() == false || index >= dependencies_.settings().external_diff_tools.size())
            return;
        update_settings([index](app_settings& settings) { settings.external_diff_tools.erase(settings.external_diff_tools.begin() + static_cast<std::ptrdiff_t>(index)); });
        // 고치던 줄이 지워졌거나 번호가 밀렸다. 어느 쪽이든 편집을 접는다.
        state_->tool_edit.reset();
        state_->message = tr(u8"settings_view.tool.removed");
    }

    void settings_controller::create_ssh_key()
    {
        if (state_.has_value() == false || dependencies_.workspace == nullptr)
            return;
        settings_state& state { *state_ };
        const std::u8string name { trimmed(state.key_name.text) };
        const std::u8string comment { trimmed(state.key_comment.text) };
        // 돌고 있거나 입력을 기다리는 작업이 있으면 하나만 낸다.
        if (is_busy(task_of(state.keygen_operation)))
            return;
        if (dependencies_.ssh_tools_found == false || dependencies_.ssh_directory.empty() || check_key_name(name).empty() == false)
            return;
        if (state.ssh.has_value() && std::ranges::any_of(state.ssh->keys, [&name](const ssh_key_entry& key) { return key.name == name; }))
            return;
        const std::u8string path { join_path(dependencies_.ssh_directory, name) };
        const operation_id id { dependencies_.workspace->submit_setup(ssh_keygen_request { .key_path = path, .comment = comment }) };
        if (id == 0)
            return;
        state.keygen_operation = id;
        state.keygen_target = path;
        state.message.clear();
    }

    void settings_controller::test_ssh_connection()
    {
        if (state_.has_value() == false || dependencies_.workspace == nullptr || dependencies_.ssh_tools_found == false)
            return;
        const std::optional<ssh_target> target { parse_ssh_target(state_->test_host.text) };
        if (target.has_value() == false || is_busy(task_of(state_->test_operation)))
            return;
        const operation_id id { dependencies_.workspace->submit_setup(ssh_connection_test_request { .host = target->host, .user_name = target->user_name, .port = target->port }) };
        if (id == 0)
            return;
        state_->test_operation = id;
        state_->test_target = target->user_name + u8"@" + target->host;
        state_->message.clear();
    }

    void settings_controller::copy_public_key(const std::u8string& name)
    {
        if (state_.has_value() == false || state_->ssh.has_value() == false)
            return;
        for (const ssh_key_entry& key : state_->ssh->keys)
            if (key.name == name && key.public_key_line.empty() == false)
            {
                if (dependencies_.copy_text != nullptr)
                    dependencies_.copy_text(to_text(key.public_key_line));
                state_->message = tr(u8"settings_view.ssh.public_key_copied", { { u8"name", name } });
                return;
            }
    }

    bool settings_controller::handle(const luil::app_message& message)
    {
        if (const auto* const intent { message.get<open_settings_intent>() }; intent != nullptr)
        {
            open(intent->page);
            return true;
        }
        if (message.get<close_settings_intent>() != nullptr)
        {
            state_.reset();
            signing_.on_closed();
            return true;
        }
        // "서명" 쪽의 메시지다. 화면이 닫힌 뒤에 늦게 온 결과는 삼킨다.
        if (signing_.handle(message, state_.has_value()))
            return true;
        if (const auto* const intent { message.get<ssh_info_loaded_intent>() }; intent != nullptr)
        {
            // 닫은 뒤에 도착한 결과는 버린다.
            if (state_.has_value())
            {
                state_->ssh = intent->info;
                state_->ssh_loading = false;
                // 이름 칸을 사용자가 고치지 않았다면 겹치지 않는 이름으로 맞춘다.
                if (trimmed(state_->key_name.text) == state_->suggested_key_name)
                {
                    state_->suggested_key_name = free_key_name(intent->info);
                    set_text(state_->key_name, state_->suggested_key_name);
                }
            }
            return true;
        }
        if (handle_hosting(message))
            return true;
        if (handle_web_links(message))
            return true;
        // 단축키와 사용자 프로필 쪽의 메시지다 (화면이 닫힌 뒤에 늦게 온 것은 삼킨다).
        if (is_shortcut_message(message))
        {
            if (state_.has_value())
                handle_shortcut_message(message);
            return true;
        }
        if (is_profile_message(message))
        {
            if (state_.has_value())
                handle_profile_message(message);
            return true;
        }
        // 나머지는 화면이 열려 있을 때만 뜻이 있다. 닫힌 뒤에 늦게 온 메시지는 삼킨다.
        const auto settings_message = [&message]() {
            if (message.get<apply_font_settings_intent>() != nullptr || message.get<select_font_intent>() != nullptr || message.get<open_font_list_intent>() != nullptr
                || message.get<close_font_list_intent>() != nullptr || message.get<font_list_scroll_intent>() != nullptr)
                return true;
            return message.get<select_settings_page_intent>() != nullptr || message.get<settings_scroll_intent>() != nullptr || message.get<set_theme_intent>() != nullptr
                || message.get<set_accent_intent>() != nullptr || message.get<set_tab_shape_intent>() != nullptr || message.get<set_tab_marker_intent>() != nullptr
                || message.get<set_language_intent>() != nullptr || message.get<set_gravatar_intent>() != nullptr || message.get<apply_git_path_intent>() != nullptr
                || message.get<reset_git_path_intent>() != nullptr || message.get<add_tool_intent>() != nullptr || message.get<edit_tool_intent>() != nullptr
                || message.get<remove_tool_intent>() != nullptr || message.get<save_tool_intent>() != nullptr || message.get<cancel_tool_edit_intent>() != nullptr
                || message.get<insert_tool_token_intent>() != nullptr || message.get<settings_file_chosen_intent>() != nullptr || message.get<refresh_ssh_intent>() != nullptr
                || message.get<create_ssh_key_intent>() != nullptr || message.get<copy_public_key_intent>() != nullptr || message.get<test_ssh_connection_intent>() != nullptr
                || message.get<set_diff_layout_default_intent>() != nullptr || message.get<set_diff_context_lines_intent>() != nullptr;
        };
        if (settings_message() == false)
            return false;
        if (state_.has_value() == false)
            return true;
        settings_state& state { *state_ };

        if (const auto* const page_intent { message.get<select_settings_page_intent>() }; page_intent != nullptr)
        {
            state.page = page_intent->page;
            state.font_list.reset();
            state.scroll = 0.0f;
            state.message.clear();
            if ((page_intent->page == settings_page::ssh || page_intent->page == settings_page::signing) && state.ssh.has_value() == false && state.ssh_loading == false)
                request_ssh_info();
            if (page_intent->page == settings_page::signing)
                signing_.on_page_opened();
        }
        else if (const auto* const scrolled { message.get<settings_scroll_intent>() }; scrolled != nullptr)
        {
            float next { scrolled->absolute ? scrolled->value : state.scroll + scrolled->value };
            if (scroll_maximum_.has_value())
                next = std::min(next, *scroll_maximum_);
            state.scroll = std::max(next, 0.0f);
        }
        else if (const auto* const theme_intent { message.get<set_theme_intent>() }; theme_intent != nullptr)
        {
            const theme_preference theme { theme_intent->theme };
            update_settings([theme](app_settings& settings) { settings.appearance.theme = theme; });
        }
        else if (const auto* const accent_intent { message.get<set_accent_intent>() }; accent_intent != nullptr)
        {
            const std::u8string accent { accent_intent->id };
            update_settings([&accent](app_settings& settings) { settings.appearance.accent = accent; });
        }
        else if (const auto* const shape_intent { message.get<set_tab_shape_intent>() }; shape_intent != nullptr)
        {
            const tab_shape shape { shape_intent->shape };
            update_settings([shape](app_settings& settings) { settings.appearance.tabs = shape; });
        }
        else if (const auto* const marker_intent { message.get<set_tab_marker_intent>() }; marker_intent != nullptr)
        {
            const tab_marker_place place { marker_intent->place };
            update_settings([place](app_settings& settings) { settings.appearance.tab_marker = place; });
        }
        else if (const auto* const language_intent { message.get<set_language_intent>() }; language_intent != nullptr)
        {
            const language_preference language { language_intent->language };
            update_settings([language](app_settings& settings) { settings.language = language; });
        }
        else if (const auto* const opened { message.get<open_font_list_intent>() }; opened != nullptr)
        {
            state.font_list = opened->list;
            state.font_scroll = 0.0f;
        }
        else if (const auto* const font_scroll { message.get<font_list_scroll_intent>() }; font_scroll != nullptr)
            state.font_scroll = std::max(0.0f, font_scroll->absolute ? font_scroll->value : state.font_scroll + font_scroll->value);
        else if (message.get<close_font_list_intent>() != nullptr)
            state.font_list.reset();
        else if (const auto* const selected { message.get<select_font_intent>() }; selected != nullptr)
        {
            const std::u8string family { trimmed(selected->family) };
            set_text(selected->code ? state.code_font : state.ui_font, family);
            update_settings([code = selected->code, &family](app_settings& settings) { (code ? settings.appearance.code_font : settings.appearance.ui_font) = family; });
            state.font_list.reset();
            state.message = tr(u8"settings_view.font.applied");
        }
        else if (message.get<apply_font_settings_intent>() != nullptr)
        {
            const std::u8string ui_family { trimmed(state.ui_font.text) };
            const std::u8string code_family { trimmed(state.code_font.text) };
            update_settings([&](app_settings& settings) {
                settings.appearance.ui_font = ui_family;
                settings.appearance.code_font = code_family;
            });
            set_text(state.ui_font, ui_family);
            set_text(state.code_font, code_family);
            state.font_list.reset();
            state.message = tr(u8"settings_view.font.applied");
        }
        else if (const auto* const gravatar_intent { message.get<set_gravatar_intent>() }; gravatar_intent != nullptr)
        {
            const bool on { gravatar_intent->on };
            update_settings([on](app_settings& settings) { settings.appearance.gravatar = on; });
        }
        else if (const auto* const layout_intent { message.get<set_diff_layout_default_intent>() }; layout_intent != nullptr)
        {
            const diff_layout_preference layout { layout_intent->layout };
            update_settings([layout](app_settings& settings) { settings.diff.layout = layout; });
        }
        else if (const auto* const context_intent { message.get<set_diff_context_lines_intent>() }; context_intent != nullptr)
        {
            const std::uint32_t lines { std::min(context_intent->lines, maximum_diff_context_lines) };
            update_settings([lines](app_settings& settings) { settings.diff.context_lines = lines; });
        }
        else if (message.get<apply_git_path_intent>() != nullptr)
            apply_git_path();
        else if (message.get<reset_git_path_intent>() != nullptr)
        {
            set_text(state.git_path, {});
            apply_git_path();
        }
        else if (const auto* const add_intent { message.get<add_tool_intent>() }; add_intent != nullptr)
            start_tool_edit(std::nullopt, add_intent->file_app);
        else if (const auto* const edit_intent { message.get<edit_tool_intent>() }; edit_intent != nullptr)
            start_tool_edit(edit_intent->index, edit_intent->file_app);
        else if (const auto* const remove_intent { message.get<remove_tool_intent>() }; remove_intent != nullptr)
            remove_tool(remove_intent->index, remove_intent->file_app);
        else if (message.get<save_tool_intent>() != nullptr)
            save_tool();
        else if (message.get<cancel_tool_edit_intent>() != nullptr)
            state.tool_edit.reset();
        else if (const auto* const token_intent { message.get<insert_tool_token_intent>() }; token_intent != nullptr)
        {
            if (state.tool_edit.has_value())
            {
                std::u8string text { state.tool_edit->arguments.text };
                if (text.empty() == false && text.back() != u8' ')
                    text.push_back(u8' ');
                text += token_intent->token;
                set_text(state.tool_edit->arguments, std::move(text));
            }
        }
        else if (const auto* const file_intent { message.get<settings_file_chosen_intent>() }; file_intent != nullptr)
        {
            if (file_intent->purpose == file_purpose::git_executable)
                set_text(state.git_path, file_intent->path);
            else if (state.tool_edit.has_value())
                set_text(state.tool_edit->executable, file_intent->path);
        }
        else if (message.get<refresh_ssh_intent>() != nullptr)
            request_ssh_info();
        else if (message.get<create_ssh_key_intent>() != nullptr)
            create_ssh_key();
        else if (const auto* const copy_intent { message.get<copy_public_key_intent>() }; copy_intent != nullptr)
            copy_public_key(copy_intent->name);
        else if (message.get<test_ssh_connection_intent>() != nullptr)
            test_ssh_connection();
        return true;
    }

    void settings_controller::after_events()
    {
        if (state_.has_value() == false || dependencies_.workspace == nullptr)
            return;
        const operation_id keygen { state_->keygen_operation };
        if (keygen == 0 || keygen == refreshed_after_)
            return;
        const operation_status* const status { dependencies_.workspace->find_operation(keygen) };
        if (status == nullptr || status->finished() == false)
            return;
        // 키를 만들었으면 목록에 나타나야 한다. 실패해도 한 번 다시 읽는다 (일부 파일이 남았을 수 있다).
        refreshed_after_ = keygen;
        // 만든 이름을 그대로 두면 곧바로 "같은 이름의 키가 이미 있습니다"가 보인다. 이름 초안을 비우고, 목록을 다시 읽으면
        // (`ssh_info_loaded_intent`) 겹치지 않는 이름으로 채운다. 실패했으면 사용자가 고치던 이름을 그대로 둔다.
        if (status->state == operation_state::succeeded)
        {
            set_text(state_->key_name, {});
            state_->suggested_key_name.clear();
        }
        request_ssh_info();
    }

    ssh_task_input settings_controller::task_of(const operation_id id) const
    {
        ssh_task_input input {};
        if (id == 0 || dependencies_.workspace == nullptr)
            return input;
        const operation_status* const status { dependencies_.workspace->find_operation(id) };
        if (status == nullptr)
            return input;
        input.id = id;
        input.state = status->state;
        input.summary = status->result_summary;
        if (status->failure.has_value())
        {
            input.failure_summary = status->failure->summary;
            input.failure_detail = status->failure->detail;
        }
        input.waiting_for_prompt = status->state == operation_state::needs_input && status->needs_input.has_value() && asks_for_input(status->needs_input->kind);
        input.cancellable = status->cancellable;
        input.classification = dependencies_.workspace->ssh_connection_result_of(id);
        return input;
    }

    std::optional<settings_view> settings_controller::make_view(const std::optional<luil::text_composition_event>& composition) const
    {
        if (state_.has_value() == false)
            return std::nullopt;
        settings_view_inputs inputs {};
        inputs.state = &*state_;
        inputs.settings = &dependencies_.settings();
        inputs.git_tool = &dependencies_.git_tool;
        inputs.probe = dependencies_.probe;
        inputs.read_only = dependencies_.read_only != nullptr && dependencies_.read_only();
        inputs.unsaved = dependencies_.unsaved != nullptr && dependencies_.unsaved();
        inputs.ssh_directory = dependencies_.ssh_directory;
        inputs.ssh_tools_found = dependencies_.ssh_tools_found && dependencies_.workspace != nullptr;
        inputs.ssh_tools_origin = dependencies_.ssh_tools_origin;
        inputs.keygen = task_of(state_->keygen_operation);
        inputs.test = task_of(state_->test_operation);
        inputs.composition = composition;
        if (dependencies_.hosting_rows != nullptr)
            inputs.hosting_hosts = dependencies_.hosting_rows(dependencies_.settings().hosting.hosts);
        settings_view view { build_settings_view(inputs) };
        view.signing = signing_.make_view(state_->ssh.has_value() ? &*state_->ssh : nullptr, dependencies_.probe, composition);
        // 단축키와 사용자 프로필 쪽은 열려 있을 때만 짓는다.
        if (state_->page == settings_page::shortcuts)
            view.shortcuts = build_shortcuts_page_view(dependencies_.settings(), state_->shortcut_edit, composition);
        else if (state_->page == settings_page::profiles)
        {
            const profiles_repository_inputs repository { dependencies_.repository_profile != nullptr ? dependencies_.repository_profile() : profiles_repository_inputs {} };
            view.profiles = build_profiles_page_view(dependencies_.settings(), state_->profile_edit, state_->rule_draft, composition, repository);
        }
        return view;
    }
} // namespace cyella
