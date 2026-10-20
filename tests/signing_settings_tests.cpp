// 환경 설정 "서명" 쪽의 규칙이다: snapshot builder, controller의 읽기·쓰기 흐름과 전역 변경의 확인, 화면 tree의 이름.
// worker는 가짜(호출을 기록하는 함수)로 바꾸고 결과 메시지를 test가 직접 넣는다.
#include "application/app_messages.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/settings/signing_intents.h"
#include "presentation/settings/signing_page_controller.h"
#include "presentation/settings/signing_page_view.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/element_kinds.h"

#include "view_snapshot_support.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace cyella;

namespace {
    struct recorded_write
    {
        bool global { false };
        std::u8string repository_path {};
        std::vector<signing_change> changes {};
    };

    struct signing_harness
    {
        explicit signing_harness(std::u8string repository = u8"C:/work/repo")
            : repository_path { std::move(repository) }
            , controller { make_dependencies() }
        {}

        signing_page_controller::dependencies make_dependencies()
        {
            signing_page_controller::dependencies dependencies {};
            dependencies.request_info = [this](const std::u8string& path) { info_requests.push_back(path); };
            dependencies.write = [this](const bool global, const std::u8string& path, std::vector<signing_change> changes) { writes.push_back({ global, path, std::move(changes) }); };
            dependencies.repository_path = [this] { return repository_path; };
            dependencies.changed = [this](const bool global) { changed.push_back(global); };
            return dependencies;
        }

        template<typename intent_type>
        bool send(intent_type intent, const bool page_open = true)
        {
            return controller.handle(luil::app_message { std::move(intent) }, page_open);
        }

        [[nodiscard]] signing_page_view view(const ssh_info_snapshot* ssh = nullptr) const
        {
            return controller.make_view(ssh, nullptr, std::nullopt);
        }

        std::u8string repository_path {};
        std::vector<std::u8string> info_requests {};
        std::vector<recorded_write> writes {};
        std::vector<bool> changed {};
        signing_page_controller controller;
    };

    signing_entry entry(const std::string& value, const config_origin origin)
    {
        return signing_entry { .value = value, .origin = origin };
    }

    // 전역은 commit.gpgsign=true와 gpg.program, 이 저장소는 gpg.format=ssh와 키를 정한 설정이다.
    signing_info_snapshot sample_info()
    {
        signing_info_snapshot info {};
        info.repository = true;
        info.config_read = true;
        info.config.global.at(signing_key_id::commit_gpgsign) = entry("true", config_origin::global);
        info.config.global.at(signing_key_id::gpg_program) = entry("C:/Program Files/Git/usr/bin/gpg.exe", config_origin::global);
        info.config.local.at(signing_key_id::gpg_format) = entry("ssh", config_origin::local);
        info.config.local.at(signing_key_id::user_signingkey) = entry("C:/Users/me/.ssh/id_ed25519.pub", config_origin::local);
        info.config.effective = info.config.global;
        info.config.effective.at(signing_key_id::gpg_format) = info.config.local.at(signing_key_id::gpg_format);
        info.config.effective.at(signing_key_id::user_signingkey) = info.config.local.at(signing_key_id::user_signingkey);
        info.default_gpg = u8"C:\\Program Files\\Git\\usr\\bin\\gpg.exe";
        info.gpg_executable = info.default_gpg;
        info.gpg_keys_read = true;
        gpg_secret_key ed {};
        ed.key_id = "6B37DB64784DD266";
        ed.fingerprint = "6DEFB236E6F1450CB6CC36996B37DB64784DD266";
        ed.algorithm = "ed25519";
        ed.can_sign = true;
        ed.user_ids = { u8"Ann Author <ann@example.com>" };
        gpg_secret_key old {};
        old.key_id = "8128B0E8888E1BBB";
        old.fingerprint = "3FBE593F109EB16EED98A7CD8128B0E8888E1BBB";
        old.algorithm = "rsa3072";
        old.can_sign = true;
        old.expired = true;
        old.user_ids = { u8"Old Key <old@example.com>" };
        info.gpg_keys = { ed, old };
        info.allowed_signers_path = u8"C:/Users/me/.ssh/allowed_signers";
        info.allowed_signers_exists = true;
        info.config.effective.at(signing_key_id::gpg_ssh_allowed_signers) = entry("~/.ssh/allowed_signers", config_origin::global);
        return info;
    }

