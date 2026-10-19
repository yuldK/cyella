// 환경 설정 화면의 snapshot builder와 controller test다. 설정 저장은 메모리 저장소로, ssh 도구 operation은
// 가짜 runner와 수동 dispatcher로 세운다 — git·ssh·디스크가 필요 없다.
#include "application/app_messages.h"
#include "application/app_settings_service.h"
#include "application/git_workspace.h"
#include "infrastructure/git_operation_runner.h"
#include "presentation/build_view_snapshot.h"
#include "presentation/settings/settings_controller.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/settings/settings_view.h"

#include "fake_git_runner.h"
#include "temporary_directory_support.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace {
    using namespace cyella;

    // 경로 탐색의 가짜다. 등록한 파일만 있는 것으로 본다.
    class fake_probe final : public file_probe
    {
    public:
        [[nodiscard]] path_kind probe(const std::u8string_view absolute_path) const noexcept override
        {
            for (const std::u8string& file : files)
                if (file == absolute_path)
                    return path_kind::file;
            return path_kind::missing;
        }

        std::vector<std::u8string> files {};
    };

    class memory_store final : public app_settings_store
    {
    public:
        [[nodiscard]] app_settings_load_result load(std::u8string_view) noexcept override
        {
            app_settings_load_result result {};
            result.settings = initial;
            result.read_failed = fail_read;
            return result;
        }

        [[nodiscard]] app_settings_save_result save(std::u8string_view, const app_settings& settings, std::u8string_view) noexcept override
        {
            ++saves;
            saved = settings;
            app_settings_save_result result {};
            result.succeeded = true;
            return result;
        }

        app_settings initial {};
        app_settings saved {};
        bool fail_read { false };
        int saves { 0 };
    };

    external_diff_tool make_tool(std::u8string extension, std::u8string executable, std::u8string arguments)
    {
        external_diff_tool tool {};
        tool.extensions = { std::move(extension) };
        tool.executable = std::move(executable);
        tool.arguments = std::move(arguments);
        return tool;
    }

    // controller와 그 의존물을 한곳에 둔 harness다.
    struct controller_harness
    {
        explicit controller_harness(std::u8string ssh_directory = u8"C:\\Users\\me\\.ssh")
            : ssh_directory { std::move(ssh_directory) }
            , runner { process, u8"C:/Git/cmd/git.exe", files, make_runner_options() }
            , workspace { runner, dispatcher.dispatcher(), events.sink(), nullptr, git_workspace_options { .refresh_on_open = false } }
            , service { store, u8"memory" }
        {
            static_cast<void>(service.load());
            controller.emplace(make_dependencies());
        }

        static git_operation_runner_options make_runner_options()
        {
            git_operation_runner_options options {};
            options.ssh_executable = u8"C:/Git/usr/bin/ssh.exe";
            options.ssh_keygen_executable = u8"C:/Git/usr/bin/ssh-keygen.exe";
            options.askpass_helper = u8"C:/Git/cyella-askpass.exe";
            options.askpass = askpass_endpoint { u8"\\\\.\\pipe\\cyella-test", u8"token" };
            return options;
        }

        settings_controller::dependencies make_dependencies()
        {
            settings_controller::dependencies dependencies {};
            dependencies.settings = [this]() -> const app_settings& { return service.settings(); };
            dependencies.update = [this](const std::function<void(app_settings&)>& change) { static_cast<void>(service.update(change)); };
            dependencies.read_only = [this] { return service.is_read_only(); };
            dependencies.unsaved = [this] { return service.has_unsaved_changes(); };
            dependencies.probe = &probe;
            dependencies.git_tool.availability = git_tool_availability::available;
            dependencies.git_tool.executable = u8"C:\\Git\\cmd\\git.exe";
            dependencies.git_tool.reported_version = u8"git version 2.52.0";
            dependencies.workspace = &workspace;
            dependencies.request_ssh_info = [this] { ++ssh_info_requests; };
            dependencies.copy_text = [this](std::string text) { copied.push_back(std::move(text)); };
            dependencies.ssh_directory = ssh_directory;
            dependencies.default_key_comment = u8"me@PC";
            dependencies.ssh_tools_found = true;
            dependencies.ssh_tools_origin = u8"Git for Windows";
            return dependencies;
        }

        template<typename intent_type>
        bool send(intent_type intent)
        {
            const bool handled { controller->handle(luil::app_message { std::move(intent) }) };
            controller->after_events();
            return handled;
        }

        [[nodiscard]] settings_view view()
        {
            const std::optional<settings_view> built { controller->make_view(std::nullopt) };
            REQUIRE(built.has_value());
            return *built;
        }

        void run_all_jobs()
        {
            while (dispatcher.run())
            {
            }
            for (const operation_event& event : events.take())
                workspace.apply(event);
        }

        // 입력 칸에 글을 넣는다 (사용자가 친 것과 같다).
        void type(const text_target target, std::u8string text)
        {
            luil::text::text_edit_state* const state { controller->draft(target) };
            REQUIRE(state != nullptr);
            *state = {};
            state->caret = text.size();
            state->anchor = text.size();
            state->text = std::move(text);
        }

        std::u8string ssh_directory {};
        test::fake_git_runner process {};
        test::fake_workspace_files files {};
        test::manual_dispatcher dispatcher {};
        test::event_queue events {};
        fake_probe probe {};
        memory_store store {};
        git_operation_runner runner;
        git_workspace workspace;
        app_settings_service service;
        std::optional<settings_controller> controller {};
        int ssh_info_requests { 0 };
        std::vector<std::string> copied {};
    };

    ssh_info_snapshot sample_info()
    {
        ssh_info_snapshot info {};
        info.service = ssh_agent_service_status::running;
        info.agent_list_available = true;
        info.agent.state = ssh_agent_list_state::identities;
        info.agent.identities.push_back({ 256, u8"SHA256:abc", u8"me@PC", u8"ED25519" });
        info.keys.push_back({ u8"id_ed25519", u8"ssh-ed25519", u8"me@PC", u8"ssh-ed25519 AAAA me@PC", u8"C:\\Users\\me\\.ssh\\id_ed25519" });
        info.ssh_directory = u8"C:\\Users\\me\\.ssh";
        return info;
    }
} // namespace

