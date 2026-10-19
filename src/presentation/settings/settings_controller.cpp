#include "presentation/settings/settings_controller.h"

#include "domain/ssh_connection.h"

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
    } // namespace

    settings_controller::settings_controller(dependencies dependencies)
        : dependencies_ { std::move(dependencies) }
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
        state.suggested_key_name = std::u8string { default_key_name };
        set_text(state.key_name, state.suggested_key_name);
        set_text(state.key_comment, dependencies_.default_key_comment);
        set_text(state.test_host, std::u8string { default_test_host });
        state_ = std::move(state);
        scroll_maximum_.reset();
        request_ssh_info();
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
        state_->message = path.empty() ? u8"자동 탐색으로 돌아갑니다. 다음 실행부터 적용됩니다." : u8"git 경로를 저장했습니다. 다음 실행부터 적용됩니다.";
    }

    void settings_controller::start_tool_edit(const std::optional<std::size_t> index)
    {
        if (state_.has_value() == false)
            return;
        tool_edit_state edit {};
        edit.index = index;
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
        state_->message = u8"외부 diff 도구를 저장했습니다.";
    }

    void settings_controller::remove_tool(const std::size_t index)
    {
        if (state_.has_value() == false || index >= dependencies_.settings().external_diff_tools.size())
            return;
        update_settings([index](app_settings& settings) { settings.external_diff_tools.erase(settings.external_diff_tools.begin() + static_cast<std::ptrdiff_t>(index)); });
        // 고치던 줄이 지워졌거나 번호가 밀렸다. 어느 쪽이든 편집을 접는다.
        state_->tool_edit.reset();
        state_->message = u8"외부 diff 도구를 지웠습니다.";
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
                state_->message = u8"공개 키를 클립보드에 복사했습니다: " + name + u8".pub";
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
            return true;
        }
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
        // 나머지는 화면이 열려 있을 때만 뜻이 있다. 닫힌 뒤에 늦게 온 메시지는 삼킨다.
        const auto settings_message = [&message]() {
            return message.get<select_settings_page_intent>() != nullptr || message.get<settings_scroll_intent>() != nullptr || message.get<set_theme_intent>() != nullptr
                || message.get<apply_git_path_intent>() != nullptr || message.get<reset_git_path_intent>() != nullptr || message.get<add_tool_intent>() != nullptr
                || message.get<edit_tool_intent>() != nullptr || message.get<remove_tool_intent>() != nullptr || message.get<save_tool_intent>() != nullptr
                || message.get<cancel_tool_edit_intent>() != nullptr || message.get<insert_tool_token_intent>() != nullptr || message.get<settings_file_chosen_intent>() != nullptr
                || message.get<refresh_ssh_intent>() != nullptr || message.get<create_ssh_key_intent>() != nullptr || message.get<copy_public_key_intent>() != nullptr
                || message.get<test_ssh_connection_intent>() != nullptr;
        };
        if (settings_message() == false)
            return false;
        if (state_.has_value() == false)
            return true;
        settings_state& state { *state_ };

        if (const auto* const page_intent { message.get<select_settings_page_intent>() }; page_intent != nullptr)
        {
            state.page = page_intent->page;
            state.scroll = 0.0f;
            state.message.clear();
            if (page_intent->page == settings_page::ssh && state.ssh.has_value() == false && state.ssh_loading == false)
                request_ssh_info();
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
        else if (message.get<apply_git_path_intent>() != nullptr)
            apply_git_path();
        else if (message.get<reset_git_path_intent>() != nullptr)
        {
            set_text(state.git_path, {});
            apply_git_path();
        }
        else if (message.get<add_tool_intent>() != nullptr)
            start_tool_edit(std::nullopt);
        else if (const auto* const edit_intent { message.get<edit_tool_intent>() }; edit_intent != nullptr)
            start_tool_edit(edit_intent->index);
        else if (const auto* const remove_intent { message.get<remove_tool_intent>() }; remove_intent != nullptr)
            remove_tool(remove_intent->index);
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
        return build_settings_view(inputs);
    }
} // namespace cyella