    ssh_info_snapshot sample_ssh()
    {
        ssh_info_snapshot info {};
        info.keys.push_back({ u8"id_ed25519", u8"ssh-ed25519", u8"me@PC", u8"ssh-ed25519 AAAA me@PC", u8"C:\\Users\\me\\.ssh\\id_ed25519" });
        info.keys.push_back({ u8"work", u8"ssh-rsa", u8"", u8"ssh-rsa BBBB", u8"C:\\Users\\me\\.ssh\\work" });
        return info;
    }
} // namespace

TEST_CASE("the signing page reads the settings when it opens and shows them per scope", "[signing][settings]")
{
    signing_harness test {};
    test.controller.on_page_opened();
    REQUIRE(test.info_requests.size() == 1);
    CHECK(test.info_requests[0] == u8"C:/work/repo");
    CHECK(test.view().loaded == false);
    CHECK(test.view().loading);

    // 이미 읽는 중이면 다시 읽지 않는다.
    test.controller.on_page_opened();
    CHECK(test.info_requests.size() == 1);

    REQUIRE(test.send(signing_info_loaded_intent { sample_info() }));
    const ssh_info_snapshot ssh { sample_ssh() };
    const signing_page_view view { test.view(&ssh) };
    CHECK(view.loaded);
    CHECK_FALSE(view.loading);
    CHECK(view.repository);
    CHECK_FALSE(view.global_scope);

    REQUIRE(view.rows.size() == 7);
    const signing_setting_row_view& commit { view.rows[0] };
    CHECK(commit.name == u8"commit.gpgsign");
    CHECK(commit.effective == u8"켜짐");
    CHECK(commit.origin == u8"전역");
    CHECK(commit.local == u8"—");
    CHECK(commit.global == u8"켜짐");
    CHECK(view.rows[1].effective == u8"꺼짐 (기본)");
    CHECK(view.rows[1].origin == u8"설정 없음");
    CHECK(view.rows[2].effective == u8"ssh");
    CHECK(view.rows[2].origin == u8"이 저장소");
    CHECK(view.rows[3].effective == u8"C:/Users/me/.ssh/id_ed25519.pub");
    CHECK(view.rows[4].effective == u8"C:/Program Files/Git/usr/bin/gpg.exe");
    CHECK(view.rows[5].effective == u8"ssh-keygen (기본)");
    CHECK(view.rows[6].name == u8"gpg.ssh.allowedSignersFile");

    // 이 저장소 범위의 선택 상태다.
    CHECK(view.commit_choice == u8"unset");
    CHECK(view.format_choice == u8"ssh");
    // ssh 형식이므로 ~/.ssh의 키가 후보이고, 정해 둔 키에 "사용 중" 표지가 선다.
    CHECK(view.key_format == signing_format::ssh);
    REQUIRE(view.keys.size() == 2);
    CHECK(view.keys[0].value == u8"C:/Users/me/.ssh/id_ed25519.pub");
    CHECK(view.keys[0].selected);
    CHECK_FALSE(view.keys[1].selected);
    CHECK(view.keys[1].value == u8"C:/Users/me/.ssh/work.pub");
    CHECK(view.allowed_signers_ok);
    CHECK(view.gpg_ok);

    // 전역 범위로 바꾸면 전역의 값이 선택 상태이고 형식은 openpgp라 gpg 키가 후보다.
    REQUIRE(test.send(select_signing_scope_intent { true }));
    const signing_page_view global { test.view(&ssh) };
    CHECK(global.global_scope);
    CHECK(global.commit_choice == u8"enabled");
    CHECK(global.format_choice == u8"unset");
    CHECK(global.key_format == signing_format::ssh);
}

