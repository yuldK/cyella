// 환경 설정 대화 상자를 tree로 지어 보는 test다. 어느 쪽(page)이든 id가 겹치지 않고 모두 배치되며, 사용자가 누를 단추가
// 이름으로 찾아지는지(UI 자동화가 같은 이름을 쓴다)를 본다.
#include "presentation/settings/settings_intents.h"
#include "presentation/settings/settings_view.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/settings/appearance_ui.h"

#include "view_snapshot_support.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace cyella;
using namespace cyella::test::snapshot;

namespace {
    ssh_info_snapshot ssh_sample()
    {
        ssh_info_snapshot info {};
        info.service = ssh_agent_service_status::running;
        info.agent_list_available = true;
        info.agent.state = ssh_agent_list_state::identities;
        info.agent.identities.push_back({ 256, u8"SHA256:abc", u8"me@PC", u8"ED25519" });
        info.keys.push_back({ u8"id_ed25519", u8"ssh-ed25519", u8"me@PC", u8"ssh-ed25519 AAAA me@PC", u8"C:\\.ssh\\id_ed25519" });
        info.keys.push_back({ u8"work", u8"ssh-rsa", u8"", u8"ssh-rsa BBBB", u8"C:\\.ssh\\work" });
        return info;
    }

    // 쪽마다 그 쪽에 있어야 할 요소의 owner 이름이다.
    struct page_case
    {
        settings_page page { settings_page::general };
        std::vector<std::u8string> buttons {};
        std::vector<std::u8string> inputs {};
    };

    ui::built_tree build_tree(const snapshot_harness& harness, settings_state state, const float width, const bool edit = false)
    {
        app_settings settings {};
        settings.external_diff_tools.push_back({ { u8".png" }, u8"C:\\Tools\\imgdiff.exe", u8"{left} {right}" });
        git_tool_info tool {};
        tool.availability = git_tool_availability::available;
        tool.executable = u8"C:\\Git\\cmd\\git.exe";
        tool.reported_version = u8"git version 2.52.0";
        if (edit)
            state.tool_edit = tool_edit_state {};
        state.ssh = ssh_sample();

        settings_view_inputs inputs {};
        inputs.state = &state;
        inputs.settings = &settings;
        inputs.git_tool = &tool;
        inputs.ssh_directory = u8"C:\\Users\\me\\.ssh";
        inputs.ssh_tools_found = true;
        inputs.ssh_tools_origin = u8"Git for Windows";

        view_snapshot view { harness.build(width) };
        view.settings = build_settings_view(inputs);
        if (state.page == settings_page::shortcuts)
            view.settings->shortcuts = build_shortcuts_page_view(settings, std::nullopt, std::nullopt);
        if (state.page == settings_page::profiles)
            view.settings->profiles = build_profiles_page_view(settings, std::nullopt, {}, std::nullopt, {});
        return ui::build_ui_tree(view);
    }

    bool has(const ui::built_tree& built, const luil::ui_element_kind kind, const std::u8string& owner)
    {
        return built.tree->find(luil::ui_element_id { kind, owner }) != nullptr;
    }
} // namespace

TEST_CASE("every settings page builds a tree with unique ids and no unarranged elements", "[settings][ui]")
{
    snapshot_harness harness {};
    static_cast<void>(harness.open(u8"C:\\work\\repo", "repo-1"));

    for (const settings_page page :
        { settings_page::general, settings_page::diff_tools, settings_page::ssh, settings_page::signing, settings_page::hosting, settings_page::profiles, settings_page::shortcuts })
        for (const float width : { 1440.0f, 900.0f, 480.0f })
        {
            INFO("page " << static_cast<int>(page) << " width " << width);
            settings_state state {};
            state.page = page;
            const ui::built_tree built { build_tree(harness, state, width, page == settings_page::diff_tools) };
            REQUIRE(built.tree != nullptr);
            for (const luil::ui_element_id& duplicate : built.tree->duplicate_ids())
            {
                INFO(std::string(duplicate.owner.begin(), duplicate.owner.end()));
                FAIL_CHECK("duplicate settings element id");
            }
            CHECK(built.tree->unarranged().empty());
            CHECK(built.feedback.settings_maximum.has_value());
        }
}

TEST_CASE("the page navigation and the close button are always there", "[settings][ui]")
{
    snapshot_harness harness {};
    static_cast<void>(harness.open(u8"C:\\work\\repo", "repo-1"));
    const ui::built_tree built { build_tree(harness, settings_state {}, 1440.0f) };
    for (const std::u8string& owner : { u8"settings-page-general", u8"settings-page-diff-tools", u8"settings-page-ssh" })
    {
        INFO(std::string(owner.begin(), owner.end()));
        CHECK(has(built, luil::ui_element_kind::list_row, owner));
    }
    CHECK(has(built, luil::ui_element_kind::modal_host, u8"settings-dialog"));
}