// ---------------------------------------------------------------------------------------------- 보조 함수

TEST_CASE("extension lists are normalised, de-duplicated and may use commas or spaces", "[settings][tools]")
{
    CHECK(parse_extension_list(u8".PNG, psd;*.tar.gz  .png") == std::vector<std::u8string> { u8".png", u8".psd", u8".tar.gz" });
    CHECK(parse_extension_list(u8"").empty());
    CHECK(parse_extension_list(u8" , ; ").empty());
    CHECK(parse_extension_list(u8".").empty());
    CHECK(format_extension_list({ u8".png", u8".psd" }) == u8".png, .psd");
}

TEST_CASE("the argv preview expands the sample paths and keeps spaces inside one argument", "[settings][tools]")
{
    const tool_draft_check check { check_tool_draft(u8".png", u8"C:\\Tools\\img diff.exe", u8"--left \"{left}\" --right {right} {path} {base}", nullptr) };
    CHECK(check.ok);
    REQUIRE(check.preview_arguments.size() == 5);
    CHECK(check.preview_arguments[0] == u8"--left");
    CHECK(check.preview_arguments[1] == u8"C:\\Temp\\cyella-diff\\1\\left\\sample.png");
    CHECK(check.preview_arguments[2] == u8"--right");
    CHECK(check.preview_arguments[3] == u8"C:\\Temp\\cyella-diff\\1\\right\\sample.png");
    CHECK(check.preview_arguments[4] == u8"art/sample.png");
    // {base}는 병합용이라 비어 그 인자가 빠졌다.
    CHECK(check.preview.starts_with(u8"\"C:\\Tools\\img diff.exe\" --left "));

    // 첫 확장자가 샘플 파일 이름을 정한다.
    const tool_draft_check psd { check_tool_draft(u8".psd .png", u8"x.exe", u8"{path}", nullptr) };
    REQUIRE(psd.preview_arguments.size() == 1);
    CHECK(psd.preview_arguments[0] == u8"art/sample.psd");
}