TEST_CASE("the signing page lists gpg keys for openpgp and marks the unusable ones", "[signing][settings]")
{
    signing_harness test {};
    signing_info_snapshot info { sample_info() };
    info.config.local.at(signing_key_id::gpg_format) = {};
    info.config.effective.at(signing_key_id::gpg_format) = {};
    info.config.local.at(signing_key_id::user_signingkey) = entry("6DEFB236E6F1450CB6CC36996B37DB64784DD266", config_origin::local);
    REQUIRE(test.send(signing_info_loaded_intent { std::move(info) }));
    const signing_page_view view { test.view() };
    CHECK(view.key_format == signing_format::openpgp);
    REQUIRE(view.keys.size() == 2);
    CHECK(view.keys[0].label == u8"Ann Author <ann@example.com>");
    CHECK(view.keys[0].value == u8"6DEFB236E6F1450CB6CC36996B37DB64784DD266");
    CHECK(view.keys[0].selected);
    CHECK(view.keys[0].usable);
    // 만료된 키는 고를 수 없다.
    CHECK_FALSE(view.keys[1].usable);
    CHECK(view.keys[1].detail.find(u8"만료됨") != std::u8string::npos);
}

TEST_CASE("the signing page says why a key list is empty", "[signing][settings]")
{
    signing_harness test {};
    signing_info_snapshot info { sample_info() };
    info.gpg_keys.clear();
    info.gpg_keys_read = false;
    info.gpg_problem = u8"gpg를 찾지 못했습니다.";
    info.config.local.at(signing_key_id::gpg_format) = {};
    info.config.effective.at(signing_key_id::gpg_format) = {};
    REQUIRE(test.send(signing_info_loaded_intent { std::move(info) }));
    signing_page_view view { test.view() };
    CHECK(view.keys.empty());
    CHECK(view.keys_message == u8"gpg를 찾지 못했습니다.");
    CHECK_FALSE(view.gpg_ok);

    // ssh 형식에서 키 목록을 아직 읽지 못했다.
    REQUIRE(test.send(set_signing_format_intent { signing_format::ssh }));
    REQUIRE(test.send(signing_write_done_intent { .ok = true }));
    signing_info_snapshot ssh_info { sample_info() };
    REQUIRE(test.send(signing_info_loaded_intent { std::move(ssh_info) }));
    view = test.view(nullptr);
    CHECK(view.keys_message == u8"키 목록을 읽는 중…");
    const ssh_info_snapshot none {};
    view = test.view(&none);
    CHECK(view.keys_message.find(u8"~/.ssh") != std::u8string::npos);
}

TEST_CASE("changes in the repository scope are written at once with --local", "[signing][settings]")
{
    signing_harness test {};
    test.controller.on_page_opened();
    REQUIRE(test.send(signing_info_loaded_intent { sample_info() }));

    REQUIRE(test.send(set_signing_toggle_intent { signing_key_id::commit_gpgsign, signing_toggle_choice::enabled }));
    REQUIRE(test.writes.size() == 1);
    CHECK_FALSE(test.writes[0].global);
    CHECK(test.writes[0].repository_path == u8"C:/work/repo");
    REQUIRE(test.writes[0].changes.size() == 1);
    CHECK(test.writes[0].changes[0].key == signing_key_id::commit_gpgsign);
    CHECK(test.writes[0].changes[0].value == std::optional<std::string> { "true" });
    CHECK(test.view().writing);
    CHECK(test.view().confirming == false);

    // 쓰는 동안은 다른 변경을 받지 않는다.
    REQUIRE(test.send(set_signing_toggle_intent { signing_key_id::tag_gpgsign, signing_toggle_choice::enabled }));
    CHECK(test.writes.size() == 1);

    // 성공하면 git의 실제 값을 다시 읽고 화면 밖에 알린다 (서명 상태·커밋 상자 기본값을 다시 읽게).
    const std::size_t requests { test.info_requests.size() };
    REQUIRE(test.send(signing_write_done_intent { .ok = true, .global = false }));
    CHECK(test.info_requests.size() == requests + 1);
    REQUIRE(test.changed.size() == 1);
    CHECK_FALSE(test.changed[0]);
    CHECK(test.view().message == u8"저장했습니다.");
    CHECK_FALSE(test.view().message_error);
    CHECK_FALSE(test.view().writing);

    // 지우기와 형식·키도 같은 길이다.
    REQUIRE(test.send(set_signing_toggle_intent { signing_key_id::tag_gpgsign, signing_toggle_choice::unset }));
    REQUIRE(test.send(signing_write_done_intent { .ok = true }));
    REQUIRE(test.send(set_signing_format_intent { std::nullopt }));
    REQUIRE(test.send(signing_write_done_intent { .ok = true }));
    REQUIRE(test.send(choose_signing_key_intent { u8"C:/Users/me/.ssh/work.pub" }));
    REQUIRE(test.writes.size() == 4);
    CHECK_FALSE(test.writes[1].changes[0].value.has_value());
    CHECK(test.writes[1].changes[0].key == signing_key_id::tag_gpgsign);
    CHECK(test.writes[2].changes[0].key == signing_key_id::gpg_format);
    CHECK_FALSE(test.writes[2].changes[0].value.has_value());
    CHECK(test.writes[3].changes[0].value == std::optional<std::string> { "C:/Users/me/.ssh/work.pub" });
}

