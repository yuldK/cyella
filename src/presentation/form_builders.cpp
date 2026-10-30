#include "presentation/form_builders.h"

#include "domain/hosting_remote.h"
#include "domain/ref_name.h"
#include "domain/ssh_remote.h"
#include "domain/text_catalog.h"
#include "presentation/signing_view.h"
#include "presentation/ui_state.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace cyella {
    namespace {
        // 파일 목록은 이만큼만 보이고 나머지는 "외 N개"다.
        constexpr std::size_t listed_file_limit { 5 };

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::u8string number(const std::uint64_t value)
        {
            return to_u8(std::to_string(value));
        }

        std::u8string short_sha(const std::string_view sha)
        {
            return to_u8(sha.substr(0, std::min<std::size_t>(sha.size(), 7)));
        }

        std::u8string problem_text(const ref_name_problem problem)
        {
            switch (problem)
            {
            case ref_name_problem::empty:
                return tr(u8"form.ref_name.empty");
            case ref_name_problem::leading_dash:
                return tr(u8"form.ref_name.leading_dash");
            case ref_name_problem::leading_slash:
            case ref_name_problem::trailing_slash:
            case ref_name_problem::consecutive_slashes:
                return tr(u8"form.ref_name.slash");
            case ref_name_problem::component_starts_with_dot:
                return tr(u8"form.ref_name.component_dot");
            case ref_name_problem::component_ends_with_lock:
                return tr(u8"form.ref_name.component_lock");
            case ref_name_problem::trailing_dot:
                return tr(u8"form.ref_name.trailing_dot");
            case ref_name_problem::contains_double_dot:
                return tr(u8"form.ref_name.double_dot");
            case ref_name_problem::contains_at_brace:
                return tr(u8"form.ref_name.at_brace");
            case ref_name_problem::is_single_at:
                return tr(u8"form.ref_name.single_at");
            case ref_name_problem::is_head:
                return tr(u8"form.ref_name.head");
            case ref_name_problem::forbidden_character:
                return tr(u8"form.ref_name.forbidden_character");
            case ref_name_problem::control_character:
                return tr(u8"form.ref_name.control_character");
            case ref_name_problem::backslash:
                return tr(u8"form.ref_name.backslash");
            }
            return tr(u8"form.ref_name.invalid");
        }

        const luil::text::text_edit_state& field_state(const form_inputs& inputs, const std::size_t index)
        {
            return inputs.fields[std::min<std::size_t>(index, inputs.fields.size() - 1)];
        }

        std::string field_text(const form_inputs& inputs, const std::size_t index)
        {
            return trimmed_text(field_state(inputs, index).text);
        }

        form_field make_field(const form_build_context& context, const std::size_t index, std::u8string label, std::u8string placeholder, const bool secret = false, const bool focus = false)
        {
            static const std::optional<luil::text_composition_event> none {};
            const std::optional<luil::text_composition_event>& composition { context.composition != nullptr ? *context.composition : none };
            form_field field {};
            field.label = std::move(label);
            field.view
                = luil::make_text_input_view(field_state(*context.inputs, index), composition, to_luil_target(static_cast<text_target>(static_cast<std::uint32_t>(text_target::form_field_0) + index)));
            field.placeholder = std::move(placeholder);
            field.secret = secret;
            field.reveal = secret && context.inputs->reveal;
            field.focus = focus;
            return field;
        }

        form_button make_button(std::u8string key, std::u8string label, const form_tone tone = form_tone::normal, const bool enabled = true, const bool default_button = false)
        {
            return form_button { .key = std::move(key), .label = std::move(label), .tone = tone, .enabled = enabled, .default_button = default_button };
        }

        const ref* find_local(const repository_snapshot& snapshot, const std::string& name)
        {
            for (const ref& value : snapshot.refs)
                if (value.kind == ref_kind::local && value.name == name)
                    return &value;
            return nullptr;
        }
    } // namespace

    std::string trimmed_text(const std::u8string_view text)
    {
        const auto space = [](const char8_t value) { return value == u8' ' || value == u8'\t' || value == u8'\r' || value == u8'\n'; };
        std::u8string_view view { text };
        while (view.empty() == false && space(view.front()))
            view.remove_prefix(1);
        while (view.empty() == false && space(view.back()))
            view.remove_suffix(1);
        return std::string { view.begin(), view.end() };
    }

    std::optional<std::u8string> branch_name_error(const std::string_view name, const repository_snapshot& snapshot)
    {
        if (const std::optional<ref_name_problem> problem { find_ref_name_problem(name) }; problem.has_value())
            return problem_text(*problem);
        if (find_local(snapshot, std::string { name }) != nullptr)
            return tr(u8"form.branch_exists", { { u8"name", name } });
        return std::nullopt;
    }

    std::optional<std::u8string> tag_name_error(const std::string_view name, const repository_snapshot& snapshot)
    {
        if (const std::optional<ref_name_problem> problem { find_ref_name_problem(name) }; problem.has_value())
            return problem_text(*problem);
        for (const ref& value : snapshot.refs)
            if (value.kind == ref_kind::tag && value.name == name)
                return tr(u8"form.tag_exists", { { u8"name", name } });
        return std::nullopt;
    }

    std::string dialog_identity(const dialog_state& dialog)
    {
        return "dialog:" + std::to_string(dialog.serial);
    }

    std::string auth_identity(const operation_status& status)
    {
        const std::uint64_t prompt { status.needs_input.has_value() ? status.needs_input->prompt_id : 0 };
        return "auth:" + std::to_string(status.id) + ":" + std::to_string(prompt);
    }

    std::string dirty_identity(const operation_status& status)
    {
        return "dirty:" + std::to_string(status.id);
    }

    // ---------------------------------------------------------------- 브랜치

    bool create_branch_ready(const dialog_state&, const form_inputs& inputs, const repository_snapshot& snapshot)
    {
        const std::string name { field_text(inputs, 0) };
        return branch_name_error(name, snapshot).has_value() == false;
    }

    form_view build_create_branch_form(const dialog_state& dialog, const form_build_context& context)
    {
        const repository_snapshot& snapshot { *context.snapshot };
        const std::string name { field_text(*context.inputs, 0) };
        form_view form {};
        form.id = u8"branch-dialog";
        form.title = tr(u8"form.create_branch.title");
        form.notes.push_back({ .text = tr(u8"form.create_branch.at_note", { { u8"target", dialog.subject_label } }) });
        form.fields.push_back(make_field(context, 0, tr(u8"form.create_branch.name_label"), u8"feature/my-work", false, true));
        form.checks.push_back({ .label = tr(u8"form.create_branch.checkout"), .checked = dialog.flag_a });
        if (name.empty() == false)
            if (const auto error { branch_name_error(name, snapshot) }; error.has_value())
                form.error = *error;
        const std::string at { dialog.subject.substr(0, 12) };
        const std::string shown_name { name.empty() ? tr_narrow(u8"form.preview_name") : name };
        form.preview = to_u8((dialog.flag_a ? "git switch -c " : "git branch ") + shown_name + (at.empty() ? std::string {} : " " + at));
        form.buttons.push_back(make_button(u8"cancel", tr(u8"form.cancel")));
        form.buttons.push_back(make_button(u8"submit", tr(u8"form.create"), form_tone::accent, create_branch_ready(dialog, *context.inputs, snapshot), true));
        form.dismiss_key = u8"cancel";
        return form;
    }

    bool rename_branch_ready(const dialog_state& dialog, const form_inputs& inputs, const repository_snapshot& snapshot)
    {
        const std::string name { field_text(inputs, 0) };
        return name != dialog.subject && branch_name_error(name, snapshot).has_value() == false;
    }

    form_view build_rename_branch_form(const dialog_state& dialog, const form_build_context& context)
    {
        const repository_snapshot& snapshot { *context.snapshot };
        const std::string name { field_text(*context.inputs, 0) };
        form_view form {};
        form.id = u8"rename-dialog";
        form.title = tr(u8"form.rename_branch.title");
        form.notes.push_back({ .text = tr(u8"form.rename_branch.current_note", { { u8"name", dialog.subject } }) });
        form.fields.push_back(make_field(context, 0, tr(u8"form.rename_branch.new_name_label"), u8"", false, true));
        if (name.empty() == false && name != dialog.subject)
            if (const auto error { branch_name_error(name, snapshot) }; error.has_value())
                form.error = *error;
        form.preview = u8"git branch -m " + to_u8(dialog.subject) + u8" " + (name.empty() ? tr(u8"form.rename_branch.preview_new_name") : to_u8(name));
        form.buttons.push_back(make_button(u8"cancel", tr(u8"form.cancel")));
        form.buttons.push_back(make_button(u8"submit", tr(u8"form.rename_branch.submit"), form_tone::accent, rename_branch_ready(dialog, *context.inputs, snapshot), true));
        form.dismiss_key = u8"cancel";
        return form;
    }

    // ---------------------------------------------------------------- 태그

    bool create_tag_ready(const dialog_state& dialog, const form_inputs& inputs, const repository_snapshot& snapshot)
    {
        if (tag_name_error(field_text(inputs, 0), snapshot).has_value())
            return false;
        return dialog.choice == 0 || field_text(inputs, 1).empty() == false;
    }

    form_view build_create_tag_form(const dialog_state& dialog, const form_build_context& context)
    {
        const repository_snapshot& snapshot { *context.snapshot };
        const std::string name { field_text(*context.inputs, 0) };
        const bool annotated { dialog.choice == 1 };
        form_view form {};
        form.id = u8"tag-dialog";
        form.title = tr(u8"form.create_tag.title");
        form.notes.push_back({ .text = tr(u8"form.create_tag.target_note", { { u8"target", dialog.subject_label } }) });
        form.fields.push_back(make_field(context, 0, tr(u8"form.create_tag.name_label"), u8"v1.0.0", false, true));
        form.choices_label = tr(u8"form.create_tag.kind_label");
        form.choices.push_back({ .label = tr(u8"form.create_tag.lightweight") });
        form.choices.push_back({ .label = tr(u8"form.create_tag.annotated") });
        form.chosen = dialog.choice;
        // 서명 토글이다. 기본값은 설정 `tag.gpgsign`이다.
        form.checks.push_back({ .label = tr(u8"form.create_tag.sign"), .checked = dialog.flag_b });
        if (dialog.flag_b)
            if (const std::u8string hint { signing_setup_hint(snapshot.signing, true) }; hint.empty() == false)
                form.warnings.push_back({ .text = hint, .warning = true });
        if (annotated)
            form.fields.push_back(make_field(context, 1, tr(u8"form.create_tag.message_label"), tr(u8"form.create_tag.message_placeholder")));
        if (name.empty() == false)
            if (const auto error { tag_name_error(name, snapshot) }; error.has_value())
                form.error = *error;
        if (annotated && name.empty() == false && field_text(*context.inputs, 1).empty() && form.error.empty())
            form.error = tr(u8"form.create_tag.message_required");
        const std::string at { dialog.subject.empty() ? std::string {} : dialog.subject.substr(0, 12) };
        const std::string shown_name { name.empty() ? tr_narrow(u8"form.preview_name") : name };
        std::string preview { "git tag " };
        preview += annotated ? (dialog.flag_b ? "-s " : "-a ") + shown_name + " -m \"…\"" : shown_name;
        if (at.empty() == false)
            preview += " " + at;
        form.preview = to_u8(preview);
        form.buttons.push_back(make_button(u8"cancel", tr(u8"form.cancel")));
        form.buttons.push_back(make_button(u8"submit", tr(u8"form.create_tag.submit"), form_tone::accent, create_tag_ready(dialog, *context.inputs, snapshot), true));
        form.dismiss_key = u8"cancel";
        return form;
    }

    // ---------------------------------------------------------------- push

    bool push_ready(const dialog_state& dialog, const form_inputs&, const repository_snapshot&)
    {
        return dialog.choices.empty() == false && dialog.subject.empty() == false;
    }

    push_execute_request make_push_request(const dialog_state& dialog, const form_inputs& inputs)
    {
        push_execute_request request {};
        if (dialog.choice < dialog.choices.size())
            request.remote = dialog.choices[dialog.choice];
        request.branch = dialog.subject;
        const std::string remote_branch { field_text(inputs, 0) };
        request.remote_branch = remote_branch.empty() ? dialog.subject : remote_branch;
        request.set_upstream = dialog.flag_a;
        request.follow_tags = dialog.flag_b;
        request.force_with_lease = dialog.flag_c;
        return request;
    }

    form_view build_push_form(const dialog_state& dialog, const form_build_context& context)
    {
        const repository_snapshot& snapshot { *context.snapshot };
        form_view form {};
        form.id = u8"push-dialog";
        form.title = u8"Push";
        const ref* const branch { find_local(snapshot, dialog.subject) };
        form.notes.push_back(
            { .text = tr(u8"form.push.branch_note", { { u8"branch", dialog.subject } }) + (branch != nullptr && branch->ahead > 0 ? u8"  (↑" + number(branch->ahead) + u8")" : std::u8string {}) });
        form.choices_label = tr(u8"form.remote_label");
        for (const std::string& remote : dialog.choices)
        {
            std::u8string label { to_u8(remote) };
            for (const remote_info& info : snapshot.summary.remotes)
            {
                if (info.name != remote)
                    continue;
                const std::string& url { info.push_url.empty() ? info.fetch_url : info.push_url };
                if (url.empty() == false)
                    label += u8"   " + to_u8(url);
            }
            form.choices.push_back({ .label = std::move(label) });
        }
        form.chosen = dialog.choice;
        if (dialog.choices.empty())
            form.error = tr(u8"form.no_remote");
        form.fields.push_back(make_field(context, 0, tr(u8"form.remote_branch_label"), to_u8(dialog.subject), false, true));
        form.checks.push_back({ .label = tr(u8"form.push.set_upstream"), .checked = dialog.flag_a });
        form.checks.push_back({ .label = tr(u8"form.push.follow_tags"), .checked = dialog.flag_b });
        form.checks.push_back({ .label = tr(u8"form.push.force"), .checked = dialog.flag_c });
        if (dialog.flag_c)
            form.warnings.push_back({ .text = tr(u8"form.push.force_warning"), .warning = true });
        if (branch != nullptr && branch->behind > 0 && dialog.flag_c == false)
        {
            std::u8string text { tr(u8"form.push.behind_warning", { { u8"count", branch->behind } }) };
            if (branch->behind == 1)
                text = tr(u8"form.push.behind_warning_one", { { u8"count", branch->behind } });
            form.warnings.push_back({ .text = std::move(text), .warning = true });
        }
        const push_execute_request request { make_push_request(dialog, *context.inputs) };
        std::string preview { "git push" };
        if (request.set_upstream)
            preview += " --set-upstream";
        if (request.follow_tags)
            preview += " --follow-tags";
        if (request.force_with_lease)
            preview += " --force-with-lease";
        preview += " " + (request.remote.empty() ? tr_narrow(u8"form.preview_remote") : request.remote) + " " + request.branch + ":" + request.remote_branch;
        form.preview = to_u8(preview);
        form.buttons.push_back(make_button(u8"cancel", tr(u8"form.cancel")));
        form.buttons.push_back(make_button(u8"submit", dialog.flag_c ? tr(u8"form.push.force_submit") : std::u8string { u8"Push" }, dialog.flag_c ? form_tone::danger : form_tone::accent,
            push_ready(dialog, *context.inputs, snapshot), true));
        form.dismiss_key = u8"cancel";
        return form;
    }

    // ---------------------------------------------------------------- upstream 설정

    set_upstream_request make_set_upstream_request(const dialog_state& dialog, const form_inputs& inputs)
    {
        set_upstream_request request {};
        request.branch = dialog.subject;
        if (dialog.choice < dialog.choices.size())
            request.remote = dialog.choices[dialog.choice];
        const std::string remote_branch { field_text(inputs, 0) };
        request.remote_branch = remote_branch.empty() ? dialog.subject : remote_branch;
        return request;
    }

    std::optional<std::u8string> set_upstream_error(const dialog_state& dialog, const form_inputs& inputs, const repository_snapshot& snapshot)
    {
        if (dialog.choices.empty())
            return tr(u8"form.no_remote");
        const set_upstream_request request { make_set_upstream_request(dialog, inputs) };
        if (const std::optional<ref_name_problem> problem { find_ref_name_problem(request.remote_branch) }; problem.has_value())
            return problem_text(*problem);
        const std::string tracking { request.remote + "/" + request.remote_branch };
        for (const ref& value : snapshot.refs)
            if (value.kind == ref_kind::remote && value.name == tracking)
                return std::nullopt;
        return tr(u8"form.set_upstream.not_fetched", { { u8"tracking", tracking } });
    }

    bool set_upstream_ready(const dialog_state& dialog, const form_inputs& inputs, const repository_snapshot& snapshot)
    {
        return dialog.subject.empty() == false && set_upstream_error(dialog, inputs, snapshot).has_value() == false;
    }

    form_view build_set_upstream_form(const dialog_state& dialog, const form_build_context& context)
    {
        const repository_snapshot& snapshot { *context.snapshot };
        form_view form {};
        form.id = u8"set-upstream-dialog";
        form.title = tr(u8"form.set_upstream.title");
        form.notes.push_back({ .text = tr(u8"form.set_upstream.branch_note", { { u8"branch", dialog.subject } }) });
        form.choices_label = tr(u8"form.remote_label");
        for (const std::string& remote : dialog.choices)
            form.choices.push_back({ .label = to_u8(remote) });
        form.chosen = dialog.choice;
        form.fields.push_back(make_field(context, 0, tr(u8"form.remote_branch_label"), to_u8(dialog.subject), false, true));
        if (const auto error { set_upstream_error(dialog, *context.inputs, snapshot) }; error.has_value())
            form.error = *error;
        const set_upstream_request request { make_set_upstream_request(dialog, *context.inputs) };
        form.preview = to_u8("git branch --set-upstream-to=" + (request.remote.empty() ? tr_narrow(u8"form.preview_remote") : request.remote) + "/" + request.remote_branch + " " + request.branch);
        form.buttons.push_back(make_button(u8"cancel", tr(u8"form.cancel")));
        form.buttons.push_back(make_button(u8"submit", tr(u8"form.set_upstream.submit"), form_tone::accent, set_upstream_ready(dialog, *context.inputs, snapshot), true));
        form.dismiss_key = u8"cancel";
        return form;
    }

    // ---------------------------------------------------------------- 원격 추가와 게시

    std::vector<git_operation> make_add_remote_operations(const dialog_state& dialog, const form_inputs& inputs)
    {
        const std::string name { field_text(inputs, 0) };
        std::vector<git_operation> operations {};
        if (dialog.flag_d && add_remote_can_create_on_server(dialog, inputs))
            operations.push_back(create_ssh_repository_request { .url = field_text(inputs, 1) });
        operations.push_back(add_remote_request { .name = name, .url = field_text(inputs, 1) });
        if (dialog.flag_a && dialog.subject.empty() == false)
        {
            push_execute_request push {};
            push.remote = name;
            push.branch = dialog.subject;
            push.remote_branch = dialog.subject;
            push.set_upstream = true;
            operations.push_back(std::move(push));
        }
        return operations;
    }

    bool add_remote_to_hosting(const dialog_state& dialog) noexcept
    {
        return dialog.choice > 0 && dialog.choice < dialog.choices.size();
    }

    bool add_remote_can_create_on_server(const dialog_state& dialog, const form_inputs& inputs)
    {
        if (add_remote_to_hosting(dialog))
            return false;
        const std::string url { field_text(inputs, 1) };
        if (parse_ssh_remote_target(url).has_value() == false)
            return false;
        const std::optional<hosting_repository> hosting { resolve_hosting_repository(url, {}, {}) };
        return hosting.has_value() == false || hosting->provider == hosting_provider::unknown;
    }

    std::optional<std::u8string> add_remote_error(const dialog_state& dialog, const form_inputs& inputs, const repository_snapshot& snapshot)
    {
        const std::string name { field_text(inputs, 0) };
        if (const std::optional<ref_name_problem> problem { find_ref_name_problem(name) }; problem.has_value())
            return problem_text(*problem);
        for (const remote_info& remote : snapshot.summary.remotes)
            if (remote.name == name)
                return tr(u8"form.add_remote.exists", { { u8"name", name } });
        if (add_remote_to_hosting(dialog))
        {
            // 서버가 더 좁게 거절할 수 있다. 여기서는 URL 경로에 그대로 설 수 있는 글자만 받는다 (서버의 이유는 실패 알림이 보인다).
            const auto path_character = [](const char value) { return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '-' || value == '_' || value == '.'; };
            const std::string repository { field_text(inputs, 1) };
            if (std::ranges::all_of(repository, path_character) == false || repository == "." || repository == "..")
                return tr(u8"form.add_remote.repository_invalid");
            const std::string owner { field_text(inputs, 2) };
            if (std::ranges::all_of(owner, [&path_character](const char value) { return path_character(value) || value == '/'; }) == false || owner.starts_with('/') || owner.ends_with('/'))
                return tr(u8"form.add_remote.owner_invalid");
            return std::nullopt;
        }
        const std::string url { field_text(inputs, 1) };
        if (url.empty() == false && url.front() == '-')
            return tr(u8"form.add_remote.url_dash");
        if (std::ranges::any_of(url, [](const char value) { return static_cast<unsigned char>(value) < 0x20 || value == 0x7f; }))
            return tr(u8"form.add_remote.url_control");
        return std::nullopt;
    }

    bool add_remote_ready(const dialog_state& dialog, const form_inputs& inputs, const repository_snapshot& snapshot)
    {
        return field_text(inputs, 1).empty() == false && add_remote_error(dialog, inputs, snapshot).has_value() == false;
    }

    form_view build_add_remote_form(const dialog_state& dialog, const form_build_context& context)
    {
        const repository_snapshot& snapshot { *context.snapshot };
        const bool publishable { dialog.subject.empty() == false };
        const bool publish { publishable && dialog.flag_a };
        const bool hosting { add_remote_to_hosting(dialog) };
        form_view form {};
        form.id = u8"add-remote-dialog";
        form.title = publishable ? tr(u8"form.add_remote.publish_title") : tr(u8"form.add_remote.title");
        if (publishable)
            form.notes.push_back({ .text = tr(u8"form.add_remote.branch_note", { { u8"branch", dialog.subject } }) });
        if (dialog.choices.size() > 1)
        {
            form.choices_label = tr(u8"form.add_remote.where_label");
            for (std::size_t index { 0 }; index < dialog.choices.size(); ++index)
                form.choices.push_back({ .label = index < dialog.lines.size() ? dialog.lines[index] : to_u8(dialog.choices[index]) });
            form.chosen = dialog.choice;
            form.choices_first = true;
        }
        form.fields.push_back(make_field(context, 0, tr(u8"form.add_remote.name_label"), u8"origin"));
        if (hosting)
        {
            form.fields.push_back(make_field(context, 1, tr(u8"form.add_remote.repository_label"), dialog.subject_label, false, true));
            form.fields.push_back(make_field(context, 2, tr(u8"form.add_remote.owner_label"), tr(u8"form.add_remote.owner_placeholder")));
        }
        else
            form.fields.push_back(make_field(context, 1, tr(u8"form.add_remote.url_label"), u8"https://github.com/user/repo.git", false, true));
        if (publishable)
            form.checks.push_back({ .label = tr(u8"form.add_remote.push_check", { { u8"branch", dialog.subject } }), .checked = dialog.flag_a });
        const bool on_server { add_remote_can_create_on_server(dialog, *context.inputs) };
        if (on_server)
        {
            form.checks.push_back({ .label = tr(u8"form.add_remote.server_check"), .checked = dialog.flag_d });
            if (const std::optional<ssh_remote_target> target { parse_ssh_remote_target(field_text(*context.inputs, 1)) }; target.has_value() && dialog.flag_d)
                form.warnings.push_back({ .text = tr(u8"form.add_remote.server_note", { { u8"host", to_u8(target->destination()) }, { u8"path", to_u8(target->path) } }) });
        }
        if (hosting)
        {
            form.checks.push_back({ .label = tr(u8"form.add_remote.private_check"), .checked = dialog.flag_b });
            form.checks.push_back({ .label = tr(u8"form.add_remote.ssh_check"), .checked = dialog.flag_c });
            form.warnings.push_back({ .text = tr(u8"form.add_remote.hosting_note", { { u8"host", dialog.choices[dialog.choice] } }) });
        }
        if (const auto error { add_remote_error(dialog, *context.inputs, snapshot) }; error.has_value())
            form.error = *error;
        const std::string name { field_text(*context.inputs, 0) };
        const std::string remote { name.empty() ? tr_narrow(u8"form.preview_remote") : name };
        const std::string url { hosting ? tr_narrow(u8"form.add_remote.preview_new_url") : field_text(*context.inputs, 1) };
        std::string preview { "git remote add " + remote + " " + (url.empty() ? std::string { "<url>" } : url) };
        if (on_server && dialog.flag_d)
            if (const std::optional<ssh_remote_target> target { parse_ssh_remote_target(url) }; target.has_value())
                preview = "ssh " + target->destination() + " git init --bare " + target->path + " && " + preview;
        if (publish)
            preview += " && git push --set-upstream " + remote + " " + dialog.subject + ":" + dialog.subject;
        form.preview = to_u8(preview);
        form.buttons.push_back(make_button(u8"cancel", tr(u8"form.cancel")));
        std::u8string submit { publish ? tr(u8"form.add_remote.publish_submit") : tr(u8"form.add_remote.submit") };
        if (hosting)
            submit = publish ? tr(u8"form.add_remote.create_publish_submit") : tr(u8"form.add_remote.create_submit");
        form.buttons.push_back(make_button(u8"submit", submit, form_tone::accent, add_remote_ready(dialog, *context.inputs, snapshot), true));
        form.dismiss_key = u8"cancel";
        return form;
    }

    // ---------------------------------------------------------------- 확인

    form_view build_confirm_form(const dialog_state& dialog, const form_build_context&)
    {
        form_view form {};
        form.id = u8"confirm-dialog";
        form.title = dialog.title;
        for (const std::u8string& line : dialog.lines)
            form.notes.push_back({ .text = line });
        if (dialog.check_label.empty() == false)
            form.checks.push_back({ .label = dialog.check_label, .checked = dialog.flag_a });
        if (dialog.check_label.empty() == false && dialog.flag_a && dialog.check_warning)
            form.warnings.push_back({ .text = tr(u8"form.confirm_dialog.remote_tag_warning"), .warning = true });
        form.buttons.push_back(make_button(u8"cancel", tr(u8"form.cancel"), form_tone::normal, true, dialog.danger));
        form.buttons.push_back(
            make_button(u8"submit", dialog.confirm_label.empty() ? tr(u8"form.confirm") : dialog.confirm_label, dialog.danger ? form_tone::danger : form_tone::accent, true, dialog.danger == false));
        form.dismiss_key = u8"cancel";
        return form;
    }

    // ---------------------------------------------------------------- 체크아웃 정책

    form_view build_dirty_checkout_form(const operation_status& status, const dirty_checkout_state& state, const form_build_context& context)
    {
        const input_request& request { *status.needs_input };
        form_view form {};
        form.id = u8"dirty-dialog";
        form.title = tr(u8"form.dirty.title");
        if (state.confirming_discard)
        {
            form.title = tr(u8"form.dirty.discard_title");
            form.notes.push_back({ .text = tr(u8"form.dirty.discard_note", { { u8"target", request.target } }) });
            for (std::size_t index { 0 }; index < std::min(request.files.size(), listed_file_limit); ++index)
                form.notes.push_back({ .text = to_u8(request.files[index]), .code = true });
            if (request.files.size() > listed_file_limit)
                form.notes.push_back({ .text = tr(u8"form.more_files", { { u8"count", request.files.size() - listed_file_limit } }) });
            if (request.untracked_files)
                form.warnings.push_back({ .text = tr(u8"form.dirty.discard_untracked_warning"), .warning = true });
            form.buttons.push_back(make_button(u8"back", tr(u8"form.dirty.back")));
            form.buttons.push_back(make_button(u8"submit", tr(u8"form.dirty.discard_submit"), form_tone::danger));
            form.dismiss_key = u8"cancel";
            return form;
        }
        form.notes.push_back({ .text = tr(u8"form.dirty.overwrite_note", { { u8"target", request.target } }) });
        for (std::size_t index { 0 }; index < std::min(request.files.size(), listed_file_limit); ++index)
            form.notes.push_back({ .text = to_u8(request.files[index]), .code = true });
        if (request.files.size() > listed_file_limit)
            form.notes.push_back({ .text = tr(u8"form.more_files", { { u8"count", request.files.size() - listed_file_limit } }) });
        form.choices_label = tr(u8"form.dirty.choices_label");
        form.choices.push_back({ .label = tr(u8"form.dirty.choice_stash") });
        form.choices.push_back({ .label = tr(u8"form.dirty.choice_merge") });
        form.choices.push_back({ .label = tr(u8"form.dirty.choice_discard") });
        form.chosen = std::min<std::size_t>(context.inputs->choice, 2);
        switch (form.chosen)
        {
        case 0:
            form.preview = u8"git stash push -u → git switch → git stash pop";
            break;
        case 1:
            form.preview = u8"git switch --merge " + to_u8(request.target);
            break;
        default:
            form.preview = u8"git switch --discard-changes " + to_u8(request.target);
            form.warnings.push_back({ .text = tr(u8"form.dirty.discard_warning"), .warning = true });
            break;
        }
        if (request.untracked_files)
            form.warnings.push_back({ .text = tr(u8"form.dirty.untracked_warning"), .warning = true });
        form.buttons.push_back(make_button(u8"cancel", tr(u8"form.cancel")));
        form.buttons.push_back(
            make_button(u8"submit", form.chosen == 2 ? tr(u8"form.dirty.discard_next") : tr(u8"form.dirty.switch"), form.chosen == 2 ? form_tone::danger : form_tone::accent, true, true));
        form.dismiss_key = u8"cancel";
        return form;
    }

    // ---------------------------------------------------------------- 인증

    form_view build_auth_form(const operation_status& status, const form_build_context& context)
    {
        const input_request& request { *status.needs_input };
        const credential_prompt& prompt { request.prompt };
        form_view form {};
        form.id = u8"auth-dialog";
        form.dismiss_key = u8"cancel";
        const auto answer_buttons = [&form](std::u8string label) {
            form.buttons.push_back(make_button(u8"cancel", tr(u8"form.cancel")));
            form.buttons.push_back(make_button(u8"answer", std::move(label), form_tone::accent, true, true));
        };
        const auto secret_field = [&](std::u8string label) {
            form.fields.push_back(make_field(context, 0, std::move(label), u8"", true, true));
            form.can_reveal = true;
            form.checks.push_back({ .label = tr(u8"form.auth.reveal"), .checked = context.inputs->reveal });
        };
        switch (prompt.kind)
        {
        case credential_prompt_kind::ssh_key_passphrase:
            form.title = tr(u8"form.auth.ssh_passphrase_title");
            if (prompt.key_path.empty() == false)
                form.notes.push_back({ .text = tr(u8"form.auth.key_note", { { u8"path", prompt.key_path } }), .code = true });
            secret_field(u8"passphrase");
            form.notes.push_back({ .text = tr(u8"form.auth.passphrase_note") });
            answer_buttons(tr(u8"form.confirm"));
            break;
        case credential_prompt_kind::ssh_new_key_passphrase:
            form.title = tr(u8"form.auth.new_passphrase_title");
            secret_field(tr(u8"form.auth.new_passphrase_label"));
            answer_buttons(tr(u8"form.confirm"));
            break;
        case credential_prompt_kind::ssh_new_key_passphrase_confirm:
            form.title = tr(u8"form.auth.confirm_passphrase_title");
            secret_field(tr(u8"form.auth.confirm_passphrase_label"));
            answer_buttons(tr(u8"form.confirm"));
            break;
        case credential_prompt_kind::ssh_host_key_confirm:
            form.title = tr(u8"form.auth.host_key_title");
            if (prompt.address.empty())
                form.notes.push_back({ .text = tr(u8"form.auth.host_note", { { u8"host", prompt.host } }) });
            else
                form.notes.push_back({ .text = tr(u8"form.auth.host_note_with_address", { { u8"host", prompt.host }, { u8"address", prompt.address } }) });
            form.notes.push_back({ .text = tr(u8"form.auth.key_type_note", { { u8"type", prompt.key_type.empty() ? tr(u8"form.auth.key_type_unknown") : prompt.key_type } }) });
            form.notes.push_back({ .text = tr(u8"form.auth.fingerprint_note", { { u8"fingerprint", prompt.fingerprint } }), .code = true });
            form.warnings.push_back({ .text = tr(u8"form.auth.fingerprint_warning"), .warning = true });
            form.preview = tr(u8"form.auth.host_key_preview");
            form.buttons.push_back(make_button(u8"no", tr(u8"form.auth.reject"), form_tone::normal, true, true));
            form.buttons.push_back(make_button(u8"once", tr(u8"form.auth.connect_once")));
            form.buttons.push_back(make_button(u8"yes", tr(u8"form.auth.connect"), form_tone::accent));
            break;
        case credential_prompt_kind::ssh_password:
            form.title = tr(u8"form.auth.ssh_password_title");
            form.notes.push_back({ .text = (prompt.user_name.empty() ? std::u8string {} : prompt.user_name + u8"@") + prompt.host });
            secret_field(tr(u8"form.auth.password_label"));
            answer_buttons(tr(u8"form.confirm"));
            break;
        case credential_prompt_kind::https_username:
            form.title = tr(u8"form.auth.https_username_title");
            if (prompt.url.empty() == false)
                form.notes.push_back({ .text = prompt.url, .code = true });
            form.fields.push_back(make_field(context, 0, tr(u8"form.auth.username_label"), u8"", false, true));
            form.notes.push_back({ .text = tr(u8"form.auth.username_note") });
            answer_buttons(tr(u8"form.auth.next"));
            break;
        case credential_prompt_kind::https_password:
            form.title = tr(u8"form.auth.https_password_title");
            if (prompt.user_name.empty() == false || prompt.url.empty() == false)
                form.notes.push_back({ .text = (prompt.user_name.empty() ? std::u8string {} : prompt.user_name + u8" · ") + prompt.url, .code = true });
            secret_field(tr(u8"form.auth.password_or_token_label"));
            answer_buttons(tr(u8"form.confirm"));
            break;
        case credential_prompt_kind::unknown:
            form.title = tr(u8"form.auth.unknown_title");
            {
                // git이 문구 끝에 붙인 공백·줄바꿈은 보이지 않는다.
                std::u8string raw { prompt.raw_text };
                const auto blank = [](const char8_t value) { return value == u8' ' || value == u8'\n' || value == u8'\r' || value == u8'\t'; };
                while (raw.empty() == false && blank(raw.back()))
                    raw.pop_back();
                form.notes.push_back({ .text = std::move(raw), .code = true });
            }
            if (request.secret)
                secret_field(tr(u8"form.auth.response_label"));
            else
                form.fields.push_back(make_field(context, 0, tr(u8"form.auth.response_label"), u8"", false, true));
            answer_buttons(tr(u8"form.confirm"));
            break;
        }
        return form;
    }
} // namespace cyella