TEST_CASE("tool drafts report what blocks saving and what is only worth a warning", "[settings][tools]")
{
    SECTION("no extension")
    {
        const tool_draft_check check { check_tool_draft(u8"", u8"C:\\a.exe", u8"{left}", nullptr) };
        CHECK_FALSE(check.ok);
        CHECK(check.error.find(u8"확장자") != std::u8string::npos);
    }

    SECTION("no executable")
    {
        const tool_draft_check check { check_tool_draft(u8".png", u8"  ", u8"{left}", nullptr) };
        CHECK_FALSE(check.ok);
        CHECK(check.error.find(u8"실행 파일") != std::u8string::npos);
    }

    SECTION("an unclosed quote in the template")
    {
        const tool_draft_check check { check_tool_draft(u8".png", u8"C:\\a.exe", u8"\"{left}", nullptr) };
        CHECK_FALSE(check.ok);
        CHECK(check.error.find(u8"큰따옴표") != std::u8string::npos);
        CHECK(check.preview.empty());
    }

    SECTION("a missing absolute executable saves with a warning")
    {
        fake_probe probe {};
        const tool_draft_check check { check_tool_draft(u8".png", u8"C:\\missing.exe", u8"{left}", &probe) };
        CHECK(check.ok);
        CHECK(check.warning.find(u8"찾지 못했") != std::u8string::npos);
        probe.files.push_back(u8"C:\\missing.exe");
        CHECK(check_tool_draft(u8".png", u8"C:\\missing.exe", u8"{left}", &probe).warning.empty());
    }

    SECTION("a template without {left} or {right} warns")
    {
        const tool_draft_check check { check_tool_draft(u8".png", u8"tool", u8"--open", nullptr) };
        CHECK(check.ok);
        CHECK(check.warning.find(u8"{left}") != std::u8string::npos);
    }
}

TEST_CASE("the made tool matches what the form shows", "[settings][tools]")
{
    const external_diff_tool tool { make_external_tool(u8" .PSD, bin ", u8"  C:\\x.exe ", u8"  {left} {right}  ") };
    CHECK(tool.extensions == std::vector<std::u8string> { u8".psd", u8".bin" });
    CHECK(tool.executable == u8"C:\\x.exe");
    CHECK(tool.arguments == u8"{left} {right}");
}

TEST_CASE("git paths must be absolute files or empty", "[settings][general]")
{
    fake_probe probe {};
    probe.files.push_back(u8"C:\\Git\\cmd\\git.exe");
    CHECK(check_git_path(u8"", &probe).empty());
    CHECK(check_git_path(u8"   ", &probe).empty());
    CHECK(check_git_path(u8"C:\\Git\\cmd\\git.exe", &probe).empty());
    CHECK_FALSE(check_git_path(u8"git", &probe).empty());
    CHECK_FALSE(check_git_path(u8"C:\\nowhere\\git.exe", &probe).empty());
}

TEST_CASE("ssh targets accept user, host and port and refuse option injection", "[settings][ssh]")
{
    CHECK(parse_ssh_target(u8"github.com") == ssh_target { u8"git", u8"github.com", 0 });
    CHECK(parse_ssh_target(u8" alice@git.example.com:2222 ") == ssh_target { u8"alice", u8"git.example.com", 2222 });
    CHECK(parse_ssh_target(u8"ssh://bob@host") == ssh_target { u8"bob", u8"host", 0 });
    CHECK_FALSE(parse_ssh_target(u8"").has_value());
    CHECK_FALSE(parse_ssh_target(u8"-oProxyCommand=x").has_value());
    CHECK_FALSE(parse_ssh_target(u8"host name").has_value());
    CHECK_FALSE(parse_ssh_target(u8"host:99999").has_value());
    CHECK_FALSE(parse_ssh_target(u8"host:0").has_value());
    CHECK_FALSE(parse_ssh_target(u8"host:abc").has_value());
    CHECK_FALSE(parse_ssh_target(u8"-bad@host").has_value());
    CHECK_FALSE(parse_ssh_target(u8"a;b@host").has_value());
}