TEST_CASE("global changes wait for a confirmation and can be cancelled", "[signing][settings]")
{
    signing_harness test {};
    test.controller.on_page_opened();
    REQUIRE(test.send(signing_info_loaded_intent { sample_info() }));
    REQUIRE(test.send(select_signing_scope_intent { true }));

    REQUIRE(test.send(set_signing_toggle_intent { signing_key_id::commit_gpgsign, signing_toggle_choice::disabled }));
    // 확인 전에는 아무것도 쓰지 않는다.
    CHECK(test.writes.empty());
    signing_page_view view { test.view() };
    CHECK(view.confirming);
    CHECK(view.confirm_text.find(u8"전역 git 설정") != std::u8string::npos);
    CHECK(view.confirm_text.find(u8"commit.gpgsign을 끕니다") != std::u8string::npos);

    REQUIRE(test.send(cancel_global_signing_intent {}));
    CHECK(test.writes.empty());
    CHECK_FALSE(test.view().confirming);

    REQUIRE(test.send(set_signing_toggle_intent { signing_key_id::commit_gpgsign, signing_toggle_choice::disabled }));
    REQUIRE(test.send(confirm_global_signing_intent {}));
    REQUIRE(test.writes.size() == 1);
    CHECK(test.writes[0].global);
    CHECK(test.writes[0].changes[0].value == std::optional<std::string> { "false" });
    CHECK_FALSE(test.view().confirming);

    REQUIRE(test.send(signing_write_done_intent { .ok = true, .global = true }));
    REQUIRE(test.changed.size() == 1);
    CHECK(test.changed[0]);

    // 범위를 바꾸면 확인을 기다리던 변경은 버린다.
    REQUIRE(test.send(set_signing_toggle_intent { signing_key_id::tag_gpgsign, signing_toggle_choice::enabled }));
    CHECK(test.view().confirming);
    REQUIRE(test.send(select_signing_scope_intent { false }));
    CHECK_FALSE(test.view().confirming);
    CHECK(test.writes.size() == 1);
}

TEST_CASE("a failed write shows the reason and reads the real values again", "[signing][settings]")
{
    signing_harness test {};
    test.controller.on_page_opened();
    REQUIRE(test.send(signing_info_loaded_intent { sample_info() }));
    REQUIRE(test.send(set_signing_toggle_intent { signing_key_id::commit_gpgsign, signing_toggle_choice::enabled }));
    const std::size_t requests { test.info_requests.size() };
    REQUIRE(test.send(signing_write_done_intent { .ok = false, .global = false, .error = u8"commit.gpgsign 설정을 바꾸지 못했습니다. error: could not lock config file" }));
    const signing_page_view view { test.view() };
    CHECK(view.message_error);
    CHECK(view.message.find(u8"could not lock") != std::u8string::npos);
    CHECK(test.info_requests.size() == requests + 1);
    CHECK(test.changed.empty());
}

