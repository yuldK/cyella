#include "presentation/form_extension.h"

#include "domain/path_syntax.h"
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

        worktree_dialog_mode mode_of(const dialog_state& dialog) noexcept
        {
            return dialog.choice == 1 ? worktree_dialog_mode::existing_branch : dialog.choice == 2 ? worktree_dialog_mode::detached : worktree_dialog_mode::new_branch;
        }

        const ref* find_local_branch(const repository_snapshot& snapshot, const std::string& name)
        {
            for (const ref& value : snapshot.refs)
                if (value.kind == ref_kind::local && value.name == name)
                    return &value;
            return nullptr;
        }

        bool worktree_folder_used(const repository_snapshot& snapshot, const std::string& folder)
        {
            const std::u8string key { normalize_path_key(to_u8(folder)) };
            return std::ranges::any_of(snapshot.worktrees, [&key](const worktree_info& info) { return normalize_path_key(to_u8(info.path)) == key; });
        }
    } // namespace

    std::u8string default_worktree_folder(const std::u8string_view repository_path, const std::string_view label)
    {
        std::u8string_view trimmed { repository_path };
        while (trimmed.size() > 1 && (trimmed.back() == u8'/' || trimmed.back() == u8'\\'))
            trimmed.remove_suffix(1);
        const std::size_t separator { trimmed.find_last_of(u8"/\\") };
        if (separator == std::u8string_view::npos)
            return {};
        std::u8string suffix {};
        for (const char value : label)
            suffix.push_back(value == '/' || value == '\\' || value == ':' || value == ' ' ? u8'-' : static_cast<char8_t>(value));
        std::u8string folder { trimmed };
        folder += u8"-";
        folder += suffix.empty() ? std::u8string { u8"worktree" } : suffix;
        return folder;
    }

    bool add_worktree_ready(const dialog_state& dialog, const form_inputs& inputs, const repository_snapshot& snapshot, std::u8string* const reason)
    {
        const auto fail = [reason](std::u8string text) {
            if (reason != nullptr)
                *reason = std::move(text);
            return false;
        };
        const std::string folder { field_text(inputs, 0) };
        if (folder.empty())
            return fail(tr(u8"form.worktree.folder_required"));
        if (is_absolute_windows_path(to_u8(folder)) == false)
            return fail(tr(u8"form.worktree.folder_absolute"));
        if (worktree_folder_used(snapshot, folder))
            return fail(tr(u8"form.worktree.folder_used"));

        const std::string branch { field_text(inputs, 1) };
        switch (mode_of(dialog))
        {
        case worktree_dialog_mode::new_branch:
            if (branch.empty())
                return fail(tr(u8"form.worktree.new_branch_required"));
            if (const auto error { branch_name_error(branch, snapshot) }; error.has_value())
                return fail(*error);
            break;
        case worktree_dialog_mode::existing_branch: {
            if (branch.empty())
                return fail(tr(u8"form.worktree.branch_required"));
            const ref* const found { find_local_branch(snapshot, branch) };
            if (found == nullptr)
                return fail(tr(u8"form.worktree.branch_missing", { { u8"branch", branch } }));
            if (found->is_current)
                return fail(tr(u8"form.worktree.branch_current"));
            if (found->worktree_path.empty() == false)
                return fail(tr(u8"form.worktree.branch_in_other_worktree", { { u8"path", found->worktree_path } }));
            break;
        }
        case worktree_dialog_mode::detached:
            break;
        }
        return true;
    }

    worktree_add_request make_worktree_add_request(const dialog_state& dialog, const form_inputs& inputs)
    {
        worktree_add_request request {};
        request.path = to_u8(field_text(inputs, 0));
        switch (mode_of(dialog))
        {
        case worktree_dialog_mode::new_branch:
            request.new_branch = field_text(inputs, 1);
            request.start_point = dialog.subject;
            break;
        case worktree_dialog_mode::existing_branch:
            request.branch = field_text(inputs, 1);
            break;
        case worktree_dialog_mode::detached:
            request.detach = true;
            request.start_point = dialog.subject;
            break;
        }
        return request;
    }

    form_view build_add_worktree_form(const dialog_state& dialog, const form_build_context& context)
    {
        const repository_snapshot& snapshot { *context.snapshot };
        form_view form {};
        form.id = u8"worktree-dialog";
        form.title = tr(u8"form.worktree.title");
        form.notes.push_back({ .text = tr(u8"form.worktree.note", { { u8"start", dialog.subject_label } }) });
        form.choices_label = tr(u8"form.worktree.mode_label");
        form.choices.push_back({ .label = tr(u8"form.worktree.mode_new_branch") });
        form.choices.push_back({ .label = tr(u8"form.worktree.mode_existing_branch") });
        form.choices.push_back({ .label = tr(u8"form.worktree.mode_detached") });
        form.chosen = std::min<std::size_t>(dialog.choice, 2);
        form.fields.push_back(make_field(context, 0, tr(u8"form.worktree.folder_label"), u8"E:/work/repo-feature", true));
        const worktree_dialog_mode mode { mode_of(dialog) };
        if (mode == worktree_dialog_mode::new_branch)
            form.fields.push_back(make_field(context, 1, tr(u8"form.worktree.new_branch_label"), u8"feature/my-work"));
        else if (mode == worktree_dialog_mode::existing_branch)
            form.fields.push_back(make_field(context, 1, tr(u8"form.worktree.branch_label"), u8"main"));

        std::u8string reason {};
        const bool ready { add_worktree_ready(dialog, *context.inputs, snapshot, &reason) };
        // 아직 아무것도 입력하지 않았으면 이유를 오류로 띄우지 않는다.
        if (ready == false && (field_text(*context.inputs, 0).empty() == false || field_text(*context.inputs, 1).empty() == false))
            form.error = reason;

        const std::string folder { field_text(*context.inputs, 0) };
        const std::string branch { field_text(*context.inputs, 1) };
        const std::string shown_folder { folder.empty() ? tr_narrow(u8"form.worktree.preview_folder") : folder };
        std::string preview { "git worktree add " };
        switch (mode)
        {
        case worktree_dialog_mode::new_branch:
            preview += "-b " + (branch.empty() ? tr_narrow(u8"form.preview_name") : branch) + " -- " + shown_folder + (dialog.subject.empty() ? std::string {} : " " + dialog.subject.substr(0, 12));
            break;
        case worktree_dialog_mode::existing_branch:
            preview += "-- " + shown_folder + " " + (branch.empty() ? tr_narrow(u8"form.worktree.preview_branch") : branch);
            break;
        case worktree_dialog_mode::detached:
            preview += "--detach -- " + shown_folder + (dialog.subject.empty() ? std::string {} : " " + dialog.subject.substr(0, 12));
            break;
        }
        form.preview = to_u8(preview);
        form.buttons.push_back(make_button(u8"cancel", tr(u8"form.cancel")));
        form.buttons.push_back(make_button(u8"submit", tr(u8"form.worktree.submit"), form_tone::accent, ready, true));
        form.dismiss_key = u8"cancel";
        return form;
    }

    bool lfs_track_ready(const dialog_state&, const form_inputs& inputs, std::u8string* const reason)
    {
        const std::string pattern { field_text(inputs, 0) };
        if (pattern.empty())
        {
            if (reason != nullptr)
                *reason = tr(u8"form.lfs_track.pattern_required");
            return false;
        }
        if (pattern.front() == '-' || pattern.front() == '#')
        {
            if (reason != nullptr)
                *reason = tr(u8"form.lfs_track.pattern_invalid_start");
            return false;
        }
        return true;
    }

    lfs_track_request make_lfs_track_request(const form_inputs& inputs)
    {
        return lfs_track_request { field_text(inputs, 0) };
    }

    form_view build_lfs_track_form(const dialog_state& dialog, const form_build_context& context)
    {
        form_view form {};
        form.id = u8"lfs-track-dialog";
        form.title = tr(u8"form.lfs_track.title");
        form.notes.push_back({ .text = tr(u8"form.lfs_track.note") });
        form.fields.push_back(make_field(context, 0, tr(u8"form.lfs_track.pattern_label"), u8"*.psd", true));
        std::u8string reason {};
        const bool ready { lfs_track_ready(dialog, *context.inputs, &reason) };
        if (ready == false && field_text(*context.inputs, 0).empty() == false)
            form.error = reason;
        const std::string pattern { field_text(*context.inputs, 0) };
        form.preview = to_u8("git lfs track -- " + (pattern.empty() ? tr_narrow(u8"form.lfs_track.preview_pattern") : pattern));
        form.buttons.push_back(make_button(u8"cancel", tr(u8"form.cancel")));
        form.buttons.push_back(make_button(u8"submit", tr(u8"form.lfs_track.submit"), form_tone::accent, ready, true));
        form.dismiss_key = u8"cancel";
        return form;
    }
} // namespace cyella