TEST_CASE("key names are plain file names", "[settings][ssh]")
{
    CHECK(check_key_name(u8"id_ed25519").empty());
    CHECK(check_key_name(u8"work-key.2026").empty());
    CHECK_FALSE(check_key_name(u8"").empty());
    CHECK_FALSE(check_key_name(u8"..\\evil").empty());
    CHECK_FALSE(check_key_name(u8"a/b").empty());
    CHECK_FALSE(check_key_name(u8"-oops").empty());
    CHECK_FALSE(check_key_name(u8".hidden").empty());
    CHECK_FALSE(check_key_name(u8"key.pub").empty());
    CHECK_FALSE(check_key_name(u8"한글").empty());
}

// ---------------------------------------------------------------------------------------------- snapshot

TEST_CASE("the general page reports the running git and a draft that differs from the saved path", "[settings][view]")
{
    controller_harness test {};
    test.send(open_settings_intent {});
    settings_view view { test.view() };
    CHECK(view.page == settings_page::general);
    CHECK(view.git.executable == u8"C:\\Git\\cmd\\git.exe");
    CHECK(view.git.origin == u8"자동 탐색");
    CHECK(view.git.problem.empty());
    CHECK_FALSE(view.git.dirty);
    CHECK_FALSE(view.git.can_apply);
    CHECK_FALSE(view.git.configured);

    test.type(text_target::settings_git_path, u8"relative\\git.exe");
    view = test.view();
    CHECK(view.git.dirty);
    CHECK_FALSE(view.git.validation.empty());
    CHECK_FALSE(view.git.can_apply);

    test.probe.files.push_back(u8"C:\\Other\\git.exe");
    test.type(text_target::settings_git_path, u8"C:\\Other\\git.exe");
    view = test.view();
    CHECK(view.git.validation.empty());
    CHECK(view.git.can_apply);
}

TEST_CASE("an unusable git is explained on the general page", "[settings][view]")
{
    settings_state state {};
    app_settings settings {};
    git_tool_info tool {};
    tool.availability = git_tool_availability::not_found;
    settings_view_inputs inputs {};
    inputs.state = &state;
    inputs.settings = &settings;
    inputs.git_tool = &tool;
    const settings_view view { build_settings_view(inputs) };
    CHECK_FALSE(view.git.problem.empty());
}

TEST_CASE("the ssh page shows the agent service, agent keys and key pairs", "[settings][view][ssh]")
{
    controller_harness test {};
    test.send(open_settings_intent { settings_page::ssh });
    CHECK(test.ssh_info_requests == 1);
    CHECK(test.view().ssh.loading);
    CHECK_FALSE(test.view().ssh.loaded);

    SECTION("running agent with identities")
    {
        test.send(ssh_info_loaded_intent { sample_info() });
        const settings_view view { test.view() };
        CHECK_FALSE(view.ssh.loading);
        CHECK(view.ssh.service_status == u8"실행 중");
        CHECK(view.ssh.service_tone == tone::good);
        REQUIRE(view.ssh.identities.size() == 1);
        CHECK(view.ssh.identities[0].find(u8"SHA256:abc") != std::u8string::npos);
        REQUIRE(view.ssh.keys.size() == 1);
        CHECK(view.ssh.keys[0].name == u8"id_ed25519");
        CHECK(view.ssh.keys[0].detail == u8"ssh-ed25519 · me@PC");
        CHECK(view.ssh.keys[0].can_copy);
    }

    SECTION("each service state has its own help")
    {
        ssh_info_snapshot info { sample_info() };
        info.agent.identities.clear();
        const std::vector<std::pair<ssh_agent_service_status, std::u8string>> states {
            { ssh_agent_service_status::stopped, u8"중지됨" },
            { ssh_agent_service_status::disabled, u8"사용 안 함" },
            { ssh_agent_service_status::not_installed, u8"설치되지 않음" },
        };
        for (const auto& [status, text] : states)
        {
            info.service = status;
            test.send(ssh_info_loaded_intent { info });
            const settings_view view { test.view() };
            CHECK(view.ssh.service_status == text);
            CHECK_FALSE(view.ssh.service_help.empty());
            CHECK(view.ssh.service_tone != tone::good);
        }
    }

    SECTION("agent list states")
    {
        ssh_info_snapshot info { sample_info() };
        info.agent.identities.clear();
        info.agent.state = ssh_agent_list_state::no_identities;
        test.send(ssh_info_loaded_intent { info });
        CHECK(test.view().ssh.agent_message.find(u8"등록된 키가 없습니다") != std::u8string::npos);
        info.agent.state = ssh_agent_list_state::agent_unavailable;
        test.send(ssh_info_loaded_intent { info });
        CHECK(test.view().ssh.agent_message.find(u8"연결하지 못했습니다") != std::u8string::npos);
        info.agent_list_available = false;
        test.send(ssh_info_loaded_intent { info });
        CHECK(test.view().ssh.agent_message.find(u8"ssh-add를 찾지 못해") != std::u8string::npos);
    }

    SECTION("a long identity list is cut with a count")
    {
        ssh_info_snapshot info { sample_info() };
        for (int index { 0 }; index < 10; ++index)
            info.agent.identities.push_back({ 256, u8"SHA256:x", u8"k", u8"ED25519" });
        test.send(ssh_info_loaded_intent { info });
        const settings_view view { test.view() };
        CHECK(view.ssh.identities.size() == 7);
        CHECK(view.ssh.identities.back().find(u8"외 5개") != std::u8string::npos);
    }
}