TEST_CASE("without a repository tab only the global scope is available", "[signing][settings]")
{
    signing_harness test { u8"" };
    test.controller.on_page_opened();
    CHECK(test.info_requests[0].empty());
    signing_info_snapshot info { sample_info() };
    info.repository = false;
    REQUIRE(test.send(signing_info_loaded_intent { std::move(info) }));
    const signing_page_view view { test.view() };
    CHECK_FALSE(view.repository);
    CHECK(view.global_scope);
    // 전역으로만 바꾸므로 확인을 거친다.
    REQUIRE(test.send(set_signing_toggle_intent { signing_key_id::tag_gpgsign, signing_toggle_choice::enabled }));
    CHECK(test.writes.empty());
    CHECK(test.view().confirming);
    REQUIRE(test.send(confirm_global_signing_intent {}));
    REQUIRE(test.writes.size() == 1);
    CHECK(test.writes[0].global);
    CHECK(test.writes[0].repository_path.empty());
}

TEST_CASE("program paths are validated, applied with forward slashes and can be cleared", "[signing][settings]")
{
    signing_harness test {};
    test.controller.on_page_opened();
    REQUIRE(test.send(signing_info_loaded_intent { sample_info() }));

    luil::text::text_edit_state* const input { test.controller.draft(text_target::settings_signing_ssh_program) };
    REQUIRE(input != nullptr);
    // 이 범위에는 정한 값이 없어 비어 있다. 비어 있으면 적용할 것이 없다.
    CHECK(test.view().ssh_program.can_apply == false);
    CHECK(test.view().ssh_program.can_clear == false);

    // 절대 경로가 아니면 막는다.
    input->text = u8"ssh-keygen";
    input->caret = input->anchor = input->text.size();
    signing_page_view view { test.view() };
    CHECK_FALSE(view.ssh_program.error.empty());
    CHECK_FALSE(view.ssh_program.can_apply);
    REQUIRE(test.send(apply_signing_text_intent { signing_key_id::gpg_ssh_program, false }));
    CHECK(test.writes.empty());

    input->text = u8"C:\\Windows\\System32\\OpenSSH\\ssh-keygen.exe";
    input->caret = input->anchor = input->text.size();
    view = test.view();
    CHECK(view.ssh_program.error.empty());
    CHECK(view.ssh_program.can_apply);
    REQUIRE(test.send(apply_signing_text_intent { signing_key_id::gpg_ssh_program, false }));
    REQUIRE(test.writes.size() == 1);
    CHECK(test.writes[0].changes[0].key == signing_key_id::gpg_ssh_program);
    CHECK(test.writes[0].changes[0].value == std::optional<std::string> { "C:/Windows/System32/OpenSSH/ssh-keygen.exe" });
    REQUIRE(test.send(signing_write_done_intent { .ok = true }));

    // allowed signers는 ~/로 시작해도 된다.
    luil::text::text_edit_state* const allowed { test.controller.draft(text_target::settings_signing_allowed_signers) };
    REQUIRE(allowed != nullptr);
    allowed->text = u8"~/.ssh/allowed_signers";
    CHECK(test.view().allowed_signers.error.empty());
    input->text = u8"~/gpg";
    CHECK_FALSE(test.view().ssh_program.error.empty());

    // 지우기는 이 범위의 설정을 지운다.
    signing_info_snapshot info { sample_info() };
    info.config.local.at(signing_key_id::gpg_ssh_program) = entry("C:/x/ssh-keygen.exe", config_origin::local);
    REQUIRE(test.send(signing_info_loaded_intent { std::move(info) }));
    CHECK(test.view().ssh_program.can_clear);
    CHECK(test.view().ssh_program.stored == u8"C:/x/ssh-keygen.exe");
    REQUIRE(test.send(apply_signing_text_intent { signing_key_id::gpg_ssh_program, true }));
    REQUIRE(test.writes.size() == 2);
    CHECK_FALSE(test.writes[1].changes[0].value.has_value());
}

