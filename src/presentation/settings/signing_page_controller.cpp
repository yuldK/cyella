#include "presentation/settings/signing_page_controller.h"

#include <algorithm>
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

        std::string to_text(const std::u8string_view text)
        {
            return std::string { text.begin(), text.end() };
        }

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        // 설정 파일에는 슬래시 경로가 읽기 쉽고 이스케이프도 필요 없다.
        std::string with_slashes(std::string text)
        {
            std::ranges::replace(text, '\\', '/');
            return text;
        }

        std::u8string key_name(const signing_key_id key)
        {
            if (key == signing_key_id::gpg_ssh_allowed_signers)
                return u8"gpg.ssh.allowedSignersFile";
            return to_u8(signing_key_name(key));
        }

        const signing_config empty_config {};
    } // namespace

    signing_page_controller::signing_page_controller(dependencies dependencies)
        : dependencies_ { std::move(dependencies) }
    {}

    void signing_page_controller::on_page_opened()
    {
        if (state_.info.has_value() == false && state_.loading == false)
            request_info();
    }

    void signing_page_controller::on_closed()
    {
        state_ = {};
    }

    bool signing_page_controller::global_scope() const
    {
        if (dependencies_.repository_path == nullptr || dependencies_.repository_path().empty())
            return true;
        return state_.global_scope;
    }

    const signing_config& signing_page_controller::scope_config() const
    {
        if (state_.info.has_value() == false)
            return empty_config;
        return global_scope() ? state_.info->config.global : state_.info->config.local;
    }

    void signing_page_controller::request_info()
    {
        if (dependencies_.request_info == nullptr)
            return;
        state_.loading = true;
        dependencies_.request_info(dependencies_.repository_path != nullptr ? dependencies_.repository_path() : std::u8string {});
    }

    void signing_page_controller::fill_drafts()
    {
        const signing_config& scope { scope_config() };
        set_text(state_.gpg_program, to_u8(scope.at(signing_key_id::gpg_program).value));
        set_text(state_.ssh_program, to_u8(scope.at(signing_key_id::gpg_ssh_program).value));
        set_text(state_.allowed_signers, to_u8(scope.at(signing_key_id::gpg_ssh_allowed_signers).value));
    }

    void signing_page_controller::adopt_info(signing_info_snapshot info)
    {
        // 같은 설정을 다시 받은 것(gpg 키 목록이 뒤따라 온 경우)이면 사용자가 치던 입력을 덮지 않는다.
        const bool same_config { state_.info.has_value() && state_.info->config == info.config };
        state_.info = std::move(info);
        state_.loading = state_.info->gpg_pending;
        if (same_config == false)
            fill_drafts();
    }

    luil::text::text_edit_state* signing_page_controller::draft(const text_target target)
    {
        switch (target)
        {
        case text_target::settings_signing_gpg_program:
            return &state_.gpg_program;
        case text_target::settings_signing_ssh_program:
            return &state_.ssh_program;
        case text_target::settings_signing_allowed_signers:
            return &state_.allowed_signers;
        default:
            break;
        }
        return nullptr;
    }

    void signing_page_controller::request_change(std::vector<signing_change> changes, std::u8string summary)
    {
        if (state_.writing || changes.empty())
            return;
        state_.message.clear();
        state_.message_error = false;
        if (global_scope())
        {
            // 전역 설정은 사용자의 실제 git 설정이다. 확인을 받은 뒤에만 쓴다.
            state_.pending = pending_signing_write { true, std::move(changes), std::move(summary) };
            return;
        }
        write_now(false, std::move(changes));
    }

    void signing_page_controller::write_now(const bool global, std::vector<signing_change> changes)
    {
        if (dependencies_.write == nullptr)
            return;
        state_.writing = true;
        state_.message = u8"저장하는 중…";
        state_.message_error = false;
        dependencies_.write(global, dependencies_.repository_path != nullptr ? dependencies_.repository_path() : std::u8string {}, std::move(changes));
    }

    void signing_page_controller::apply_text(const signing_key_id key, const bool clear)
    {
        luil::text::text_edit_state* const input { key == signing_key_id::gpg_program ? &state_.gpg_program : key == signing_key_id::gpg_ssh_program ? &state_.ssh_program : &state_.allowed_signers };
        const std::u8string value { clear ? std::u8string {} : trimmed(input->text) };
        if (value.empty() == false && check_signing_path(value, key == signing_key_id::gpg_ssh_allowed_signers).empty() == false)
            return;
        signing_change change {};
        change.key = key;
        if (value.empty() == false)
            change.value = with_slashes(to_text(value));
        const std::u8string name { key_name(key) };
        request_change({ std::move(change) }, value.empty() ? name + u8" 설정을 지웁니다" : name + u8"을 " + value + u8"로 정합니다");
    }

    bool signing_page_controller::handle(const luil::app_message& message, const bool page_open)
    {
        if (const auto* const intent { message.get<signing_info_loaded_intent>() }; intent != nullptr)
        {
            // 닫은 뒤에 도착한 결과는 버린다.
            if (page_open)
                adopt_info(intent->info);
            return true;
        }
        if (const auto* const intent { message.get<signing_write_done_intent>() }; intent != nullptr)
        {
            if (page_open == false)
                return true;
            state_.writing = false;
            state_.message_error = intent->ok == false;
            state_.message = intent->ok ? u8"저장했습니다." : intent->error;
            // 성공해도 실패해도 git의 실제 값을 다시 읽어 화면과 맞춘다.
            request_info();
            if (intent->ok && dependencies_.changed != nullptr)
                dependencies_.changed(intent->global);
            return true;
        }

        const bool ours = message.get<refresh_signing_intent>() != nullptr || message.get<select_signing_scope_intent>() != nullptr || message.get<set_signing_toggle_intent>() != nullptr
            || message.get<set_signing_format_intent>() != nullptr || message.get<choose_signing_key_intent>() != nullptr || message.get<apply_signing_text_intent>() != nullptr
            || message.get<use_default_gpg_intent>() != nullptr || message.get<confirm_global_signing_intent>() != nullptr || message.get<cancel_global_signing_intent>() != nullptr;
        if (ours == false)
            return false;
        if (page_open == false)
            return true;

        if (message.get<refresh_signing_intent>() != nullptr)
        {
            if (state_.loading == false)
                request_info();
        }
        else if (const auto* const scope { message.get<select_signing_scope_intent>() }; scope != nullptr)
        {
            state_.global_scope = scope->global;
            state_.scope_chosen = true;
            state_.pending.reset();
            state_.message.clear();
            fill_drafts();
        }
        else if (const auto* const toggle { message.get<set_signing_toggle_intent>() }; toggle != nullptr)
        {
            signing_change change {};
            change.key = toggle->key;
            const std::u8string name { key_name(toggle->key) };
            std::u8string summary {};
            switch (toggle->choice)
            {
            case signing_toggle_choice::enabled:
                change.value = "true";
                summary = name + u8"을 켭니다";
                break;
            case signing_toggle_choice::disabled:
                change.value = "false";
                summary = name + u8"을 끕니다";
                break;
            case signing_toggle_choice::unset:
                summary = name + u8" 설정을 지웁니다";
                break;
            }
            request_change({ std::move(change) }, std::move(summary));
        }
        else if (const auto* const format { message.get<set_signing_format_intent>() }; format != nullptr)
        {
            signing_change change {};
            change.key = signing_key_id::gpg_format;
            if (format->format.has_value())
                change.value = std::string { signing_format_name(*format->format) };
            request_change({ std::move(change) }, format->format.has_value() ? u8"gpg.format을 " + to_u8(signing_format_name(*format->format)) + u8"로 정합니다" : u8"gpg.format 설정을 지웁니다");
        }
        else if (const auto* const key { message.get<choose_signing_key_intent>() }; key != nullptr)
        {
            signing_change change {};
            change.key = signing_key_id::user_signingkey;
            if (key->value.empty() == false)
                change.value = to_text(key->value);
            request_change({ std::move(change) }, key->value.empty() ? u8"user.signingkey 설정을 지웁니다" : u8"user.signingkey를 " + key->value + u8"로 정합니다");
        }
        else if (const auto* const text { message.get<apply_signing_text_intent>() }; text != nullptr)
            apply_text(text->key, text->clear);
        else if (message.get<use_default_gpg_intent>() != nullptr)
        {
            if (state_.info.has_value() && state_.info->default_gpg.empty() == false)
            {
                signing_change change {};
                change.key = signing_key_id::gpg_program;
                change.value = with_slashes(to_text(state_.info->default_gpg));
                request_change({ std::move(change) }, u8"gpg.program을 " + state_.info->default_gpg + u8"로 정합니다");
            }
        }
        else if (message.get<confirm_global_signing_intent>() != nullptr)
        {
            if (state_.pending.has_value() && state_.writing == false)
            {
                pending_signing_write pending { std::move(*state_.pending) };
                state_.pending.reset();
                write_now(pending.global, std::move(pending.changes));
            }
        }
        else if (message.get<cancel_global_signing_intent>() != nullptr)
            state_.pending.reset();
        return true;
    }

    signing_page_view signing_page_controller::make_view(const ssh_info_snapshot* const ssh, const file_probe* const probe, const std::optional<luil::text_composition_event>& composition) const
    {
        signing_page_view_inputs inputs {};
        inputs.state = &state_;
        inputs.ssh = ssh;
        inputs.probe = probe;
        inputs.repository_path = dependencies_.repository_path != nullptr ? dependencies_.repository_path() : std::u8string {};
        inputs.composition = composition;
        return build_signing_page_view(inputs);
    }
} // namespace cyella