TEST_CASE("the key name suggestion avoids names that already exist until the user types one", "[settings][ssh]")
{
    controller_harness test {};
    test.send(open_settings_intent { settings_page::ssh });
    test.send(ssh_info_loaded_intent { sample_info() });
    CHECK(test.view().ssh.key_name.text == u8"id_ed25519_2");
    CHECK(test.view().ssh.keygen_error.empty());
    CHECK(test.view().ssh.can_create);

    // 사용자가 고친 이름은 목록이 다시 와도 그대로다.
    test.type(text_target::settings_key_name, u8"work");
    test.send(ssh_info_loaded_intent { sample_info() });
    CHECK(test.view().ssh.key_name.text == u8"work");

    test.type(text_target::settings_key_name, u8"id_ed25519");
    CHECK_FALSE(test.view().ssh.keygen_error.empty());
    CHECK_FALSE(test.view().ssh.can_create);
}

// ---------------------------------------------------------------------------------------------- 저장

TEST_CASE("choosing a theme is saved through the settings service", "[settings][controller]")
{
    controller_harness test {};
    test.send(open_settings_intent {});
    CHECK(test.view().theme == theme_preference::system);
    test.send(set_theme_intent { theme_preference::dark });
    CHECK(test.view().theme == theme_preference::dark);
    CHECK(test.store.saved.appearance.theme == theme_preference::dark);
}

TEST_CASE("applying a git path saves it and resetting goes back to auto detection", "[settings][controller]")
{
    controller_harness test {};
    test.probe.files.push_back(u8"C:\\Other\\git.exe");
    test.send(open_settings_intent {});

    // 잘못된 경로는 저장하지 않는다.
    test.type(text_target::settings_git_path, u8"nope");
    test.send(apply_git_path_intent {});
    CHECK(test.store.saved.git_executable.empty());
    CHECK(test.store.saves == 0);

    test.type(text_target::settings_git_path, u8"  C:\\Other\\git.exe ");
    test.send(apply_git_path_intent {});
    CHECK(test.store.saved.git_executable == u8"C:\\Other\\git.exe");
    CHECK(test.view().git.configured);
    CHECK_FALSE(test.view().git.dirty);
    CHECK(test.view().message.find(u8"다음 실행") != std::u8string::npos);

    test.send(reset_git_path_intent {});
    CHECK(test.store.saved.git_executable.empty());
    CHECK(test.view().git.path.text.empty());
}

