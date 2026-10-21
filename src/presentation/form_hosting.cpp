#include "presentation/form_hosting.h"

#include "domain/ref_name.h"
#include "domain/text_catalog.h"
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
            return fail(tr(u8"form.pull_request.target_required"));
        if (find_revision_problem(target).has_value())
            return fail(tr(u8"form.pull_request.target_invalid"));
        if (target == dialog.subject)
            return fail(tr(u8"form.pull_request.target_same"));
        if (field_text(inputs, 1).empty())
            return fail(tr(u8"form.pull_request.title_required"));
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
        form.title = dialog.title.empty() ? tr(u8"form.pull_request.title") : dialog.title;
        for (const std::u8string& line : dialog.lines)
            form.notes.push_back({ .text = line });
        form.fields.push_back(make_field(context, 0, tr(u8"form.pull_request.target_label"), u8"main"));
        form.fields.push_back(make_field(context, 1, tr(u8"form.pull_request.title_label"), tr(u8"form.pull_request.title_placeholder"), true));
        form.fields.push_back(make_field(context, 2, tr(u8"form.pull_request.body_label"), tr(u8"form.pull_request.body_placeholder")));
        form.checks.push_back({ .label = tr(u8"form.pull_request.draft"), .checked = dialog.flag_a });
        if (dialog.flag_c)
        {
            const std::u8string remote { dialog.choices.empty() ? tr(u8"form.pull_request.default_remote") : to_u8(dialog.choices.front()) };
            form.checks.push_back({ .label = tr(u8"form.pull_request.push_first", { { u8"branch", dialog.subject }, { u8"remote", remote } }), .checked = dialog.flag_b });
            if (dialog.flag_b == false)
                form.warnings.push_back({ .text = tr(u8"form.pull_request.push_warning"), .warning = true });
        }
        std::u8string reason {};
        const bool ready { create_pull_request_ready(dialog, *context.inputs, &reason) };
        if (ready == false && (field_text(*context.inputs, 0).empty() == false || field_text(*context.inputs, 1).empty() == false))
            form.error = reason;
        form.buttons.push_back(make_button(u8"cancel", tr(u8"form.cancel")));
        form.buttons.push_back(make_button(u8"submit", tr(u8"form.create"), form_tone::accent, ready));
        form.dismiss_key = u8"cancel";
        return form;
    }
} // namespace cyella
