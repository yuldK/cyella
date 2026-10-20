#include "presentation/settings/profiles_page.h"

#include "domain/user_profile.h"
#include "presentation/ui_state.h"

#include <utility>

namespace cyella {
    namespace {
        std::u8string trimmed(std::u8string_view text)
        {
            const auto space = [](const char8_t value) { return value == u8' ' || value == u8'\t' || value == u8'\r' || value == u8'\n'; };
            while (text.empty() == false && space(text.front()))
                text.remove_prefix(1);
            while (text.empty() == false && space(text.back()))
                text.remove_suffix(1);
            return std::u8string { text };
        }
    } // namespace

    user_profile make_profile_from_edit(const profile_edit_state& edit)
    {
        return user_profile { .name = trimmed(edit.name.text), .user_name = trimmed(edit.user_name.text), .user_email = trimmed(edit.user_email.text), .signing_key = trimmed(edit.signing_key.text) };
    }

    profiles_page_view build_profiles_page_view(const app_settings& settings, const std::optional<profile_edit_state>& edit, const profile_rule_draft& rule,
        const std::optional<luil::text_composition_event>& composition, const profiles_repository_inputs& repository)
    {
        profiles_page_view view {};
        view.repository_open = repository.open;
        view.repository_name = repository.name;
        view.identity_text = repository.identity_text;
        for (std::size_t index { 0 }; index < settings.profiles.size(); ++index)
        {
            const user_profile& profile { settings.profiles[index] };
            profile_row_view row {};
            row.index = index;
            row.name = profile.name;
            row.identity = profile.user_name + u8" <" + profile.user_email + u8">";
            row.signing_key = profile.signing_key;
            row.applied = repository.applied_profile.empty() == false && repository.applied_profile == profile.name;
            row.editing = edit.has_value() && edit->index == index;
            view.profiles.push_back(std::move(row));
        }
        if (edit.has_value())
        {
            profile_edit_view editing {};
            editing.is_new = edit->index.has_value() == false;
            editing.name = luil::make_text_input_view(edit->name, composition, to_luil_target(text_target::settings_profile_name));
            editing.user_name = luil::make_text_input_view(edit->user_name, composition, to_luil_target(text_target::settings_profile_user_name));
            editing.user_email = luil::make_text_input_view(edit->user_email, composition, to_luil_target(text_target::settings_profile_user_email));
            editing.signing_key = luil::make_text_input_view(edit->signing_key, composition, to_luil_target(text_target::settings_profile_signing_key));
            editing.error = check_user_profile(make_profile_from_edit(*edit), settings.profiles, edit->index);
            editing.can_save = editing.error.empty();
            view.edit = std::move(editing);
        }
        for (std::size_t index { 0 }; index < settings.profile_rules.size(); ++index)
            view.rules.push_back(rule_row_view { .index = index, .prefix = settings.profile_rules[index].path_prefix, .profile = settings.profile_rules[index].profile });
        view.rule_prefix = luil::make_text_input_view(rule.prefix, composition, to_luil_target(text_target::settings_rule_prefix));
        // 고른 프로필이 지워졌거나 아직 고르지 않았으면 첫 프로필을 권한다.
        view.rule_profile = rule.profile;
        if (find_profile_index(settings.profiles, view.rule_profile).has_value() == false)
            view.rule_profile = settings.profiles.empty() ? std::u8string {} : settings.profiles.front().name;
        if (view.rule_prefix.displayed_text().empty() == false || view.rule_profile.empty() == false)
        {
            view.rule_error = check_profile_rule(profile_rule { .path_prefix = trimmed(rule.prefix.text), .profile = view.rule_profile }, settings.profiles);
            view.can_add_rule = view.rule_error.empty();
        }
        return view;
    }
} // namespace cyella
