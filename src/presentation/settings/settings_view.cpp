#include "presentation/settings/settings_view.h"

#include "domain/path_syntax.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace cyella {
    namespace {
        constexpr std::size_t maximum_key_name_length { 64 };
        // 한 화면에 보이는 agent 줄의 상한이다. 넘으면 "외 N개"로 줄인다.
        constexpr std::size_t maximum_identity_lines { 6 };

        std::u8string trimmed(std::u8string_view text)
        {
            const auto space = [](const char8_t value) { return value == u8' ' || value == u8'\t' || value == u8'\r' || value == u8'\n'; };
            while (text.empty() == false && space(text.front()))
                text.remove_prefix(1);
            while (text.empty() == false && space(text.back()))
                text.remove_suffix(1);
            return std::u8string { text };
        }

        bool is_separator(const char8_t value) noexcept
        {
            return value == u8',' || value == u8';' || value == u8' ' || value == u8'\t' || value == u8'\r' || value == u8'\n';
        }

        std::u8string number_text(const std::uint64_t value)
        {
            const std::string text { std::to_string(value) };
            return std::u8string { text.begin(), text.end() };
        }

        bool is_word_character(const char8_t value, const bool allow_underscore) noexcept
        {
            const auto narrow { static_cast<unsigned char>(value) };
            return std::isalnum(narrow) != 0 || value == u8'.' || value == u8'-' || (allow_underscore && value == u8'_');
        }

        bool contains(const std::u8string_view text, const std::u8string_view needle) noexcept
        {
            return text.find(needle) != std::u8string_view::npos;
        }

        std::u8string first_line(const std::u8string_view text)
        {
            const std::size_t end { text.find(u8'\n') };
            std::u8string line { text.substr(0, end) };
            while (line.empty() == false && (line.back() == u8'\r' || line.back() == u8' '))
                line.pop_back();
            return line;
        }

        // ------------------------------------------------------------------ SSH 화면 조각

        std::u8string service_status_text(const ssh_agent_service_status status)
        {
            switch (status)
            {
            case ssh_agent_service_status::running:
                return u8"실행 중";
            case ssh_agent_service_status::stopped:
                return u8"중지됨";
            case ssh_agent_service_status::disabled:
                return u8"사용 안 함";
            case ssh_agent_service_status::not_installed:
                return u8"설치되지 않음";
            }
            return u8"알 수 없음";
        }

        tone service_tone_of(const ssh_agent_service_status status) noexcept
        {
            switch (status)
            {
            case ssh_agent_service_status::running:
                return tone::good;
            case ssh_agent_service_status::stopped:
            case ssh_agent_service_status::disabled:
                return tone::warning;
            case ssh_agent_service_status::not_installed:
                break;
            }
            return tone::bad;
        }

        std::u8string service_help_text(const ssh_agent_service_status status)
        {
            switch (status)
            {
            case ssh_agent_service_status::running:
                return u8"passphrase를 한 번 풀어 둔 키를 agent가 기억해 다시 묻지 않습니다.";
            case ssh_agent_service_status::stopped:
                return u8"시작하려면 관리자 권한의 PowerShell에서 Start-Service ssh-agent를 실행하세요.";
            case ssh_agent_service_status::disabled:
                return u8"시작 유형이 '사용 안 함'입니다. 관리자 권한으로 Set-Service ssh-agent -StartupType Manual 뒤 시작하세요.";
            case ssh_agent_service_status::not_installed:
                return u8"Windows의 선택적 기능 'OpenSSH 클라이언트'를 설치하면 서비스가 생깁니다.";
            }
            return {};
        }

        void fill_agent(ssh_view& view, const ssh_info_snapshot& info)
        {
            if (info.agent_list_available == false)
            {
                view.agent_message = u8"ssh-add를 찾지 못해 agent의 키를 조회하지 못했습니다.";
                return;
            }
            switch (info.agent.state)
            {
            case ssh_agent_list_state::identities: {
                for (const ssh_agent_identity& identity : info.agent.identities)
                {
                    if (view.identities.size() == maximum_identity_lines)
                        break;
                    std::u8string line { identity.fingerprint };
                    if (identity.comment.empty() == false)
                        line += u8"  " + identity.comment;
                    if (identity.key_type.empty() == false)
                        line += u8"  (" + identity.key_type + u8")";
                    view.identities.push_back(std::move(line));
                }
                if (info.agent.identities.size() > view.identities.size())
                    view.identities.push_back(u8"외 " + number_text(info.agent.identities.size() - view.identities.size()) + u8"개");
                return;
            }
            case ssh_agent_list_state::no_identities:
                view.agent_message = u8"agent는 떠 있지만 등록된 키가 없습니다. ssh-add로 키를 더할 수 있습니다.";
                return;
            case ssh_agent_list_state::agent_unavailable:
                view.agent_message = u8"agent에 연결하지 못했습니다. ssh-agent 서비스를 시작한 뒤 [새로고침]을 누르세요.";
                return;
            case ssh_agent_list_state::failed:
                break;
            }
            view.agent_message = u8"agent의 키 목록을 읽지 못했습니다.";
        }

        tone tone_of(const ssh_connection_result result) noexcept
        {
            switch (result)
            {
            case ssh_connection_result::authenticated:
                return tone::good;
            case ssh_connection_result::host_key_rejected:
            case ssh_connection_result::unknown:
                return tone::warning;
            case ssh_connection_result::permission_denied:
            case ssh_connection_result::host_key_changed:
            case ssh_connection_result::connection_failed:
                break;
            }
            return tone::bad;
        }

        task_view make_task_view(const ssh_task_input& input, const std::u8string_view running_text)
        {
            task_view view {};
            view.id = input.id;
            if (input.id == 0 || input.state.has_value() == false)
                return view;
            view.cancellable = input.cancellable;
            switch (*input.state)
            {
            case operation_state::queued:
            case operation_state::running:
                view.state = task_view::phase::running;
                view.text = std::u8string { running_text };
                return view;
            case operation_state::needs_input:
                view.state = task_view::phase::waiting_for_input;
                view.text = input.waiting_for_prompt ? u8"입력을 기다립니다. 인증 대화 상자에서 passphrase나 호스트 키 확인에 답하세요." : u8"입력이 필요합니다.";
                return view;
            case operation_state::succeeded:
                view.state = task_view::phase::finished;
                view.result_tone = tone::good;
                view.text = input.summary;
                if (input.classification.has_value())
                {
                    view.result_tone = tone_of(input.classification->result);
                    view.text = describe_ssh_connection(*input.classification);
                }
                return view;
            case operation_state::cancelled:
                view.state = task_view::phase::finished;
                view.text = u8"취소했습니다.";
                return view;
            case operation_state::failed:
                break;
            }
            view.state = task_view::phase::finished;
            view.result_tone = tone::bad;
            view.text = input.failure_summary.empty() ? u8"작업이 실패했습니다." : input.failure_summary;
            view.detail = first_line(input.failure_detail);
            return view;
        }

        std::u8string join_path(std::u8string directory, const std::u8string_view name)
        {
            if (directory.empty())
                return std::u8string { name };
            if (directory.back() != u8'\\' && directory.back() != u8'/')
                directory.push_back(u8'\\');
            directory.append(name);
            return directory;
        }

        void fill_ssh(ssh_view& view, const settings_view_inputs& inputs)
        {
            const settings_state& state { *inputs.state };
            view.loading = state.ssh_loading;
            view.loaded = state.ssh.has_value();
            view.tools_found = inputs.ssh_tools_found;
            view.tools_origin = inputs.ssh_tools_origin;
            view.ssh_directory = inputs.ssh_directory;
            if (state.ssh.has_value())
            {
                const ssh_info_snapshot& info { *state.ssh };
                view.service_status = service_status_text(info.service);
                view.service_tone = service_tone_of(info.service);
                view.service_help = service_help_text(info.service);
                fill_agent(view, info);
                for (const ssh_key_entry& key : info.keys)
                {
                    ssh_key_row_view row {};
                    row.name = key.name;
                    row.detail = key.key_type;
                    if (key.comment.empty() == false)
                        row.detail += (row.detail.empty() ? u8"" : u8" · ") + key.comment;
                    row.can_copy = key.public_key_line.empty() == false;
                    view.keys.push_back(std::move(row));
                }
            }

            const auto composition { inputs.composition };
            view.key_name = luil::make_text_input_view(state.key_name, composition, to_luil_target(text_target::settings_key_name));
            view.key_comment = luil::make_text_input_view(state.key_comment, composition, to_luil_target(text_target::settings_key_comment));
            view.test_host = luil::make_text_input_view(state.test_host, composition, to_luil_target(text_target::settings_test_host));

            const std::u8string name { trimmed(view.key_name.displayed_text()) };
            const std::u8string comment { trimmed(view.key_comment.displayed_text()) };
            view.keygen_error = check_key_name(name);
            if (view.keygen_error.empty())
            {
                const std::u8string path { join_path(inputs.ssh_directory, name) };
                const auto has_name = [&name](const ssh_key_row_view& row) { return row.name == name; };
                const bool listed { std::ranges::any_of(view.keys, has_name) };
                const bool on_disk { inputs.probe != nullptr && inputs.ssh_directory.empty() == false && inputs.probe->probe(path) != path_kind::missing };
                const bool exists { listed || on_disk };
                if (exists)
                    view.keygen_error = u8"같은 이름의 키가 이미 있습니다. 다른 이름을 고르세요.";
                view.keygen_command = u8"ssh-keygen -t ed25519 -C \"" + comment + u8"\" -f \"" + path + u8"\"";
            }
            view.keygen = make_task_view(inputs.keygen, u8"ssh-keygen을 실행하는 중…");
            const bool keygen_busy { view.keygen.state == task_view::phase::running || view.keygen.state == task_view::phase::waiting_for_input };
            view.can_create = inputs.ssh_tools_found && inputs.ssh_directory.empty() == false && view.keygen_error.empty() && keygen_busy == false;

            const std::optional<ssh_target> target { parse_ssh_target(view.test_host.displayed_text()) };
            if (target.has_value() == false)
                view.test_error
                    = trimmed(view.test_host.displayed_text()).empty() ? u8"시험할 호스트를 입력하세요 (예: github.com)." : u8"호스트 형식이 올바르지 않습니다. [계정@]호스트[:포트]로 쓰세요.";
            else
            {
                view.test_command
                    = u8"ssh -T -o StrictHostKeyChecking=ask " + (target->port != 0 ? u8"-p " + number_text(target->port) + u8" " : std::u8string {}) + target->user_name + u8"@" + target->host;
            }
            view.test = make_task_view(inputs.test, u8"연결을 시험하는 중…");
            const bool test_busy { view.test.state == task_view::phase::running || view.test.state == task_view::phase::waiting_for_input };
            view.can_test = inputs.ssh_tools_found && target.has_value() && test_busy == false;
        }

        // ------------------------------------------------------------------ 일반·diff 도구

        git_settings_view make_git_view(const settings_view_inputs& inputs)
        {
            const settings_state& state { *inputs.state };
            git_settings_view view {};
            view.path = luil::make_text_input_view(state.git_path, inputs.composition, to_luil_target(text_target::settings_git_path));
            view.configured = inputs.settings->git_executable.empty() == false;
            if (inputs.git_tool != nullptr)
            {
                const git_tool_info& tool { *inputs.git_tool };
                view.executable = tool.executable;
                view.version = tool.reported_version;
                view.origin = tool.manually_configured ? u8"직접 지정" : u8"자동 탐색";
                if (tool.usable() == false)
                    view.problem = std::u8string { git_tool_unavailable_message(tool.availability) };
            }
            const std::u8string draft { trimmed(view.path.displayed_text()) };
            view.validation = check_git_path(draft, inputs.probe);
            view.dirty = draft != inputs.settings->git_executable;
            view.can_apply = view.dirty && view.validation.empty();
            return view;
        }

        std::vector<tool_row_view> make_tool_rows(const app_settings& settings)
        {
            std::vector<tool_row_view> rows {};
            for (std::size_t index { 0 }; index < settings.external_diff_tools.size(); ++index)
            {
                const external_diff_tool& tool { settings.external_diff_tools[index] };
                rows.push_back({ index, format_extension_list(tool.extensions), tool.executable, tool.arguments });
            }
            return rows;
        }

        tool_edit_view make_edit_view(const settings_view_inputs& inputs, const tool_edit_state& edit)
        {
            tool_edit_view view {};
            view.is_new = edit.index.has_value() == false;
            view.extensions = luil::make_text_input_view(edit.extensions, inputs.composition, to_luil_target(text_target::settings_tool_extensions));
            view.executable = luil::make_text_input_view(edit.executable, inputs.composition, to_luil_target(text_target::settings_tool_executable));
            view.arguments = luil::make_text_input_view(edit.arguments, inputs.composition, to_luil_target(text_target::settings_tool_arguments));
            const tool_draft_check check { check_tool_draft(view.extensions.displayed_text(), view.executable.displayed_text(), view.arguments.displayed_text(), inputs.probe) };
            view.error = check.error;
            view.warning = check.warning;
            view.preview = check.preview;
            view.preview_arguments = check.preview_arguments;
            view.can_save = check.ok;
            return view;
        }
    } // namespace

    // ------------------------------------------------------------------ 보조 함수

    std::vector<std::u8string> parse_extension_list(const std::u8string_view text)
    {
        std::vector<std::u8string> extensions {};
        std::size_t position { 0 };
        while (position < text.size())
        {
            while (position < text.size() && is_separator(text[position]))
                ++position;
            const std::size_t begin { position };
            while (position < text.size() && is_separator(text[position]) == false)
                ++position;
            if (begin == position)
                break;
            std::u8string normalized { normalize_diff_extension(text.substr(begin, position - begin)) };
            if (normalized.empty() == false && std::ranges::find(extensions, normalized) == extensions.end())
                extensions.push_back(std::move(normalized));
        }
        return extensions;
    }

    std::u8string format_extension_list(const std::vector<std::u8string>& extensions)
    {
        std::u8string text {};
        for (const std::u8string& extension : extensions)
        {
            if (text.empty() == false)
                text += u8", ";
            text += extension;
        }
        return text;
    }

    external_diff_values sample_external_diff_values(const std::u8string_view extension)
    {
        const std::u8string file { u8"sample" + (extension.empty() ? std::u8string { u8".png" } : std::u8string { extension }) };
        external_diff_values values {};
        values.left = u8"C:\\Temp\\cyella-diff\\1\\left\\" + file;
        values.right = u8"C:\\Temp\\cyella-diff\\1\\right\\" + file;
        values.path = u8"art/" + file;
        return values;
    }

    std::u8string format_command_preview(const std::u8string_view executable, const std::vector<std::u8string>& arguments)
    {
        const auto quoted = [](const std::u8string_view value) {
            if (value.empty() || value.find(u8' ') != std::u8string_view::npos)
                return u8"\"" + std::u8string { value } + u8"\"";
            return std::u8string { value };
        };
        std::u8string line { quoted(executable) };
        for (const std::u8string& argument : arguments)
            line += u8" " + quoted(argument);
        return line;
    }

    tool_draft_check check_tool_draft(const std::u8string_view extensions, const std::u8string_view executable, const std::u8string_view arguments, const file_probe* const probe)
    {
        tool_draft_check check {};
        check.extensions = parse_extension_list(extensions);
        const std::u8string program { trimmed(executable) };
        const std::u8string argument_template { trimmed(arguments) };

        const external_diff_values values { sample_external_diff_values(check.extensions.empty() ? std::u8string_view {} : std::u8string_view { check.extensions.front() }) };
        if (std::optional<std::vector<std::u8string>> expanded { expand_external_diff_arguments(argument_template, values) }; expanded.has_value())
        {
            check.preview_arguments = std::move(*expanded);
            check.preview = format_command_preview(program.empty() ? std::u8string_view { u8"<실행 파일>" } : std::u8string_view { program }, check.preview_arguments);
        }
        else
            check.error = u8"인자 템플릿의 큰따옴표가 닫히지 않았습니다.";

        if (check.extensions.empty())
            check.error = u8"확장자를 하나 이상 입력하세요 (예: .png, .psd).";
        else if (program.empty())
            check.error = u8"실행 파일을 입력하세요.";

        if (check.error.empty())
        {
            if (is_absolute_windows_path(program) && probe != nullptr && probe->probe(program) != path_kind::file)
                check.warning = u8"실행 파일을 찾지 못했습니다. 경로를 확인하세요 (저장은 됩니다).";
            else if (contains(argument_template, u8"{left}") == false && contains(argument_template, u8"{right}") == false)
                check.warning = u8"인자에 {left}나 {right}가 없어 도구가 비교할 파일을 받지 못합니다.";
        }
        check.ok = check.error.empty();
        return check;
    }

    external_diff_tool make_external_tool(const std::u8string_view extensions, const std::u8string_view executable, const std::u8string_view arguments)
    {
        external_diff_tool tool {};
        tool.extensions = parse_extension_list(extensions);
        tool.executable = trimmed(executable);
        tool.arguments = trimmed(arguments);
        return tool;
    }

    std::u8string check_git_path(const std::u8string_view text, const file_probe* const probe)
    {
        const std::u8string path { trimmed(text) };
        if (path.empty())
            return {};
        if (is_absolute_windows_path(path) == false)
            return u8"절대 경로여야 합니다 (예: C:\\Program Files\\Git\\cmd\\git.exe).";
        if (probe != nullptr && probe->probe(path) != path_kind::file)
            return u8"그 경로에 파일이 없습니다.";
        return {};
    }

    std::optional<ssh_target> parse_ssh_target(std::u8string_view text)
    {
        std::u8string value { trimmed(text) };
        constexpr std::u8string_view scheme { u8"ssh://" };
        if (value.starts_with(scheme))
            value.erase(0, scheme.size());
        if (value.empty())
            return std::nullopt;

        ssh_target target {};
        std::u8string host_part { value };
        if (const std::size_t at { value.find(u8'@') }; at != std::u8string::npos)
        {
            target.user_name = value.substr(0, at);
            host_part = value.substr(at + 1);
        }
        if (const std::size_t colon { host_part.rfind(u8':') }; colon != std::u8string::npos)
        {
            const std::u8string port_text { host_part.substr(colon + 1) };
            host_part.erase(colon);
            if (port_text.empty() || port_text.size() > 5 || std::ranges::all_of(port_text, [](const char8_t digit) { return digit >= u8'0' && digit <= u8'9'; }) == false)
                return std::nullopt;
            const unsigned long port { std::stoul(std::string { port_text.begin(), port_text.end() }) };
            if (port == 0 || port > 65535)
                return std::nullopt;
            target.port = static_cast<std::uint16_t>(port);
        }
        target.host = host_part;

        const auto valid = [](const std::u8string_view token, const bool allow_underscore) {
            return token.empty() == false && token.front() != u8'-'
                && std::ranges::all_of(token, [allow_underscore](const char8_t character) { return is_word_character(character, allow_underscore); });
        };
        if (valid(target.host, false) == false || valid(target.user_name, true) == false)
            return std::nullopt;
        return target;
    }

    std::u8string check_key_name(const std::u8string_view name)
    {
        if (name.empty())
            return u8"파일 이름을 입력하세요 (예: id_ed25519).";
        if (name.size() > maximum_key_name_length)
            return u8"파일 이름이 너무 깁니다.";
        if (name.front() == u8'-' || name.front() == u8'.')
            return u8"파일 이름은 '-'나 '.'로 시작할 수 없습니다.";
        if (std::ranges::all_of(name, [](const char8_t character) { return is_word_character(character, true); }) == false)
            return u8"파일 이름에는 영문, 숫자, '.', '_', '-'만 쓸 수 있습니다.";
        if (name.ends_with(u8".pub"))
            return u8"개인 키 이름은 .pub로 끝날 수 없습니다. 공개 키는 .pub가 자동으로 붙습니다.";
        return {};
    }

    // ------------------------------------------------------------------ snapshot

    settings_view build_settings_view(const settings_view_inputs& inputs)
    {
        settings_view view {};
        const settings_state& state { *inputs.state };
        view.page = state.page;
        view.scroll = state.scroll;
        view.read_only = inputs.read_only;
        view.unsaved = inputs.unsaved;
        view.message = state.message;
        view.theme = inputs.settings->appearance.theme;
        view.diff = inputs.settings->diff;
        view.git = make_git_view(inputs);
        view.tools = make_tool_rows(*inputs.settings);
        if (state.tool_edit.has_value())
            view.edit = make_edit_view(inputs, *state.tool_edit);
        fill_ssh(view.ssh, inputs);
        fill_hosting_page(view.hosting, inputs);
        return view;
    }
} // namespace cyella
