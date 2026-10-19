#include "presentation/form_builders.h"

#include "domain/ref_name.h"
#include "presentation/ui_state.h"

#include <algorithm>
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
                return u8"이름을 입력하세요.";
            case ref_name_problem::leading_dash:
                return u8"'-'로 시작할 수 없습니다.";
            case ref_name_problem::leading_slash:
            case ref_name_problem::trailing_slash:
            case ref_name_problem::consecutive_slashes:
                return u8"처음이나 끝에 '/'를 두거나 '/'를 이어 쓸 수 없습니다.";
            case ref_name_problem::component_starts_with_dot:
                return u8"'.'로 시작하는 구성 요소를 둘 수 없습니다.";
            case ref_name_problem::component_ends_with_lock:
                return u8"'.lock'으로 끝나는 구성 요소를 둘 수 없습니다.";
            case ref_name_problem::trailing_dot:
                return u8"'.'로 끝낼 수 없습니다.";
            case ref_name_problem::contains_double_dot:
                return u8"'..'을 쓸 수 없습니다.";
            case ref_name_problem::contains_at_brace:
                return u8"'@{'를 쓸 수 없습니다.";
            case ref_name_problem::is_single_at:
                return u8"'@' 하나만으로는 이름을 지을 수 없습니다.";
            case ref_name_problem::is_head:
                return u8"'HEAD'는 쓸 수 없는 이름입니다.";
            case ref_name_problem::forbidden_character:
                return u8"공백과 ~ ^ : ? * [ 문자를 쓸 수 없습니다.";
            case ref_name_problem::control_character:
                return u8"제어 문자를 쓸 수 없습니다.";
            case ref_name_problem::backslash:
                return u8"'\\'를 쓸 수 없습니다.";
            }
            return u8"쓸 수 없는 이름입니다.";
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
            field.view = luil::make_text_input_view(field_state(*context.inputs, index), composition, to_luil_target(static_cast<text_target>(static_cast<std::uint32_t>(text_target::form_field_0) + index)));
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
            return u8"'" + to_u8(name) + u8"' 브랜치가 이미 있습니다.";
        return std::nullopt;
    }

    std::optional<std::u8string> tag_name_error(const std::string_view name, const repository_snapshot& snapshot)
    {
        if (const std::optional<ref_name_problem> problem { find_ref_name_problem(name) }; problem.has_value())
            return problem_text(*problem);
        for (const ref& value : snapshot.refs)
            if (value.kind == ref_kind::tag && value.name == name)
                return u8"'" + to_u8(name) + u8"' 태그가 이미 있습니다.";
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
        form.title = u8"새 브랜치";
        form.notes.push_back({ .text = u8"만들 위치: " + dialog.subject_label });
        form.fields.push_back(make_field(context, 0, u8"브랜치 이름", u8"feature/my-work", false, true));
        form.checks.push_back({ .label = u8"만든 뒤 체크아웃", .checked = dialog.flag_a });
        if (name.empty() == false)
            if (const auto error { branch_name_error(name, snapshot) }; error.has_value())
                form.error = *error;
        const std::string at { dialog.subject.substr(0, 12) };
        const std::string shown_name { name.empty() ? std::string { "<이름>" } : name };
        form.preview = to_u8((dialog.flag_a ? "git switch -c " : "git branch ") + shown_name + (at.empty() ? std::string {} : " " + at));
        form.buttons.push_back(make_button(u8"cancel", u8"취소"));
        form.buttons.push_back(make_button(u8"submit", u8"만들기", form_tone::accent, create_branch_ready(dialog, *context.inputs, snapshot), true));
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
        form.title = u8"브랜치 이름 바꾸기";
        form.notes.push_back({ .text = u8"현재 이름: " + to_u8(dialog.subject) });
        form.fields.push_back(make_field(context, 0, u8"새 이름", u8"", false, true));
        if (name.empty() == false && name != dialog.subject)
            if (const auto error { branch_name_error(name, snapshot) }; error.has_value())
                form.error = *error;
        form.preview = u8"git branch -m " + to_u8(dialog.subject) + u8" " + (name.empty() ? std::u8string { u8"<새 이름>" } : to_u8(name));
        form.buttons.push_back(make_button(u8"cancel", u8"취소"));
        form.buttons.push_back(make_button(u8"submit", u8"이름 바꾸기", form_tone::accent, rename_branch_ready(dialog, *context.inputs, snapshot), true));
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
        form.title = u8"태그 만들기";
        form.notes.push_back({ .text = u8"대상 커밋: " + dialog.subject_label });
        form.fields.push_back(make_field(context, 0, u8"태그 이름", u8"v1.0.0", false, true));
        form.choices_label = u8"종류";
        form.choices.push_back({ .label = u8"lightweight (이름만)" });
        form.choices.push_back({ .label = u8"annotated (메시지·작성자·날짜를 남김)" });
        form.chosen = dialog.choice;
        if (annotated)
            form.fields.push_back(make_field(context, 1, u8"태그 메시지", u8"이 태그가 무엇인지 한 줄로 적습니다"));
        if (name.empty() == false)
            if (const auto error { tag_name_error(name, snapshot) }; error.has_value())
                form.error = *error;
        if (annotated && name.empty() == false && field_text(*context.inputs, 1).empty() && form.error.empty())
            form.error = u8"annotated 태그에는 메시지가 필요합니다.";
        const std::string at { dialog.subject.empty() ? std::string {} : dialog.subject.substr(0, 12) };
        const std::string shown_name { name.empty() ? std::string { "<이름>" } : name };
        std::string preview { "git tag " };
        preview += annotated ? "-a " + shown_name + " -m \"…\"" : shown_name;
        if (at.empty() == false)
            preview += " " + at;
        form.preview = to_u8(preview);
        form.buttons.push_back(make_button(u8"cancel", u8"취소"));
        form.buttons.push_back(make_button(u8"submit", u8"태그 만들기", form_tone::accent, create_tag_ready(dialog, *context.inputs, snapshot), true));
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
        form.notes.push_back({ .text = u8"보낼 브랜치: " + to_u8(dialog.subject) + (branch != nullptr && branch->ahead > 0 ? u8"  (↑" + number(branch->ahead) + u8")" : std::u8string {}) });
        form.choices_label = u8"원격";
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
            form.error = u8"원격이 없습니다. 먼저 원격을 추가하세요.";
        form.fields.push_back(make_field(context, 0, u8"원격 브랜치 이름", to_u8(dialog.subject), false, true));
        form.checks.push_back({ .label = u8"upstream으로 설정 (--set-upstream)", .checked = dialog.flag_a });
        form.checks.push_back({ .label = u8"annotated 태그도 함께 push (--follow-tags)", .checked = dialog.flag_b });
        form.checks.push_back({ .label = u8"강제 push (--force-with-lease)", .checked = dialog.flag_c });
        if (dialog.flag_c)
            form.warnings.push_back({ .text = u8"원격 이력을 덮어쓸 수 있습니다. 마지막으로 fetch한 뒤 원격이 바뀌지 않았을 때만 성공합니다.", .warning = true });
        if (branch != nullptr && branch->behind > 0 && dialog.flag_c == false)
            form.warnings.push_back({ .text = u8"원격에 내게 없는 커밋이 " + number(branch->behind) + u8"개 있습니다. push가 거절될 수 있습니다.", .warning = true });
        const push_execute_request request { make_push_request(dialog, *context.inputs) };
        std::string preview { "git push" };
        if (request.set_upstream)
            preview += " --set-upstream";
        if (request.follow_tags)
            preview += " --follow-tags";
        if (request.force_with_lease)
            preview += " --force-with-lease";
        preview += " " + (request.remote.empty() ? std::string { "<원격>" } : request.remote) + " " + request.branch + ":" + request.remote_branch;
        form.preview = to_u8(preview);
        form.buttons.push_back(make_button(u8"cancel", u8"취소"));
        form.buttons.push_back(make_button(u8"submit", dialog.flag_c ? u8"강제 Push" : u8"Push", dialog.flag_c ? form_tone::danger : form_tone::accent, push_ready(dialog, *context.inputs, snapshot), true));
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
            form.warnings.push_back({ .text = u8"원격에서 지운 태그는 다른 사람의 저장소에도 영향을 줍니다.", .warning = true });
        form.buttons.push_back(make_button(u8"cancel", u8"취소", form_tone::normal, true, dialog.danger));
        form.buttons.push_back(make_button(u8"submit", dialog.confirm_label.empty() ? std::u8string { u8"확인" } : dialog.confirm_label, dialog.danger ? form_tone::danger : form_tone::accent, true, dialog.danger == false));
        form.dismiss_key = u8"cancel";
        return form;
    }

    // ---------------------------------------------------------------- 체크아웃 정책

    form_view build_dirty_checkout_form(const operation_status& status, const dirty_checkout_state& state, const form_build_context& context)
    {
        const input_request& request { *status.needs_input };
        form_view form {};
        form.id = u8"dirty-dialog";
        form.title = u8"작업 트리에 변경이 있습니다";
        if (state.confirming_discard)
        {
            form.title = u8"변경을 버리고 이동할까요?";
            form.notes.push_back({ .text = u8"'" + to_u8(request.target) + u8"'(으)로 옮기면서 아래 파일의 변경을 버립니다. 되돌릴 수 없습니다." });
            for (std::size_t index { 0 }; index < std::min(request.files.size(), listed_file_limit); ++index)
                form.notes.push_back({ .text = to_u8(request.files[index]), .code = true });
            if (request.files.size() > listed_file_limit)
                form.notes.push_back({ .text = u8"외 " + number(request.files.size() - listed_file_limit) + u8"개" });
            if (request.untracked_files)
                form.warnings.push_back({ .text = u8"추적하지 않는 파일은 이 방법으로 지워지지 않아 이동이 다시 막힐 수 있습니다.", .warning = true });
            form.buttons.push_back(make_button(u8"back", u8"뒤로"));
            form.buttons.push_back(make_button(u8"submit", u8"변경을 버리고 이동", form_tone::danger));
            form.dismiss_key = u8"cancel";
            return form;
        }
        form.notes.push_back({ .text = u8"'" + to_u8(request.target) + u8"'(으)로 옮기려면 아래 파일의 변경이 덮어써집니다." });
        for (std::size_t index { 0 }; index < std::min(request.files.size(), listed_file_limit); ++index)
            form.notes.push_back({ .text = to_u8(request.files[index]), .code = true });
        if (request.files.size() > listed_file_limit)
            form.notes.push_back({ .text = u8"외 " + number(request.files.size() - listed_file_limit) + u8"개" });
        form.choices_label = u8"어떻게 할까요?";
        form.choices.push_back({ .label = u8"stash 후 이동하고 다시 꺼내기" });
        form.choices.push_back({ .label = u8"변경을 가지고 이동 (--merge)" });
        form.choices.push_back({ .label = u8"변경 버리고 이동 (--discard-changes)" });
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
            form.warnings.push_back({ .text = u8"변경이 영구히 사라집니다. 다음 단계에서 한 번 더 확인합니다.", .warning = true });
            break;
        }
        if (request.untracked_files)
            form.warnings.push_back({ .text = u8"추적하지 않는 파일도 덮어써집니다. \"변경 버리고 이동\"으로는 풀리지 않으니 stash를 고르세요.", .warning = true });
        form.buttons.push_back(make_button(u8"cancel", u8"취소"));
        form.buttons.push_back(make_button(u8"submit", form.chosen == 2 ? u8"변경 버리고 이동…" : u8"이동", form.chosen == 2 ? form_tone::danger : form_tone::accent, true, true));
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
            form.buttons.push_back(make_button(u8"cancel", u8"취소"));
            form.buttons.push_back(make_button(u8"answer", std::move(label), form_tone::accent, true, true));
        };
        const auto secret_field = [&](std::u8string label) {
            form.fields.push_back(make_field(context, 0, std::move(label), u8"", true, true));
            form.can_reveal = true;
            form.checks.push_back({ .label = u8"입력 보이기", .checked = context.inputs->reveal });
        };
        switch (prompt.kind)
        {
        case credential_prompt_kind::ssh_key_passphrase:
            form.title = u8"SSH 키 passphrase";
            if (prompt.key_path.empty() == false)
                form.notes.push_back({ .text = u8"키: " + prompt.key_path, .code = true });
            secret_field(u8"passphrase");
            form.notes.push_back({ .text = u8"cyella는 passphrase를 저장하지 않습니다. 틀리면 ssh가 최대 3번 다시 묻습니다." });
            answer_buttons(u8"확인");
            break;
        case credential_prompt_kind::ssh_new_key_passphrase:
            form.title = u8"새 SSH 키 passphrase";
            secret_field(u8"passphrase (비워 두면 없음)");
            answer_buttons(u8"확인");
            break;
        case credential_prompt_kind::ssh_new_key_passphrase_confirm:
            form.title = u8"passphrase 확인";
            secret_field(u8"같은 passphrase를 한 번 더");
            answer_buttons(u8"확인");
            break;
        case credential_prompt_kind::ssh_host_key_confirm:
            form.title = u8"처음 보는 호스트 키";
            form.notes.push_back({ .text = u8"호스트: " + prompt.host + (prompt.address.empty() ? std::u8string {} : u8" (" + prompt.address + u8")") });
            form.notes.push_back({ .text = u8"키 종류: " + (prompt.key_type.empty() ? std::u8string { u8"알 수 없음" } : prompt.key_type) });
            form.notes.push_back({ .text = u8"지문: " + prompt.fingerprint, .code = true });
            form.warnings.push_back({ .text = u8"서버가 공개한 공식 지문과 같은지 확인하세요. 같지 않으면 거절하세요.", .warning = true });
            form.preview = u8"연결: known_hosts에 기록하고 이어서 · 이번만 연결: 이 작업에서만 믿음";
            form.buttons.push_back(make_button(u8"no", u8"거절", form_tone::normal, true, true));
            form.buttons.push_back(make_button(u8"once", u8"이번만 연결"));
            form.buttons.push_back(make_button(u8"yes", u8"연결", form_tone::accent));
            break;
        case credential_prompt_kind::ssh_password:
            form.title = u8"SSH 비밀번호";
            form.notes.push_back({ .text = (prompt.user_name.empty() ? std::u8string {} : prompt.user_name + u8"@") + prompt.host });
            secret_field(u8"비밀번호");
            answer_buttons(u8"확인");
            break;
        case credential_prompt_kind::https_username:
            form.title = u8"HTTPS 사용자 이름";
            if (prompt.url.empty() == false)
                form.notes.push_back({ .text = prompt.url, .code = true });
            form.fields.push_back(make_field(context, 0, u8"사용자 이름", u8"", false, true));
            form.notes.push_back({ .text = u8"Git Credential Manager가 없어 cyella가 묻습니다. 자격 증명은 저장하지 않습니다." });
            answer_buttons(u8"다음");
            break;
        case credential_prompt_kind::https_password:
            form.title = u8"HTTPS 비밀번호 또는 토큰";
            if (prompt.user_name.empty() == false || prompt.url.empty() == false)
                form.notes.push_back({ .text = (prompt.user_name.empty() ? std::u8string {} : prompt.user_name + u8" · ") + prompt.url, .code = true });
            secret_field(u8"비밀번호 / 토큰");
            answer_buttons(u8"확인");
            break;
        case credential_prompt_kind::unknown:
            form.title = u8"인증 정보가 필요합니다";
            {
                // git이 문구 끝에 붙인 공백·줄바꿈은 보이지 않는다.
                std::u8string raw { prompt.raw_text };
                const auto blank = [](const char8_t value) { return value == u8' ' || value == u8'\n' || value == u8'\r' || value == u8'\t'; };
                while (raw.empty() == false && blank(raw.back()))
                    raw.pop_back();
                form.notes.push_back({ .text = std::move(raw), .code = true });
            }
            if (request.secret)
                secret_field(u8"응답");
            else
                form.fields.push_back(make_field(context, 0, u8"응답", u8"", false, true));
            answer_buttons(u8"확인");
            break;
        }
        return form;
    }
} // namespace cyella