TEST_CASE("each page offers its own controls under stable names", "[settings][ui]")
{
    snapshot_harness harness {};
    static_cast<void>(harness.open(u8"C:\\work\\repo", "repo-1"));

    const std::vector<page_case> cases {
        { settings_page::general, { u8"settings-git-browse", u8"settings-git-apply", u8"settings-git-reset" }, { u8"settings-git-path" } },
        { settings_page::diff_tools, { u8"settings-tool-add", u8"settings-tool-edit-0", u8"settings-tool-remove-0" }, {} },
        {
            settings_page::ssh,
            { u8"settings-ssh-refresh", u8"settings-key-copy-id_ed25519", u8"settings-key-copy-work", u8"settings-keygen-create", u8"settings-test-run" },
            { u8"settings-keygen-name", u8"settings-keygen-comment", u8"settings-test-host" },
        },
    };
    for (const page_case& expected : cases)
    {
        settings_state state {};
        state.page = expected.page;
        const ui::built_tree built { build_tree(harness, state, 1440.0f) };
        for (const std::u8string& owner : expected.buttons)
        {
            INFO(std::string(owner.begin(), owner.end()));
            CHECK(has(built, ui::kind_text_button, owner));
        }
        for (const std::u8string& owner : expected.inputs)
        {
            INFO(std::string(owner.begin(), owner.end()));
            // 입력 칸의 kind는 칸마다 다르다. owner로 찾는다.
            bool found { false };
            for (const luil::ui_element_kind kind : { ui::kind_settings_git_path_input, ui::kind_settings_key_name_input, ui::kind_settings_key_comment_input, ui::kind_settings_test_host_input })
                found = found || has(built, kind, owner);
            CHECK(found);
        }
    }
}

TEST_CASE("the tool form offers the token chips and a live preview", "[settings][ui]")
{
    snapshot_harness harness {};
    static_cast<void>(harness.open(u8"C:\\work\\repo", "repo-1"));
    settings_state state {};
    state.page = settings_page::diff_tools;
    const ui::built_tree built { build_tree(harness, state, 1440.0f, true) };
    const std::vector<std::u8string> buttons {
        u8"settings-tool-chip-left",
        u8"settings-tool-chip-right",
        u8"settings-tool-chip-base",
        u8"settings-tool-chip-path",
        u8"settings-tool-save",
        u8"settings-tool-cancel",
        u8"settings-tool-browse",
    };
    for (const std::u8string& owner : buttons)
    {
        INFO(std::string(owner.begin(), owner.end()));
        CHECK(has(built, ui::kind_text_button, owner));
    }
    CHECK(has(built, ui::kind_settings_tool_extensions_input, u8"settings-tool-extensions-input"));
    CHECK(has(built, ui::kind_settings_tool_executable_input, u8"settings-tool-executable-input"));
    CHECK(has(built, ui::kind_settings_tool_arguments_input, u8"settings-tool-arguments-input"));
    CHECK(has(built, ui::kind_label, u8"settings-tool-preview"));
}

TEST_CASE("a read only settings file puts a banner above the page", "[settings][ui]")
{
    snapshot_harness harness {};
    static_cast<void>(harness.open(u8"C:\\work\\repo", "repo-1"));
    settings_state state {};
    app_settings settings {};
    settings_view_inputs inputs {};
    inputs.state = &state;
    inputs.settings = &settings;

    view_snapshot view { harness.build() };
    view.settings = build_settings_view(inputs);
    CHECK_FALSE(has(ui::build_ui_tree(view), ui::kind_banner, u8"settings-banner"));

    inputs.read_only = true;
    view.settings = build_settings_view(inputs);
    const ui::built_tree built { ui::build_ui_tree(view) };
    CHECK(has(built, ui::kind_banner, u8"settings-banner"));
    CHECK(built.tree->duplicate_ids().empty());
    CHECK(built.tree->unarranged().empty());
}