TEST_CASE("use_default_gpg_intent sets gpg.program to the gpg of Git for Windows", "[signing][settings]")
{
    signing_harness test {};
    test.controller.on_page_opened();
    REQUIRE(test.send(signing_info_loaded_intent { sample_info() }));
    REQUIRE(test.send(use_default_gpg_intent {}));
    REQUIRE(test.writes.size() == 1);
    CHECK(test.writes[0].changes[0].key == signing_key_id::gpg_program);
    CHECK(test.writes[0].changes[0].value == std::optional<std::string> { "C:/Program Files/Git/usr/bin/gpg.exe" });

    // 못 찾았으면 아무것도 하지 않는다.
    signing_harness none {};
    signing_info_snapshot info { sample_info() };
    info.default_gpg.clear();
    REQUIRE(none.send(signing_info_loaded_intent { std::move(info) }));
    REQUIRE(none.send(use_default_gpg_intent {}));
    CHECK(none.writes.empty());
}

TEST_CASE("results and clicks that arrive after the settings were closed are swallowed", "[signing][settings]")
{
    signing_harness test {};
    test.controller.on_page_opened();
    CHECK(test.send(signing_info_loaded_intent { sample_info() }, false));
    CHECK(test.view().loaded == false);
    CHECK(test.send(set_signing_toggle_intent { signing_key_id::commit_gpgsign, signing_toggle_choice::enabled }, false));
    CHECK(test.writes.empty());
    CHECK(test.send(signing_write_done_intent { .ok = true }, false));
    CHECK(test.changed.empty());

    // 닫으면 읽은 값과 확인 대기를 버린다. 다시 열면 다시 읽는다.
    REQUIRE(test.send(signing_info_loaded_intent { sample_info() }));
    test.controller.on_closed();
    CHECK(test.view().loaded == false);
    const std::size_t requests { test.info_requests.size() };
    test.controller.on_page_opened();
    CHECK(test.info_requests.size() == requests + 1);

    // 서명 쪽의 메시지가 아니면 처리하지 않는다.
    CHECK_FALSE(test.send(refresh_ssh_intent {}));
}

TEST_CASE("check_signing_path accepts absolute paths and the home prefix only where it is allowed", "[signing][settings]")
{
    CHECK(check_signing_path(u8"", false).empty());
    CHECK(check_signing_path(u8"  C:/Program Files/Git/usr/bin/gpg.exe ", false).empty());
    CHECK(check_signing_path(u8"gpg", false).empty() == false);
    CHECK(check_signing_path(u8"~/x", false).empty() == false);
    CHECK(check_signing_path(u8"~/x", true).empty());
    CHECK(check_signing_path(u8"C:/a\nb", false).empty() == false);
    CHECK(describe_signing_value(signing_key_id::commit_gpgsign, entry("yes", config_origin::local)) == u8"켜짐");
    CHECK(describe_signing_value(signing_key_id::tag_gpgsign, signing_entry { .origin = config_origin::global, .valueless = true }) == u8"켜짐");
    CHECK(describe_signing_value(signing_key_id::gpg_format, entry("ssh", config_origin::local)) == u8"ssh");
    CHECK(describe_signing_value(signing_key_id::gpg_format, signing_entry {}).empty());
    CHECK(describe_effective_signing_value(signing_key_id::gpg_format, signing_entry {}) == u8"openpgp (기본)");
}

// ---------------------------------------------------------------------- 화면 tree

namespace {
    ui::built_tree build_signing_tree(const test::snapshot::snapshot_harness& harness, const signing_page_view& page, const float width)
    {
        app_settings settings {};
        settings_state state {};
        state.page = settings_page::signing;
        git_tool_info tool {};
        tool.availability = git_tool_availability::available;
        tool.executable = u8"C:\\Git\\cmd\\git.exe";
        tool.reported_version = u8"git version 2.52.0";
        settings_view_inputs inputs {};
        inputs.state = &state;
        inputs.settings = &settings;
        inputs.git_tool = &tool;
        view_snapshot view { harness.build(width) };
        view.settings = build_settings_view(inputs);
        view.settings->signing = page;
        return ui::build_ui_tree(view);
    }

