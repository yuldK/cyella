// 환경 설정 "호스팅" 화면: 호스트 표 고치기, 검사, 로그인 상태 표시, 로그아웃이다. 설정은 메모리 저장소로 세우고 workspace는 쓰지 않는다.
#include "application/app_settings_service.h"
#include "presentation/codicon_names.h"
#include "presentation/hosting_controller.h"
#include "presentation/settings/settings_controller.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/settings/settings_view.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace {
    using namespace cyella;

    class memory_store final : public app_settings_store
    {
    public:
        [[nodiscard]] app_settings_load_result load(std::u8string_view) noexcept override
        {
            app_settings_load_result result {};
            result.settings = initial;
            return result;
        }

        [[nodiscard]] app_settings_save_result save(std::u8string_view, const app_settings& settings, std::u8string_view) noexcept override
        {
            saved = settings;
            ++saves;
            app_settings_save_result result {};
            result.succeeded = true;
            return result;
        }

        app_settings initial {};
        app_settings saved {};
        int saves { 0 };
    };

    struct harness
    {
        harness()
            : service { store, u8"memory" }
            , hosting { make_hosting_dependencies() }
        {
            static_cast<void>(service.load());
            settings_controller::dependencies dependencies {};
            dependencies.settings = [this]() -> const app_settings& { return service.settings(); };
            dependencies.update = [this](const std::function<void(app_settings&)>& change) { static_cast<void>(service.update(change)); };
            dependencies.read_only = [] { return false; };
            dependencies.unsaved = [] { return false; };
            dependencies.hosting_rows = [this](const std::vector<hosting_host_setting>& configured) { return hosting.host_rows(configured); };
            dependencies.hosting_sign_out = [this](const std::string& host_key) {
                sign_outs.push_back(host_key);
                hosting.sign_out(host_key);
            };
            controller.emplace(std::move(dependencies));
            REQUIRE(controller->handle(luil::app_message { open_settings_intent { settings_page::hosting } }));
        }

        hosting_controller::dependencies make_hosting_dependencies()
        {
            hosting_controller::dependencies result {};
            result.target_of = [this](const std::string&) -> std::optional<hosting_target> {
                hosting_target target {};
                target.repository_id = "repo";
                target.repository = resolve_hosting_repository("git@github.com:octo/hello.git", "origin", {});
                return target;
            };
            result.selected_repository_id = [] { return std::string { "repo" }; };
            result.submit_job = [this](hosting_job job) { jobs.push_back(std::move(job)); };
            result.now = [] { return std::int64_t { 1700000000 }; };
            return result;
        }

        template<typename intent_type>
        void send(intent_type intent)
        {
            REQUIRE(controller->handle(luil::app_message { std::move(intent) }));
        }

        [[nodiscard]] settings_view view()
        {
            const std::optional<settings_view> built { controller->make_view(std::nullopt) };
            REQUIRE(built.has_value());
            return *built;
        }

        void type(const text_target target, std::u8string text)
        {
            luil::text::text_edit_state* const state { controller->draft(target) };
            REQUIRE(state != nullptr);
            *state = {};
            state->caret = text.size();
            state->anchor = text.size();
            state->text = std::move(text);
        }

        void sign_in_github()
        {
            hosting.request_sign_in("repo");
            REQUIRE(jobs.empty() == false);
            hosting_result_intent result {};
            result.serial = jobs.back().serial;
            result.repository_id = "repo";
            result.host_key = "github.com";
            result.payload = hosting_result<std::string> { .value = std::string { "octocat" } };
            result.signed_in = true;
            result.user = "octocat";
            REQUIRE(hosting.handle(luil::app_message { result }));
        }

        memory_store store {};
        app_settings_service service;
        hosting_controller hosting;
        std::optional<settings_controller> controller {};
        std::vector<hosting_job> jobs {};
        std::vector<std::string> sign_outs {};
    };
} // namespace

TEST_CASE("the hosting page lists the built-in hosts with their in-memory sign-in state", "[hosting][settings]")
{
    harness test {};
    settings_view view { test.view() };
    CHECK(view.page == settings_page::hosting);
    REQUIRE(view.hosting.hosts.size() == 2);
    CHECK(view.hosting.hosts[0].host == "github.com");
    CHECK(view.hosting.hosts[0].builtin);
    CHECK(view.hosting.hosts[0].signed_in == false);

    test.sign_in_github();
    view = test.view();
    CHECK(view.hosting.hosts[0].signed_in);
    CHECK(view.hosting.hosts[0].user == u8"octocat");
    CHECK(view.hosting.hosts[1].signed_in == false);
}

TEST_CASE("adding a host validates the draft, saves the table and clears the inputs", "[hosting][settings]")
{
    harness test {};
    CHECK(test.view().hosting.can_add == false);
    CHECK(test.view().hosting.error.empty());

    test.type(text_target::settings_hosting_host, u8"git corp");
    CHECK(test.view().hosting.can_add == false);
    CHECK(test.view().hosting.error.empty() == false);
    test.type(text_target::settings_hosting_host, u8"  Git.Corp.Example ");
    test.type(text_target::settings_hosting_api_base, u8"ftp://nope");
    CHECK(test.view().hosting.can_add == false);
    test.type(text_target::settings_hosting_api_base, u8"https://git.corp.example/api/v4/");
    CHECK(test.view().hosting.can_add);
    test.send(set_new_hosting_provider_intent { hosting_provider::gitlab });
    CHECK(test.view().hosting.new_provider == hosting_provider::gitlab);

    test.send(add_hosting_host_intent {});
    REQUIRE(test.service.settings().hosting.hosts.size() == 1);
    const hosting_host_setting& added { test.service.settings().hosting.hosts[0] };
    CHECK(added.host == "git.corp.example");
    CHECK(added.provider == hosting_provider::gitlab);
    CHECK(added.api_base == "https://git.corp.example/api/v4");
    // 저장됐고 입력은 비워졌다.
    CHECK(test.store.saved.hosting.hosts.size() == 1);
    CHECK(test.view().hosting.host.displayed_text().empty());
    const settings_view view { test.view() };
    REQUIRE(view.hosting.hosts.size() == 3);
    CHECK(view.hosting.hosts[2].host == "git.corp.example");
    CHECK(view.hosting.hosts[2].builtin == false);
    CHECK(view.message.find(u8"git.corp.example") != std::u8string::npos);

    // 같은 호스트는 두 번 더하지 못한다.
    test.type(text_target::settings_hosting_host, u8"GIT.corp.example");
    CHECK(test.view().hosting.can_add == false);
    test.send(add_hosting_host_intent {});
    CHECK(test.service.settings().hosting.hosts.size() == 1);
    CHECK(test.view().hosting.error == u8"이미 표에 있는 호스트입니다.");
}