TEST_CASE("a running ssh task offers a cancel button", "[settings][ui]")
{
    snapshot_harness harness {};
    static_cast<void>(harness.open(u8"C:\\work\\repo", "repo-1"));
    settings_state state {};
    state.page = settings_page::ssh;
    app_settings settings {};
    settings_view_inputs inputs {};
    inputs.state = &state;
    inputs.settings = &settings;
    inputs.ssh_tools_found = true;
    inputs.ssh_directory = u8"C:\\Users\\me\\.ssh";
    inputs.keygen.id = 9;
    inputs.keygen.state = operation_state::needs_input;
    inputs.keygen.waiting_for_prompt = true;
    inputs.keygen.cancellable = true;

    view_snapshot view { harness.build(1440.0f) };
    view.settings = build_settings_view(inputs);
    const ui::built_tree built { ui::build_ui_tree(view) };
    CHECK(has(built, ui::kind_text_button, u8"settings-keygen-cancel"));
    CHECK_FALSE(has(built, ui::kind_text_button, u8"settings-test-cancel"));
}

TEST_CASE("appearance uses preview choices and editable font combo boxes", "[settings][ui][fonts]")
{
    snapshot_harness harness {};
    for (const float width : { 480.0f, 900.0f, 1440.0f })
    {
        CAPTURE(width);
        const ui::built_tree built { build_tree(harness, settings_state {}, width) };
        REQUIRE(built.tree->duplicate_ids().empty());
        for (const std::u8string owner : { u8"settings-theme-light", u8"settings-theme-system", u8"settings-theme-dark" })
        {
            const luil::ui_element* const option { built.tree->find({ ui::kind_theme_option, owner }) };
            REQUIRE(option != nullptr);
            CHECK(option->accessibility().role == luil::access_role::radio_button);
            CHECK(option->bounds().width >= 80.0f);
        }
        for (const std::u8string owner : { u8"settings-ui-font-combo", u8"settings-code-font-combo" })
        {
            const luil::ui_element* const combo { built.tree->find({ ui::kind_font_combo, owner }) };
            REQUIRE(combo != nullptr);
            CHECK(combo->accessibility().role == luil::access_role::combo_box);
            REQUIRE(combo->children().size() == 2);
            CHECK(combo->children().front()->text_input().has_value());
        }
    }
}

TEST_CASE("the key color swatches are a radio group that marks the saved color", "[settings][ui][accent]")
{
    snapshot_harness harness {};
    for (const float width : { 480.0f, 1440.0f })
    {
        CAPTURE(width);
        const ui::built_tree built { build_tree(harness, settings_state {}, width) };
        REQUIRE(built.tree->duplicate_ids().empty());
        const luil::ui_element* const picker { built.tree->find({ ui::kind_accent_picker, u8"settings-accent" }) };
        REQUIRE(picker != nullptr);
        CHECK(picker->accessibility().role == luil::access_role::radio_group);
        // 저장된 값이 없으면 luil의 기본 키 컬러가 고른 것이다.
        const luil::ui_element* const fallback { built.tree->find({ ui::kind_accent_option, u8"settings-accent-" + std::u8string { luil::default_accent_id } }) };
        REQUIRE(fallback != nullptr);
        CHECK(fallback->accessibility().selected == true);
        // 목록의 마지막 견본까지 격자 안에 들어온다 (줄바꿈 높이를 담는 쪽이 맞게 잡았다).
        const luil::accent_definition& last { luil::accent_catalog().back() };
        const luil::ui_element* const tail { built.tree->find({ ui::kind_accent_option, u8"settings-accent-" + std::u8string { last.id } }) };
        REQUIRE(tail != nullptr);
        CHECK(tail->bounds().y + tail->bounds().height <= picker->bounds().y + picker->bounds().height + 0.5f);
        CHECK(tail->accessibility().selected == false);
    }

    settings_state state {};
    app_settings settings {};
    settings.appearance.accent = u8"violet";
    settings_view_inputs inputs {};
    inputs.state = &state;
    inputs.settings = &settings;
    view_snapshot view { harness.build(1440.0f) };
    view.settings = build_settings_view(inputs);
    const ui::built_tree built { ui::build_ui_tree(view) };
    const luil::ui_element* const violet { built.tree->find({ ui::kind_accent_option, u8"settings-accent-violet" }) };
    REQUIRE(violet != nullptr);
    CHECK(violet->accessibility().selected == true);
    CHECK(violet->accessibility().name == ui::accent_title(luil::accent_for(u8"violet")));
}