TEST_CASE("a chosen file fills the git path draft or the tool executable draft", "[settings][controller]")
{
    controller_harness test {};
    test.send(open_settings_intent {});
    test.send(settings_file_chosen_intent { file_purpose::git_executable, u8"C:\\Picked\\git.exe" });
    CHECK(test.view().git.path.text == u8"C:\\Picked\\git.exe");
    // 저장은 [적용]을 눌러야 한다.
    CHECK(test.store.saves == 0);

    // 도구 편집 중이 아니면 도구 실행 파일 선택은 무시한다.
    test.send(settings_file_chosen_intent { file_purpose::tool_executable, u8"C:\\ignored.exe" });
    test.send(add_tool_intent {});
    test.send(settings_file_chosen_intent { file_purpose::tool_executable, u8"C:\\tool.exe" });
    REQUIRE(test.view().edit.has_value());
    CHECK(test.view().edit->executable.text == u8"C:\\tool.exe");
}

TEST_CASE("the external tool table supports add, edit and remove and saves every change", "[settings][controller][tools]")
{
    controller_harness test {};
    test.send(open_settings_intent { settings_page::diff_tools });
    CHECK(test.view().tools.empty());

    // 추가
    test.send(add_tool_intent {});
    REQUIRE(test.view().edit.has_value());
    CHECK(test.view().edit->is_new);
    CHECK(test.view().edit->arguments.text == u8"{left} {right}");
    CHECK_FALSE(test.view().edit->can_save);

    test.type(text_target::settings_tool_extensions, u8".psd, .png");
    test.type(text_target::settings_tool_executable, u8"C:\\Tools\\view.exe");
    test.send(insert_tool_token_intent { u8"{path}" });
    CHECK(test.view().edit->arguments.text == u8"{left} {right} {path}");
    REQUIRE(test.view().edit->can_save);
    test.send(save_tool_intent {});

    REQUIRE(test.store.saved.external_diff_tools.size() == 1);
    CHECK(test.store.saved.external_diff_tools[0].extensions == std::vector<std::u8string> { u8".psd", u8".png" });
    CHECK(test.store.saved.external_diff_tools[0].arguments == u8"{left} {right} {path}");
    CHECK_FALSE(test.view().edit.has_value());
    REQUIRE(test.view().tools.size() == 1);
    CHECK(test.view().tools[0].extensions == u8".psd, .png");

    // 편집
    test.send(edit_tool_intent { 0 });
    REQUIRE(test.view().edit.has_value());
    CHECK_FALSE(test.view().edit->is_new);
    CHECK(test.view().edit->executable.text == u8"C:\\Tools\\view.exe");
    test.type(text_target::settings_tool_executable, u8"C:\\Tools\\other.exe");
    test.send(save_tool_intent {});
    REQUIRE(test.store.saved.external_diff_tools.size() == 1);
    CHECK(test.store.saved.external_diff_tools[0].executable == u8"C:\\Tools\\other.exe");

    // 저장을 막는 입력은 표를 바꾸지 않는다.
    test.send(add_tool_intent {});
    test.type(text_target::settings_tool_extensions, u8"");
    test.send(save_tool_intent {});
    CHECK(test.view().edit.has_value());
    CHECK(test.store.saved.external_diff_tools.size() == 1);
    test.send(cancel_tool_edit_intent {});
    CHECK_FALSE(test.view().edit.has_value());

    // 삭제
    test.send(remove_tool_intent { 0 });
    CHECK(test.store.saved.external_diff_tools.empty());
    CHECK(test.view().tools.empty());
}

TEST_CASE("existing tools are listed in the order of the settings file", "[settings][controller][tools]")
{
    controller_harness test {};
    test.store.initial.external_diff_tools = { make_tool(u8".png", u8"C:\\a.exe", u8"{left} {right}"), make_tool(u8".bin", u8"C:\\b.exe", u8"{path}") };
    test.service.load();
    test.send(open_settings_intent { settings_page::diff_tools });
    const settings_view view { test.view() };
    REQUIRE(view.tools.size() == 2);
    CHECK(view.tools[0].index == 0);
    CHECK(view.tools[0].executable == u8"C:\\a.exe");
    CHECK(view.tools[1].extensions == u8".bin");
}

