#include "presentation/settings/signing_page_view.h"

#include "domain/path_syntax.h"
#include "domain/text_catalog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace cyella {
    namespace {
        constexpr std::array all_keys {
            signing_key_id::commit_gpgsign,
            signing_key_id::tag_gpgsign,
            signing_key_id::gpg_format,
            signing_key_id::user_signingkey,
            signing_key_id::gpg_program,
            signing_key_id::gpg_ssh_program,
            signing_key_id::gpg_ssh_allowed_signers,
        };

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

        bool is_boolean_key(const signing_key_id key) noexcept
        {
            return key == signing_key_id::commit_gpgsign || key == signing_key_id::tag_gpgsign;
        }

        // 경로 비교용이다: 구분자를 `/`로 맞추고 대소문자를 무시한다.
        std::string path_key(std::string text)
        {
            for (char& value : text)
            {
                if (value == '\\')
                    value = '/';
                value = static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
            }
            return text;
        }

        std::string slashes(std::string text)
        {
            std::ranges::replace(text, '\\', '/');
            return text;
        }

        const signing_config& scope_config(const signing_info_snapshot& info, const bool global) noexcept
        {
            return global ? info.config.global : info.config.local;
        }

        std::u8string toggle_choice_of(const signing_entry& entry)
        {
            if (entry.is_set() == false)
                return u8"unset";
            if (entry.valueless)
                return u8"enabled";
            bool value { false };
            if (parse_config_bool(entry.value, value) == false)
                return u8"unset";
            return value ? u8"enabled" : u8"disabled";
        }

        std::u8string format_choice_of(const signing_entry& entry)
        {
            signing_format format { signing_format::openpgp };
            if (entry.is_set() == false || parse_signing_format(entry.value, format) == false)
                return u8"unset";
            return to_u8(signing_format_name(format));
        }

        void fill_text(signing_text_view& view, const luil::text::text_edit_state& draft, const signing_config& scope, const signing_key_id key,
            const std::optional<luil::text_composition_event>& composition, const luil::text_input_target target, const bool allow_home_prefix, const file_probe* probe)
        {
            view.input = luil::make_text_input_view(draft, composition, target);
            view.stored = to_u8(scope.at(key).value);
            const std::u8string text { trimmed(view.input.displayed_text()) };
            view.error = check_signing_path(text, allow_home_prefix);
            const bool dirty { text != view.stored };
            view.can_apply = dirty && view.error.empty();
            view.can_clear = view.stored.empty() == false;
            if (view.error.empty() && text.empty() == false && is_absolute_windows_path(text) && probe != nullptr && probe->probe(text) != path_kind::file)
                view.warning = tr(u8"signing_page.path_missing_warning");
        }

        void fill_keys(signing_page_view& view, const signing_page_view_inputs& inputs, const signing_info_snapshot& info)
        {
            const signing_page_state& state { *inputs.state };
            const signing_config& scope { scope_config(info, state.global_scope) };
            // 고른 범위가 형식을 정하지 않았으면 이미 적용되는 형식을 따른다.
            view.key_format = scope.at(signing_key_id::gpg_format).is_set() ? scope.format() : info.config.effective.format();
            view.current_key = to_u8(scope.at(signing_key_id::user_signingkey).value);
            const std::string current { path_key(scope.at(signing_key_id::user_signingkey).value) };

            if (view.key_format == signing_format::ssh)
            {
                if (inputs.ssh == nullptr)
                {
                    view.keys_message = tr(u8"signing_page.loading_keys");
                    return;
                }
                for (const ssh_key_entry& key : inputs.ssh->keys)
                {
                    if (key.public_key_line.empty() || key.private_key_path.empty())
                        continue;
                    signing_key_row_view row {};
                    row.label = key.name;
                    row.detail = key.key_type;
                    if (key.comment.empty() == false)
                        row.detail += (row.detail.empty() ? u8"" : u8" · ") + key.comment;
                    const std::string value { slashes(std::string { key.private_key_path.begin(), key.private_key_path.end() }) + ".pub" };
                    row.value = to_u8(value);
                    row.selected = current.empty() == false && path_key(value) == current;
                    view.keys.push_back(std::move(row));
                }
                if (view.keys.empty())
                    view.keys_message = tr(u8"signing_page.no_ssh_key_pairs");
                return;
            }

            if (info.gpg_pending)
            {
                view.keys_message = tr(u8"signing_page.loading_gpg_keys");
                return;
            }
            if (info.gpg_keys_read == false)
            {
                view.keys_message = info.gpg_problem.empty() ? tr(u8"signing_page.gpg_keys_unreadable") : info.gpg_problem;
                return;
            }
            for (const gpg_secret_key& key : info.gpg_keys)
            {
                signing_key_row_view row {};
                row.label = key.user_ids.empty() ? to_u8(key.key_id) : key.user_ids.front();
                row.detail = to_u8(key.algorithm) + u8" · " + to_u8(key.key_id);
                row.value = to_u8(key.fingerprint.empty() ? key.key_id : key.fingerprint);
                row.usable = key.can_sign && key.expired == false && key.revoked == false && key.disabled == false;
                if (key.revoked)
                    row.detail += tr(u8"signing_page.key_revoked_suffix");
                else if (key.expired)
                    row.detail += tr(u8"signing_page.key_expired_suffix");
                else if (key.can_sign == false)
                    row.detail += tr(u8"signing_page.key_cannot_sign_suffix");
                const std::string stored { scope.at(signing_key_id::user_signingkey).value };
                row.selected = stored.empty() == false && (stored == key.fingerprint || stored == key.key_id || (stored.size() >= 8 && key.fingerprint.ends_with(stored)));
                view.keys.push_back(std::move(row));
            }
            if (view.keys.empty())
                view.keys_message = tr(u8"signing_page.no_gpg_keys");
        }
    } // namespace

    std::u8string describe_signing_value(const signing_key_id key, const signing_entry& entry)
    {
        if (entry.is_set() == false)
            return {};
        if (is_boolean_key(key))
        {
            if (entry.valueless)
                return tr(u8"signing_page.value_on");
            bool value { false };
            if (parse_config_bool(entry.value, value))
                return value ? tr(u8"signing_page.value_on") : tr(u8"signing_page.value_off");
        }
        return to_u8(entry.value);
    }

    std::u8string describe_effective_signing_value(const signing_key_id key, const signing_entry& entry)
    {
        if (entry.is_set())
            return describe_signing_value(key, entry);
        switch (key)
        {
        case signing_key_id::commit_gpgsign:
        case signing_key_id::tag_gpgsign:
            return tr(u8"signing_page.default_off");
        case signing_key_id::gpg_format:
            return tr(u8"signing_page.default_openpgp");
        case signing_key_id::user_signingkey:
            return tr(u8"signing_page.value_none");
        case signing_key_id::gpg_program:
            return tr(u8"signing_page.default_gpg_program");
        case signing_key_id::gpg_ssh_program:
            return tr(u8"signing_page.default_ssh_program");
        case signing_key_id::gpg_ssh_allowed_signers:
            break;
        }
        return tr(u8"signing_page.value_none");
    }

    std::u8string check_signing_path(const std::u8string_view text, const bool allow_home_prefix)
    {
        const std::u8string value { trimmed(text) };
        if (value.empty())
            return {};
        if (std::ranges::any_of(value, [](const char8_t character) { return character < 0x20; }))
            return tr(u8"signing_page.path_single_line");
        if (is_absolute_windows_path(value) || (allow_home_prefix && value.starts_with(u8"~/")))
            return {};
        return allow_home_prefix ? tr(u8"signing_page.path_absolute_or_home") : tr(u8"signing_page.path_absolute");
    }

    signing_page_view build_signing_page_view(const signing_page_view_inputs& inputs)
    {
        signing_page_view view {};
        const signing_page_state& state { *inputs.state };
        view.loading = state.loading;
        view.writing = state.writing;
        view.repository = inputs.repository_path.empty() == false;
        view.repository_path = inputs.repository_path;
        // 저장소가 없으면 이 저장소 범위는 쓸 수 없으니 전역만이다.
        view.global_scope = view.repository ? state.global_scope : true;
        view.message = state.message;
        view.message_error = state.message_error;
        view.confirming = state.pending.has_value();
        if (state.pending.has_value())
            view.confirm_text = tr(u8"signing_page.confirm_global", { { u8"summary", state.pending->summary } });

        if (state.info.has_value() == false)
            return view;
        const signing_info_snapshot& info { *state.info };
        view.loaded = true;
        view.default_gpg = info.default_gpg;

        for (const signing_key_id key : all_keys)
        {
            signing_setting_row_view row {};
            row.key = key;
            row.name = to_u8(signing_key_name(key));
            const signing_entry& effective { info.config.effective.at(key) };
            row.effective = describe_effective_signing_value(key, effective);
            row.origin = std::u8string { config_origin_label(effective.origin) };
            const std::u8string local { describe_signing_value(key, info.config.local.at(key)) };
            const std::u8string global { describe_signing_value(key, info.config.global.at(key)) };
            row.local = local.empty() ? u8"—" : local;
            row.global = global.empty() ? u8"—" : global;
            view.rows.push_back(std::move(row));
        }
        // git은 설정 이름을 소문자로 돌려주지만 화면에는 읽기 쉬운 이름을 쓴다.
        for (signing_setting_row_view& row : view.rows)
            if (row.key == signing_key_id::gpg_ssh_allowed_signers)
                row.name = u8"gpg.ssh.allowedSignersFile";

        const signing_config& scope { scope_config(info, view.global_scope) };
        view.commit_choice = toggle_choice_of(scope.at(signing_key_id::commit_gpgsign));
        view.tag_choice = toggle_choice_of(scope.at(signing_key_id::tag_gpgsign));
        view.format_choice = format_choice_of(scope.at(signing_key_id::gpg_format));
        fill_keys(view, inputs, info);

        const std::optional<luil::text_composition_event>& composition { inputs.composition };
        fill_text(view.gpg_program, state.gpg_program, scope, signing_key_id::gpg_program, composition, to_luil_target(text_target::settings_signing_gpg_program), false, inputs.probe);
        fill_text(view.ssh_program, state.ssh_program, scope, signing_key_id::gpg_ssh_program, composition, to_luil_target(text_target::settings_signing_ssh_program), false, inputs.probe);
        fill_text(view.allowed_signers, state.allowed_signers, scope, signing_key_id::gpg_ssh_allowed_signers, composition, to_luil_target(text_target::settings_signing_allowed_signers), true,
            inputs.probe);

        if (info.gpg_pending)
            view.gpg_status = tr(u8"signing_page.loading_gpg_keys");
        else if (info.gpg_keys_read)
            view.gpg_status = tr(u8"signing_page.gpg_status", { { u8"executable", info.gpg_executable }, { u8"count", info.gpg_keys.size() } });
        else
            view.gpg_status = info.gpg_problem.empty() ? tr(u8"signing_page.gpg_keys_unreadable") : info.gpg_problem;
        view.gpg_ok = info.gpg_keys_read;

        const signing_entry& allowed { info.config.effective.at(signing_key_id::gpg_ssh_allowed_signers) };
        if (allowed.is_set() == false || allowed.value.empty())
            view.allowed_signers_status = tr(u8"signing_page.allowed_signers_unset");
        else if (info.allowed_signers_exists)
        {
            view.allowed_signers_status = tr(u8"signing_page.allowed_signers_found", { { u8"path", info.allowed_signers_path } });
            view.allowed_signers_ok = true;
        }
        else
            view.allowed_signers_status = tr(u8"signing_page.allowed_signers_missing", { { u8"path", info.allowed_signers_path.empty() ? to_u8(allowed.value) : info.allowed_signers_path } });
        return view;
    }
} // namespace cyella