TEST_CASE("settings navigation exposes the selected page as a selection item", "[settings][ui]")
{
    snapshot_harness harness {};
    for (const float width : { 480.0f, 1440.0f })
    {
        settings_state state {};
        state.page = settings_page::ssh;
        const ui::built_tree built { build_tree(harness, state, width) };
        const luil::ui_element* const selected { built.tree->find({ luil::ui_element_kind::list_row, u8"settings-page-ssh" }) };
        const luil::ui_element* const other { built.tree->find({ luil::ui_element_kind::list_row, u8"settings-page-general" }) };
        REQUIRE(selected != nullptr);
        REQUIRE(other != nullptr);
        CHECK(selected->accessibility().selected == true);
        CHECK(other->accessibility().selected == false);
        CHECK(other->access_actions({ .command = luil::access_command::select }).has_value());
    }
}

TEST_CASE("narrow settings keep command titles and file app actions readable", "[settings][ui]")
{
    snapshot_harness harness {};
    for (const float width : { 480.0f, 600.0f, 1440.0f })
    {
        CAPTURE(width);
        settings_state state {};
        state.page = settings_page::shortcuts;
        const ui::built_tree shortcuts { build_tree(harness, state, width) };
        const luil::ui_element* const title { shortcuts.tree->find({ ui::kind_label, u8"shortcut-title-view.changes" }) };
        REQUIRE(title != nullptr);
        CHECK(title->bounds().width >= 160.0f);

        state.page = settings_page::diff_tools;
        const ui::built_tree apps { build_tree(harness, state, width) };
        const luil::ui_element* const row { apps.tree->find({ ui::kind_panel, u8"settings-tool-bar-app-0" }) };
        REQUIRE(row != nullptr);
        for (const std::u8string owner : { u8"settings-tool-edit-app-0", u8"settings-tool-remove-app-0" })
        {
            const luil::ui_element* const button { apps.tree->find({ ui::kind_text_button, owner }) };
            REQUIRE(button != nullptr);
            CHECK(button->bounds().x >= row->bounds().x);
            CHECK(button->bounds().x + button->bounds().width <= row->bounds().x + row->bounds().width + 0.1f);
        }
    }
}

TEST_CASE("the tab shape and tab status cards are radio groups with inert preview bars", "[settings][ui][tabs]")
{
    snapshot_harness harness {};
    settings_state state {};
    app_settings settings {};
    settings.appearance.tabs = tab_shape::flat;
    settings.appearance.tab_marker = tab_marker_place::close_slot;
    settings_view_inputs inputs {};
    inputs.state = &state;
    inputs.settings = &settings;
    view_snapshot view { harness.build(1440.0f) };
    view.settings = build_settings_view(inputs);
    const ui::built_tree built { ui::build_ui_tree(view) };
    REQUIRE(built.tree->duplicate_ids().empty());

    const luil::ui_element* const shapes { built.tree->find({ ui::kind_tab_style_picker, u8"settings-tab-shape" }) };
    REQUIRE(shapes != nullptr);
    CHECK(shapes->accessibility().role == luil::access_role::radio_group);
    const luil::ui_element* const flat { built.tree->find({ ui::kind_tab_style_option, u8"settings-tab-shape-flat" }) };
    const luil::ui_element* const connected { built.tree->find({ ui::kind_tab_style_option, u8"settings-tab-shape-connected" }) };
    REQUIRE(flat != nullptr);
    REQUIRE(connected != nullptr);
    CHECK(flat->accessibility().selected == true);
    CHECK(connected->accessibility().selected == false);

    const luil::ui_element* const close_slot { built.tree->find({ ui::kind_tab_style_option, u8"settings-tab-marker-close_slot" }) };
    REQUIRE(close_slot != nullptr);
    CHECK(close_slot->accessibility().selected == true);
    REQUIRE(built.tree->find({ ui::kind_tab_style_option, u8"settings-tab-marker-badge" }) != nullptr);
    REQUIRE(built.tree->find({ ui::kind_tab_style_option, u8"settings-tab-marker-icon_corner" }) != nullptr);

    // 견본 막대의 탭은 누를 수 없는 그림이다. 진짜 탭 kind로 찾히지 않는다.
    CHECK(built.tree->find({ luil::ui_element_kind::tab, u8"settings-tab-shape-flat-preview-0" }) == nullptr);

    // 카드를 누르면 그 값을 고르자는 메시지다.
    const luil::ui_action* const pick { connected->action(luil::ui_trigger::left_click) };
    REQUIRE(pick != nullptr);
    const auto actions { (*pick)(luil::ui_action_context { connected->id(), 0.0f, 0.0f }) };
    REQUIRE(actions.size() == 1);
    const auto* const picked { std::get_if<luil::app_message>(&actions.front()) };
    REQUIRE(picked != nullptr);
    REQUIRE(picked->get<set_tab_shape_intent>() != nullptr);
    CHECK(picked->get<set_tab_shape_intent>()->shape == tab_shape::connected);
}