TEST_CASE("a read only settings file shows a banner and keeps changes in memory only", "[settings][controller]")
{
    controller_harness test {};
    test.store.fail_read = true;
    test.service.load();
    test.send(open_settings_intent {});
    CHECK(test.view().read_only);

    test.send(set_theme_intent { theme_preference::light });
    // 메모리는 바뀌지만 파일은 건드리지 않는다.
    CHECK(test.view().theme == theme_preference::light);
    CHECK(test.store.saves == 0);
}

TEST_CASE("closing the screen forgets the drafts and late messages are swallowed", "[settings][controller]")
{
    controller_harness test {};
    test.send(open_settings_intent {});
    CHECK(test.controller->is_open());
    test.send(close_settings_intent {});
    CHECK_FALSE(test.controller->is_open());
    CHECK_FALSE(test.controller->make_view(std::nullopt).has_value());
    CHECK(test.controller->draft(text_target::settings_git_path) == nullptr);
    // 닫힌 뒤에 온 설정 메시지는 처리한 것으로 삼키고 아무 일도 하지 않는다.
    CHECK(test.send(set_theme_intent { theme_preference::dark }));
    CHECK(test.store.saves == 0);
    CHECK(test.send(ssh_info_loaded_intent { sample_info() }));
    // 설정과 무관한 메시지는 건드리지 않는다.
    CHECK_FALSE(test.send(window_metrics_intent {}));
}

TEST_CASE("the scroll is clamped to the last arranged maximum", "[settings][controller]")
{
    controller_harness test {};
    test.send(open_settings_intent {});
    test.controller->apply_scroll_maximum(120.0f);
    test.send(settings_scroll_intent { .value = 500.0f, .absolute = false });
    CHECK(test.view().scroll == 120.0f);
    test.send(settings_scroll_intent { .value = -1000.0f, .absolute = false });
    CHECK(test.view().scroll == 0.0f);
    test.send(settings_scroll_intent { .value = 60.0f, .absolute = true });
    CHECK(test.view().scroll == 60.0f);
    // 쪽을 옮기면 맨 위로 돌아간다.
    test.send(select_settings_page_intent { settings_page::diff_tools });
    CHECK(test.view().scroll == 0.0f);
}

// ---------------------------------------------------------------------------------------------- SSH 작업

TEST_CASE("copying a public key goes to the clipboard and says so", "[settings][controller][ssh]")
{
    controller_harness test {};
    test.send(open_settings_intent { settings_page::ssh });
    test.send(ssh_info_loaded_intent { sample_info() });
    test.send(copy_public_key_intent { u8"id_ed25519" });
    REQUIRE(test.copied.size() == 1);
    CHECK(test.copied[0] == "ssh-ed25519 AAAA me@PC");
    CHECK(test.view().message.find(u8"id_ed25519.pub") != std::u8string::npos);

    // 목록에 없는 이름은 아무 것도 복사하지 않는다.
    test.send(copy_public_key_intent { u8"nope" });
    CHECK(test.copied.size() == 1);
}

TEST_CASE("creating a key submits an ssh_keygen setup operation and refreshes the list when it ends", "[settings][controller][ssh]")
{
    // 작업이 실제로 돌면 키 폴더를 만들므로 임시 폴더를 쓴다.
    const test::temporary_directory scratch { "settings-keygen" };
    const std::filesystem::path directory { scratch.path() / ".ssh" };
    controller_harness test { directory.u8string() };
    test.send(open_settings_intent { settings_page::ssh });
    test.send(ssh_info_loaded_intent { sample_info() });
    test.type(text_target::settings_key_name, u8"deploy");
    test.type(text_target::settings_key_comment, u8" deploy@ci ");
    REQUIRE(test.view().ssh.can_create);
    const std::u8string key_path { directory.u8string() + u8"\\deploy" };
    CHECK(test.view().ssh.keygen_command == u8"ssh-keygen -t ed25519 -C \"deploy@ci\" -f \"" + key_path + u8"\"");

    const int before { test.ssh_info_requests };
    test.send(create_ssh_key_intent {});
    REQUIRE(test.dispatcher.pending() == 1);
    CHECK(test.view().ssh.keygen.state == task_view::phase::running);
    CHECK_FALSE(test.view().ssh.can_create);

    // 하나만 낸다. 돌고 있는 동안 다시 눌러도 더하지 않는다.
    test.send(create_ssh_key_intent {});
    CHECK(test.dispatcher.pending() == 1);

    const operation_id id { test.controller->state()->keygen_operation };
    const operation_status* const status { test.workspace.find_operation(id) };
    REQUIRE(status != nullptr);
    CHECK(status->kind == operation_kind::ssh_keygen);
    CHECK(status->target_path == key_path);

    // 끝나면 키 목록을 다시 읽는다.
    test.run_all_jobs();
    test.send(window_metrics_intent {});
    test.controller->after_events();
    CHECK(test.ssh_info_requests == before + 1);
    // 같은 operation으로 두 번 읽지 않는다.
    test.controller->after_events();
    CHECK(test.ssh_info_requests == before + 1);
}