TEST_CASE("removing a custom host forgets its in-memory token and keeps the others", "[hosting][settings]")
{
    harness test {};
    test.service.update([](app_settings& settings) {
        settings.hosting.hosts.push_back({ "git.corp.example", hosting_provider::github, "" });
        settings.hosting.hosts.push_back({ "127.0.0.1:8123", hosting_provider::gitlab, "http://127.0.0.1:8123" });
    });
    CHECK(test.view().hosting.hosts.size() == 4);
    test.send(remove_hosting_host_intent { "git.corp.example" });
    REQUIRE(test.service.settings().hosting.hosts.size() == 1);
    CHECK(test.service.settings().hosting.hosts[0].host == "127.0.0.1:8123");
    REQUIRE(test.sign_outs.size() == 1);
    CHECK(test.sign_outs[0] == "git.corp.example");
}

TEST_CASE("logging out from the hosting page forgets only the in-memory sign-in", "[hosting][settings]")
{
    harness test {};
    test.sign_in_github();
    REQUIRE(test.view().hosting.hosts[0].signed_in);
    test.hosting.handle(luil::app_message { hosting_sign_out_intent { "github.com" } });
    CHECK(test.view().hosting.hosts[0].signed_in == false);
    // 설정 파일에는 로그인과 관련된 것이 아무것도 쓰이지 않는다.
    CHECK(test.service.settings().hosting.hosts.empty());
    CHECK(test.store.saves == 0);
}

TEST_CASE("hosting messages that arrive after the settings screen closed are swallowed", "[hosting][settings]")
{
    harness test {};
    test.send(close_settings_intent {});
    CHECK(test.controller->is_open() == false);
    CHECK(test.controller->handle(luil::app_message { add_hosting_host_intent {} }));
    CHECK(test.service.settings().hosting.hosts.empty());
}

TEST_CASE("the web link table lists the built-in services and saves, overrides and removes custom rules", "[hosting][settings][web_link]")
{
    harness test {};
    settings_view view { test.view() };
    REQUIRE(view.hosting.web_links.size() == 5);
    CHECK(view.hosting.web_links[0].host == "github.com");
    CHECK(view.hosting.web_links[0].builtin);
    CHECK(view.hosting.web_links[0].glyph == find_codicon("github"));
    CHECK(view.hosting.can_add_link == false);
    CHECK(view.hosting.link_error.empty());

    // 모르는 아이콘 이름은 막고, 미리보기는 서비스 기본 아이콘에서 시작한다.
    test.type(text_target::settings_web_link_host, u8"ssh.code.example");
    test.type(text_target::settings_web_link_base, u8"https://code.example/");
    test.type(text_target::settings_web_link_icon, u8"not-an-icon");
    view = test.view();
    CHECK(view.hosting.can_add_link == false);
    CHECK(view.hosting.link_error.empty() == false);
    CHECK(view.hosting.link_icon_preview == 0);
    test.type(text_target::settings_web_link_icon, u8"");
    CHECK(test.view().hosting.link_icon_preview == find_codicon("repo"));
    test.type(text_target::settings_web_link_icon, u8"server");
    view = test.view();
    CHECK(view.hosting.can_add_link);
    CHECK(view.hosting.link_icon_preview == find_codicon("server"));

    test.send(set_new_web_link_service_intent { web_link_service::gitea });
    test.send(add_web_link_intent {});
    REQUIRE(test.service.settings().hosting.web_links.size() == 1);
    const web_link_setting& added { test.service.settings().hosting.web_links[0] };
    CHECK(added.host == "ssh.code.example");
    CHECK(added.service == web_link_service::gitea);
    CHECK(added.web_base == "https://code.example");
    CHECK(added.icon == "server");
    CHECK(test.store.saved.hosting.web_links.size() == 1);
    CHECK(test.view().hosting.link_host.displayed_text().empty());

    // 내장 서비스와 같은 호스트를 더하면 그 줄이 내장 줄을 대신한다.
    test.type(text_target::settings_web_link_host, u8"github.com");
    test.send(set_new_web_link_service_intent { web_link_service::other });
    test.send(add_web_link_intent {});
    view = test.view();
    CHECK(std::ranges::count_if(view.hosting.web_links, [](const web_link_row_view& row) { return row.host == "github.com"; }) == 1);
    CHECK(std::ranges::find_if(view.hosting.web_links, [](const web_link_row_view& row) { return row.host == "github.com"; })->builtin == false);

    test.send(remove_web_link_intent { "ssh.code.example" });
    REQUIRE(test.service.settings().hosting.web_links.size() == 1);
    CHECK(test.service.settings().hosting.web_links[0].host == "github.com");
}
