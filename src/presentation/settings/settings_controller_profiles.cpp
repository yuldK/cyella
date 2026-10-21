// 환경 설정 "사용자 프로필" 쪽의 메시지 처리다. settings_controller의 멤버 함수를 파일 하나로 나눠 둔다.
#include "domain/text_catalog.h"
#include "domain/user_profile.h"
#include "presentation/settings/profiles_intents.h"
#include "presentation/settings/profiles_page.h"
#include "presentation/settings/settings_controller.h"

#include <utility>

namespace cyella {
    namespace {
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
    } // namespace

    bool settings_controller::is_profile_message(const luil::app_message& message)
    {
        return message.get<add_profile_intent>() != nullptr || message.get<edit_profile_intent>() != nullptr || message.get<remove_profile_intent>() != nullptr
            || message.get<save_profile_intent>() != nullptr || message.get<cancel_profile_edit_intent>() != nullptr || message.get<choose_rule_profile_intent>() != nullptr
            || message.get<add_profile_rule_intent>() != nullptr || message.get<remove_profile_rule_intent>() != nullptr;
    }

    void settings_controller::handle_profile_message(const luil::app_message& message)
    {
        settings_state& state { *state_ };
        const app_settings& settings { dependencies_.settings() };
        if (message.get<add_profile_intent>() != nullptr)
        {
            state.profile_edit = profile_edit_state {};
            state.message.clear();
        }
        else if (const auto* const edit_intent { message.get<edit_profile_intent>() }; edit_intent != nullptr)
        {
            if (edit_intent->index >= settings.profiles.size())
                return;
            const user_profile& profile { settings.profiles[edit_intent->index] };
            profile_edit_state edit {};
            edit.index = edit_intent->index;
            set_text(edit.name, profile.name);
            set_text(edit.user_name, profile.user_name);
            set_text(edit.user_email, profile.user_email);
            set_text(edit.signing_key, profile.signing_key);
            state.profile_edit = std::move(edit);
            state.message.clear();
        }
        else if (const auto* const remove_intent { message.get<remove_profile_intent>() }; remove_intent != nullptr)
        {
            if (remove_intent->index >= settings.profiles.size())
                return;
            const std::u8string name { settings.profiles[remove_intent->index].name };
            const std::size_t index { remove_intent->index };
            update_settings([index](app_settings& value) { static_cast<void>(remove_user_profile(value, index)); });
            // 줄 번호가 밀렸으니 고치던 줄은 접는다.
            state.profile_edit.reset();
            state.message = tr(u8"profiles.removed", { { u8"name", name } });
        }
        else if (message.get<save_profile_intent>() != nullptr)
        {
            if (state.profile_edit.has_value() == false)
                return;
            const user_profile profile { make_profile_from_edit(*state.profile_edit) };
            const std::optional<std::size_t> index { state.profile_edit->index };
            if (check_user_profile(profile, settings.profiles, index).empty() == false)
                return;
            update_settings([&profile, index](app_settings& value) { upsert_user_profile(value, index, profile); });
            state.message = tr(u8"profiles.saved", { { u8"name", profile.name } });
            state.profile_edit.reset();
        }
        else if (message.get<cancel_profile_edit_intent>() != nullptr)
            state.profile_edit.reset();
        else if (const auto* const choose_intent { message.get<choose_rule_profile_intent>() }; choose_intent != nullptr)
            state.rule_draft.profile = choose_intent->name;
        else if (message.get<add_profile_rule_intent>() != nullptr)
        {
            profile_rule rule { .path_prefix = trimmed(state.rule_draft.prefix.text), .profile = state.rule_draft.profile };
            if (find_profile_index(settings.profiles, rule.profile).has_value() == false && settings.profiles.empty() == false)
                rule.profile = settings.profiles.front().name;
            if (check_profile_rule(rule, settings.profiles).empty() == false)
                return;
            const std::u8string text { tr(u8"profiles.rule_added", { { u8"prefix", rule.path_prefix }, { u8"profile", rule.profile } }) };
            update_settings([&rule](app_settings& value) { upsert_profile_rule(value, rule); });
            set_text(state.rule_draft.prefix, {});
            state.message = text;
        }
        else if (const auto* const remove_rule_intent { message.get<remove_profile_rule_intent>() }; remove_rule_intent != nullptr)
        {
            const std::size_t index { remove_rule_intent->index };
            update_settings([index](app_settings& value) { static_cast<void>(remove_profile_rule(value, index)); });
            state.message = tr(u8"profiles.rule_removed");
        }
    }
} // namespace cyella