TEST_CASE("an ssh_keygen operation waiting for the passphrase shows as waiting for input", "[settings][view][ssh]")
{
    settings_state state {};
    app_settings settings {};
    settings_view_inputs inputs {};
    inputs.state = &state;
    inputs.settings = &settings;
    inputs.ssh_directory = u8"C:\\Users\\me\\.ssh";
    inputs.ssh_tools_found = true;
    inputs.keygen.id = 4;
    inputs.keygen.state = operation_state::needs_input;
    inputs.keygen.waiting_for_prompt = true;
    const settings_view view { build_settings_view(inputs) };
    CHECK(view.ssh.keygen.state == task_view::phase::waiting_for_input);
    CHECK(view.ssh.keygen.text.find(u8"대화 상자") != std::u8string::npos);
    CHECK_FALSE(view.ssh.can_create);
}

TEST_CASE("connection tests go through the setup lane and show the classification", "[settings][controller][ssh]")
{
    controller_harness test {};
    test.send(open_settings_intent { settings_page::ssh });
    test.type(text_target::settings_test_host, u8"alice@git.example.com:2222");
    REQUIRE(test.view().ssh.can_test);
    CHECK(test.view().ssh.test_command == u8"ssh -T -o StrictHostKeyChecking=ask -p 2222 alice@git.example.com");

    test.process.when({ "-T" }, test::scripted_response { .exit_code = 255, .standard_error_lines = { "ssh: Could not resolve hostname git.example.com: Name or service not known" } });
    test.send(test_ssh_connection_intent {});
    REQUIRE(test.dispatcher.pending() == 1);
    CHECK(test.view().ssh.test.state == task_view::phase::running);
    test.run_all_jobs();

    const settings_view view { test.view() };
    CHECK(view.ssh.test.state == task_view::phase::finished);
    CHECK(view.ssh.test.result_tone == tone::bad);
    CHECK(view.ssh.test.text == describe_ssh_connection({ ssh_connection_result::connection_failed, {} }));
    CHECK(view.ssh.can_test);

    // 실제로 낸 명령이다: ssh -T, 호스트 키는 묻고, 계정과 포트를 따른다.
    const std::vector<test::recorded_call> calls { test.process.calls() };
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].arguments == test::text_list { "-T", "-o", "StrictHostKeyChecking=ask", "-p", "2222", "alice@git.example.com" });
}

TEST_CASE("an invalid host is refused before anything is submitted", "[settings][controller][ssh]")
{
    controller_harness test {};
    test.send(open_settings_intent { settings_page::ssh });
    test.type(text_target::settings_test_host, u8"-oProxyCommand=calc");
    CHECK_FALSE(test.view().ssh.can_test);
    CHECK_FALSE(test.view().ssh.test_error.empty());
    test.send(test_ssh_connection_intent {});
    CHECK(test.dispatcher.pending() == 0);
}

TEST_CASE("the ssh operations have readable names for the status line", "[settings][view]")
{
    CHECK(operation_display_name(operation_kind::ssh_keygen) == u8"SSH 키 만들기");
    CHECK(operation_display_name(operation_kind::ssh_connection_test) == u8"SSH 연결 시험");
}
