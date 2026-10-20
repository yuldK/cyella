#include "domain/hosting_remote.h"
#include "presentation/settings/settings_controller.h"
#include "presentation/settings/settings_view.h"

#include <algorithm>
#include <utility>

// 환경 설정 "호스팅" 화면이다: 자체 호스팅 서버 표를 고치고 로그인 상태를 보인다. 토큰은 여기서 다루지 않는다 - 로그인 상태는 이번 실행의 메모리
// 값이고 [로그아웃]은 그 토큰만 잊는다.
namespace cyella {
    namespace {
        std::string to_text(const std::u8string_view text)
        {
            return std::string { text.begin(), text.end() };
        }

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        void set_text(luil::text::text_edit_state& state, std::u8string text)
        {
            state = {};
            state.caret = text.size();
            state.anchor = text.size();
            state.text = std::move(text);
        }

        hosting_host_setting make_setting(const std::u8string_view host, const hosting_provider provider, const std::u8string_view api_base)
        {
            hosting_host_setting setting {};
            setting.host = normalize_hosting_host(to_text(host));
            setting.provider = provider;
            std::string api { to_text(api_base) };
            while (api.empty() == false && (api.front() == ' ' || api.front() == '\t'))
                api.erase(api.begin());
            while (api.empty() == false && (api.back() == ' ' || api.back() == '\t' || api.back() == '/'))
                api.pop_back();
            setting.api_base = std::move(api);
            return setting;
        }
    } // namespace

    std::u8string check_hosting_draft(const std::u8string_view host, const hosting_provider provider, const std::u8string_view api_base, const std::vector<hosting_host_setting>& existing)
    {
        const hosting_host_setting setting { make_setting(host, provider, api_base) };
        if (const std::optional<std::string> problem { validate_hosting_host_setting(setting) }; problem.has_value())
            return to_u8(*problem);
        if (std::ranges::any_of(existing, [&setting](const hosting_host_setting& other) { return normalize_hosting_host(other.host) == setting.host; }))
            return u8"이미 표에 있는 호스트입니다.";
        return {};
    }

    void fill_hosting_page(hosting_settings_page_view& view, const settings_view_inputs& inputs)
    {
        const settings_state& state { *inputs.state };
        view.hosts = inputs.hosting_hosts;
        view.host = luil::make_text_input_view(state.hosting_host, inputs.composition, to_luil_target(text_target::settings_hosting_host));
        view.api_base = luil::make_text_input_view(state.hosting_api_base, inputs.composition, to_luil_target(text_target::settings_hosting_api_base));
        view.new_provider = state.hosting_new_provider;
        const std::u8string problem { check_hosting_draft(state.hosting_host.text, state.hosting_new_provider, state.hosting_api_base.text, inputs.settings->hosting.hosts) };
        view.can_add = problem.empty();
        // 아직 아무것도 입력하지 않았으면 이유를 오류로 띄우지 않는다.
        view.error = state.hosting_host.text.empty() ? state.hosting_error : problem;
    }

    bool settings_controller::handle_hosting(const luil::app_message& message)
    {
        const auto* const add { message.get<add_hosting_host_intent>() };
        const auto* const remove { message.get<remove_hosting_host_intent>() };
        const auto* const provider { message.get<set_new_hosting_provider_intent>() };
        if (add == nullptr && remove == nullptr && provider == nullptr)
            return false;
        if (state_.has_value() == false)
            return true;
        settings_state& state { *state_ };
        if (provider != nullptr)
        {
            state.hosting_new_provider = provider->provider;
            return true;
        }
        if (add != nullptr)
        {
            const std::u8string problem { check_hosting_draft(state.hosting_host.text, state.hosting_new_provider, state.hosting_api_base.text, dependencies_.settings().hosting.hosts) };
            if (problem.empty() == false)
            {
                state.hosting_error = problem;
                return true;
            }
            const hosting_host_setting setting { make_setting(state.hosting_host.text, state.hosting_new_provider, state.hosting_api_base.text) };
            update_settings([&setting](app_settings& settings) { settings.hosting.hosts.push_back(setting); });
            set_text(state.hosting_host, {});
            set_text(state.hosting_api_base, {});
            state.hosting_error.clear();
            state.message = u8"호스트 " + to_u8(setting.host) + u8"을(를) 표에 더했습니다. 이 호스트의 원격이 있는 저장소를 다시 열면 연동이 켜집니다.";
            return true;
        }
        const std::string host { normalize_hosting_host(remove->host) };
        update_settings([&host](app_settings& settings) { std::erase_if(settings.hosting.hosts, [&host](const hosting_host_setting& entry) { return normalize_hosting_host(entry.host) == host; }); });
        // 표에서 지운 호스트의 메모리 토큰도 잊는다 (도우미의 저장본은 그대로다).
        if (dependencies_.hosting_sign_out != nullptr)
            dependencies_.hosting_sign_out(host);
        state.message = u8"호스트 " + to_u8(host) + u8"을(를) 표에서 지웠습니다.";
        return true;
    }
} // namespace cyella
