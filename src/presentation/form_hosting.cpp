#include "presentation/form_hosting.h"

#include "domain/ref_name.h"
#include "presentation/ui_state.h"

#include <algorithm>
#include <utility>

namespace cyella {
    namespace {
        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        const luil::text::text_edit_state& field_state(const form_inputs& inputs, const std::size_t index)
        {
            return inputs.fields[std::min<std::size_t>(index, inputs.fields.size() - 1)];
        }

        std::string field_text(const form_inputs& inputs, const std::size_t index)
        {
            return trimmed_text(field_state(inputs, index).text);
        }

        form_field make_field(const form_build_context& context, const std::size_t index, std::u8string label, std::u8string placeholder, const bool focus = false)
        {
            static const std::optional<luil::text_composition_event> none {};
            const std::optional<luil::text_composition_event>& composition { context.composition != nullptr ? *context.composition : none };
            form_field field {};
            field.label = std::move(label);
            field.view
                = luil::make_text_input_view(field_state(*context.inputs, index), composition, to_luil_target(static_cast<text_target>(static_cast<std::uint32_t>(text_target::form_field_0) + index)));
            field.placeholder = std::move(placeholder);
            field.focus = focus;
            return field;
        }

        form_button make_button(std::u8string key, std::u8string label, const form_tone tone = form_tone::normal, const bool enabled = true, const bool default_button = false)
        {
            return form_button { .key = std::move(key), .label = std::move(label), .tone = tone, .enabled = enabled, .default_button = default_button };
        }
    } // namespace

    bool create_pull_request_ready(const dialog_state& dialog, const form_inputs& inputs, std::u8string* const reason)
    {
        const auto fail = [reason](std::u8string text) {
            if (reason != nullptr)
                *reason = std::move(text);
            return false;
        };
        const std::string target { field_text(inputs, 0) };
        if (target.empty())
            return fail(u8"대상 브랜치를 입력하세요.");
        if (find_revision_problem(target).has_value())
            return fail(u8"대상 브랜치 이름이 올바르지 않습니다.");
        if (target == dialog.subject)
            return fail(u8"대상 브랜치가 현재 브랜치와 같습니다. 다른 브랜치를 고르세요.");
        if (field_text(inputs, 1).empty())
            return fail(u8"제목을 입력하세요.");
        return true;
    }

    create_pull_request_params make_create_pull_request_params(const dialog_state& dialog, const form_inputs& inputs)
    {
        create_pull_request_params params {};
        params.target_branch = field_text(inputs, 0);
        params.source_branch = dialog.subject;
        params.title = field_text(inputs, 1);
        params.body = field_text(inputs, 2);
        params.draft = dialog.flag_a;
        return params;
    }

    form_view build_create_pull_request_form(const dialog_state& dialog, const form_build_context& context)
    {
        form_view form {};
        form.id = u8"create-pull-request-dialog";
        form.title = dialog.title.empty() ? std::u8string { u8"PR 만들기" } : dialog.title;
        for (const std::u8string& line : dialog.lines)
            form.notes.push_back({ .text = line });
        form.fields.push_back(make_field(context, 0, u8"대상 브랜치", u8"main"));
        form.fields.push_back(make_field(context, 1, u8"제목", u8"변경을 한 줄로 설명하세요", true));
        form.fields.push_back(make_field(context, 2, u8"본문 (선택)", u8"무엇을 왜 바꿨는지"));
        form.checks.push_back({ .label = u8"초안으로 만들기", .checked = dialog.flag_a });
        if (dialog.flag_c)
        {
            const std::u8string remote { dialog.choices.empty() ? std::u8string { u8"원격" } : to_u8(dialog.choices.front()) };
            form.checks.push_back({ .label = u8"먼저 '" + to_u8(dialog.subject) + u8"'을(를) " + remote + u8"에 push하고 만들기", .checked = dialog.flag_b });
            if (dialog.flag_b == false)
                form.warnings.push_back({ .text = u8"이 브랜치가 원격에 없거나 원격보다 앞서 있습니다. push하지 않으면 서버가 커밋을 찾지 못해 만들지 못할 수 있습니다.", .warning = true });
        }
        std::u8string reason {};
        const bool ready { create_pull_request_ready(dialog, *context.inputs, &reason) };
        if (ready == false && (field_text(*context.inputs, 0).empty() == false || field_text(*context.inputs, 1).empty() == false))
            form.error = reason;
        form.buttons.push_back(make_button(u8"cancel", u8"취소"));
        form.buttons.push_back(make_button(u8"submit", u8"만들기", form_tone::accent, ready));
        form.dismiss_key = u8"cancel";
        return form;
    }
} // namespace cyella