    bool has_owner(const ui::built_tree& built, const luil::ui_element_kind kind, const std::u8string& owner)
    {
        return built.tree->find(luil::ui_element_id { kind, owner }) != nullptr;
    }
} // namespace

TEST_CASE("the signing page builds a tree with stable names for its controls", "[signing][settings][ui]")
{
    test::snapshot::snapshot_harness harness {};
    static_cast<void>(harness.open(u8"C:\\work\\repo", "repo-1"));
    signing_harness test {};
    test.controller.on_page_opened();
    REQUIRE(test.send(signing_info_loaded_intent { sample_info() }));
    const ssh_info_snapshot ssh { sample_ssh() };

    for (const float width : { 1440.0f, 900.0f, 480.0f })
    {
        INFO("width " << width);
        const ui::built_tree built { build_signing_tree(harness, test.view(&ssh), width) };
        REQUIRE(built.tree != nullptr);
        for (const luil::ui_element_id& duplicate : built.tree->duplicate_ids())
            WARN("duplicate " << std::string(duplicate.owner.begin(), duplicate.owner.end()));
        CHECK(built.tree->duplicate_ids().empty());
        CHECK(built.tree->unarranged().empty());
        CHECK(built.feedback.settings_maximum.has_value());
    }

    const ui::built_tree built { build_signing_tree(harness, test.view(&ssh), 1440.0f) };
    CHECK(has_owner(built, ui::kind_text_button, u8"settings-page-signing"));
    std::vector<std::u8string> buttons {};
    buttons.push_back(u8"settings-signing-refresh");
    buttons.push_back(u8"settings-signing-key-use-1");
    buttons.push_back(u8"settings-signing-key-clear");
    buttons.push_back(u8"settings-signing-gpg-apply");
    buttons.push_back(u8"settings-signing-gpg-clear");
    buttons.push_back(u8"settings-signing-gpg-default");
    buttons.push_back(u8"settings-signing-ssh-apply");
    buttons.push_back(u8"settings-signing-allowed-apply");
    for (const std::u8string& owner : buttons)
    {
        INFO(std::string(owner.begin(), owner.end()));
        CHECK(has_owner(built, ui::kind_text_button, owner));
    }
    CHECK(has_owner(built, ui::kind_settings_signing_gpg_input, u8"settings-signing-gpg-input"));
    CHECK(has_owner(built, ui::kind_settings_signing_ssh_input, u8"settings-signing-ssh-input"));
    CHECK(has_owner(built, ui::kind_settings_signing_allowed_input, u8"settings-signing-allowed-input"));
    // 설정 표는 이름과 적용되는 값을 글로 보인다 (UI 자동화가 이 이름으로 읽는다).
    CHECK(has_owner(built, ui::kind_label, u8"settings-signing-effective-commit.gpgsign"));
    CHECK(has_owner(built, ui::kind_label, u8"settings-signing-origin-gpg.format"));
}

TEST_CASE("the confirmation box appears for global changes and offers apply and cancel", "[signing][settings][ui]")
{
    test::snapshot::snapshot_harness harness {};
    static_cast<void>(harness.open(u8"C:\\work\\repo", "repo-1"));
    signing_harness test {};
    test.controller.on_page_opened();
    REQUIRE(test.send(signing_info_loaded_intent { sample_info() }));
    REQUIRE(test.send(select_signing_scope_intent { true }));
    REQUIRE(test.send(set_signing_toggle_intent { signing_key_id::tag_gpgsign, signing_toggle_choice::enabled }));
    const ui::built_tree built { build_signing_tree(harness, test.view(), 1440.0f) };
    CHECK(built.tree->duplicate_ids().empty());
    CHECK(has_owner(built, ui::kind_text_button, u8"settings-signing-confirm-apply"));
    CHECK(has_owner(built, ui::kind_text_button, u8"settings-signing-confirm-cancel"));
}
