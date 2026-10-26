#include "domain/hosting_remote.h"
#include "domain/remote_web_link.h"
#include "domain/text_catalog.h"
#include "presentation/codicon_names.h"
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
        std::string trimmed(const std::u8string_view text)
        {
            std::string value { to_text(text) };
            while (value.empty() == false && (value.front() == ' ' || value.front() == '\t'))
                value.erase(value.begin());
            while (value.empty() == false && (value.back() == ' ' || value.back() == '\t'))
                value.pop_back();
            return value;
        }

        web_link_setting make_link_setting(const std::u8string_view host, const web_link_service service, const std::u8string_view web_base, const std::u8string_view icon)
        {
            web_link_setting setting {};
            setting.host = normalize_hosting_host(to_text(host));
            setting.service = service;
            setting.web_base = trimmed(web_base);
            while (setting.web_base.empty() == false && setting.web_base.back() == '/')
                setting.web_base.pop_back();
            setting.icon = trimmed(icon);
            return setting;
        }
    } // namespace

    std::u8string check_web_link_draft(const std::u8string_view host, const std::u8string_view web_base, const std::u8string_view icon, const std::vector<web_link_setting>& existing)
    {
        const web_link_setting setting { make_link_setting(host, web_link_service::other, web_base, icon) };
        if (const std::optional<std::string> problem { validate_web_link_setting(setting) }; problem.has_value())
            return to_u8(*problem);
        if (setting.icon.empty() == false && find_codicon(setting.icon) == 0)
            return tr(u8"settings_view.web_link.icon_unknown", { { u8"icon", setting.icon } });
        if (std::ranges::any_of(existing, [&setting](const web_link_setting& other) { return normalize_hosting_host(other.host) == setting.host; }))
            return tr(u8"settings_view.web_link.host_exists");
        return {};
    }

    std::u8string check_hosting_draft(const std::u8string_view host, const hosting_provider provider, const std::u8string_view api_base, const std::vector<hosting_host_setting>& existing)
    {
        const hosting_host_setting setting { make_setting(host, provider, api_base) };
        if (const std::optional<std::string> problem { validate_hosting_host_setting(setting) }; problem.has_value())
            return to_u8(*problem);
        if (std::ranges::any_of(existing, [&setting](const hosting_host_setting& other) { return normalize_hosting_host(other.host) == setting.host; }))
            return tr(u8"settings_view.hosting.host_exists");
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

        const std::vector<web_link_setting>& configured { inputs.settings->hosting.web_links };
        const auto add_row = [&view](const web_link_setting& link, const bool builtin) {
            const std::string icon { link.icon.empty() ? std::string { default_web_link_icon(link.service) } : link.icon };
            view.web_links.push_back({ .host = link.host, .service = link.service, .web_base = link.web_base, .icon = icon, .glyph = find_codicon(icon), .builtin = builtin });
        };
        // 설정의 줄이 같은 호스트의 내장 규칙을 덮는다 (`resolve_remote_web_link`와 같은 차례).
        for (const web_link_setting& link : builtin_web_links())
            if (std::ranges::none_of(configured, [&link](const web_link_setting& other) { return normalize_hosting_host(other.host) == link.host; }))
                add_row(link, true);
        for (const web_link_setting& link : configured)
            add_row(link, false);
        view.link_host = luil::make_text_input_view(state.web_link_host, inputs.composition, to_luil_target(text_target::settings_web_link_host));
        view.link_base = luil::make_text_input_view(state.web_link_base, inputs.composition, to_luil_target(text_target::settings_web_link_base));
        view.link_icon = luil::make_text_input_view(state.web_link_icon, inputs.composition, to_luil_target(text_target::settings_web_link_icon));
        view.new_link_service = state.web_link_new_service;
        const std::string icon { trimmed(state.web_link_icon.text) };
        view.link_icon_preview = find_codicon(icon.empty() ? default_web_link_icon(state.web_link_new_service) : icon);
        const std::u8string link_problem { check_web_link_draft(state.web_link_host.text, state.web_link_base.text, state.web_link_icon.text, configured) };
        view.can_add_link = link_problem.empty();
        view.link_error = state.web_link_host.text.empty() ? state.web_link_error : link_problem;
    }

    bool settings_controller::handle_web_links(const luil::app_message& message)
    {
        const auto* const add { message.get<add_web_link_intent>() };
        const auto* const remove { message.get<remove_web_link_intent>() };
        const auto* const service { message.get<set_new_web_link_service_intent>() };
        if (add == nullptr && remove == nullptr && service == nullptr)
            return false;
        if (state_.has_value() == false)
            return true;
        settings_state& state { *state_ };
        if (service != nullptr)
        {
            state.web_link_new_service = service->service;
            return true;
        }
        if (add != nullptr)
        {
            const std::u8string problem { check_web_link_draft(state.web_link_host.text, state.web_link_base.text, state.web_link_icon.text, dependencies_.settings().hosting.web_links) };
            if (problem.empty() == false)
            {
                state.web_link_error = problem;
                return true;
            }
            const web_link_setting setting { make_link_setting(state.web_link_host.text, state.web_link_new_service, state.web_link_base.text, state.web_link_icon.text) };
            update_settings([&setting](app_settings& settings) { settings.hosting.web_links.push_back(setting); });
            set_text(state.web_link_host, {});
            set_text(state.web_link_base, {});
            set_text(state.web_link_icon, {});
            state.web_link_error.clear();
            state.message = tr(u8"settings_view.web_link.added", { { u8"host", setting.host } });
            return true;
        }
        const std::string host { normalize_hosting_host(remove->host) };
        update_settings([&host](app_settings& settings) { std::erase_if(settings.hosting.web_links, [&host](const web_link_setting& entry) { return normalize_hosting_host(entry.host) == host; }); });
        state.message = tr(u8"settings_view.web_link.removed", { { u8"host", host } });
        return true;
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
            state.message = tr(u8"settings_view.hosting.host_added", { { u8"host", setting.host } });
            return true;
        }
        const std::string host { normalize_hosting_host(remove->host) };
        update_settings([&host](app_settings& settings) { std::erase_if(settings.hosting.hosts, [&host](const hosting_host_setting& entry) { return normalize_hosting_host(entry.host) == host; }); });
        // 표에서 지운 호스트의 메모리 토큰도 잊는다 (도우미의 저장본은 그대로다).
        if (dependencies_.hosting_sign_out != nullptr)
            dependencies_.hosting_sign_out(host);
        state.message = tr(u8"settings_view.hosting.host_removed", { { u8"host", host } });
        return true;
    }
} // namespace cyella
