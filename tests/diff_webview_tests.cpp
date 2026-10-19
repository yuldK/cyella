// diff 웹뷰를 프레임에 끼우는 쪽(정책·테마·대체 보기 판단·tree 자리표)의 test다.
#include "application/diff_view_protocol.h"
#include "domain/commit_graph_layout.h"
#include "presentation/diff_view_intents.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/diff_webview.h"
#include "presentation/ui/diff_webview_theme.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui_intents.h"

#include "view_snapshot_support.h"

#include "luil/theme/ui_theme.h"
#include "luil/ui/webview_element.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace cyella;
using namespace cyella::test::snapshot;

namespace {
    // 고른 파일의 diff를 세션에 싣는다. `show_diff`가 읽는 값은 세션의 `selected_diff`다.
    void load_diff(repository_session& session, const diff_file_request& request, const bool binary = false)
    {
        session.expect_file_diff(5);
        file_diff diff {};
        diff.request = request;
        diff_file file {};
        file.old_path = file.new_path = request.path;
        file.is_binary = binary;
        diff_hunk hunk {};
        hunk.header = "@@ -1 +1 @@";
        hunk.lines.push_back({ diff_line_kind::del, 1, 0, "a" });
        hunk.lines.push_back({ diff_line_kind::add, 0, 1, "b" });
        if (binary == false)
            file.hunks.push_back(std::move(hunk));
        diff.files.push_back(std::move(file));
        REQUIRE(session.apply(5, file_diff_loaded { std::move(diff) }));
    }

    struct opened_diff
    {
        snapshot_harness harness {};
        repository_tab_id tab { 0 };

        opened_diff()
        {
            tab = harness.open(u8"C:\\work\\repo", "repo-1");
            load(*harness.sessions["repo-1"], linear_history(3), 2);
            harness.ui.tabs[tab].selected_sha = std::string { graph_wip_sha };
            harness.ui.tabs[tab].diff = diff_file_request { .path = "src/a.cpp", .view = diff_view::unstaged };
            load_diff(*harness.sessions["repo-1"], *harness.ui.tabs[tab].diff);
        }
    };
} // namespace

TEST_CASE("css colors are written as #rrggbb with an alpha only when translucent", "[diff][webview]")
{
    CHECK(ui::css_color(luil::make_ui_color(0x1e, 0x1e, 0x1e)) == "#1e1e1e");
    CHECK(ui::css_color(luil::make_ui_color(0xff, 0xff, 0xff, 0xbf)) == "#ffffffbf");
    CHECK(ui::css_color(luil::make_ui_color(0x00, 0x90, 0x70, 0x00)) == "#00907000");
}

TEST_CASE("the diff theme carries the luil palette into the viewer's css variables", "[diff][webview]")
{
    const luil::ui_color_palette dark { luil::color_palette_for(luil::color_theme::dark) };
    const diff_theme dark_theme { ui::make_diff_theme(dark) };
    CHECK(dark_theme.mode == diff_theme_mode::dark);
    CHECK(dark_theme.tokens.at("--window-background") == ui::css_color(dark.window_background));
    CHECK(dark_theme.tokens.at("--accent") == ui::css_color(dark.accent));
    CHECK(dark_theme.tokens.at("--primary-foreground") == ui::css_color(dark.primary_foreground));

    const diff_theme light_theme { ui::make_diff_theme(luil::color_palette_for(luil::color_theme::light)) };
    CHECK(light_theme.mode == diff_theme_mode::light);
    // 테마가 바뀌면 값이 달라야 컨트롤러가 `set_theme`을 보낸다.
    CHECK_FALSE(light_theme == dark_theme);
    CHECK(light_theme.tokens.at("--window-background") != dark_theme.tokens.at("--window-background"));

    // 뷰어는 `--`로 시작하는 소문자·숫자·`-` 이름만 받는다. 모든 토큰이 그 규칙을 지키고 값은 CSS 색이다.
    for (const auto& [name, value] : dark_theme.tokens)
    {
        INFO(name);
        REQUIRE(name.starts_with("--"));
        for (const char letter : name.substr(2))
            CHECK(((letter >= 'a' && letter <= 'z') || (letter >= '0' && letter <= '9') || letter == '-'));
        REQUIRE(value.size() >= 7);
        CHECK(value.front() == '#');
    }
    // 뷰어가 쓰는 diff 줄 바탕과 레인 색이 빠지지 않았다.
    for (const char* const name : { "--diff-add-background", "--diff-add-strong", "--diff-del-background", "--diff-del-strong", "--diff-hunk-background", "--lane-0", "--lane-7", "--success-accent" })
        CHECK(dark_theme.tokens.contains(name));
}

TEST_CASE("the diff webview policy opens only file urls, takes 8 MiB messages and keeps developer tools off", "[diff][webview]")
{
    const luil::ui_webview webview { ui::make_diff_webview({ u8"file:///C:/app/diff-viewer/index.html", u8"C:\\data\\webview", 3, u8"{\"type\":\"clear\"}" }) };
    CHECK(webview.id == u8"diff");
    CHECK(webview.user_data_folder == u8"C:\\data\\webview");
    CHECK(webview.navigate_url == u8"file:///C:/app/diff-viewer/index.html");
    CHECK(webview.navigate_revision == 1);
    CHECK(webview.post_revision == 3);
    CHECK(webview.post_message == u8"{\"type\":\"clear\"}");
    REQUIRE(webview.policy.allowed_schemes.size() == 1);
    CHECK(webview.policy.allowed_schemes[0] == u8"file");
    CHECK(webview.policy.maximum_message_bytes == diff_view_maximum_message_bytes);
    CHECK_FALSE(webview.policy.developer_tools_enabled);
    CHECK_FALSE(webview.policy.default_context_menus_enabled);
    CHECK_FALSE(webview.policy.open_new_windows_here);
    REQUIRE(webview.on_event != nullptr);

    // 웹뷰 사건은 logic thread에서 앱 메시지로 받는다.
    luil::webview_event event {};
    event.kind = luil::webview_event_kind::web_message_received;
    event.message = u8"{}";
    const luil::app_message message { webview.on_event(event) };
    REQUIRE(message.get<ui::diff_webview_event_intent>() != nullptr);
    CHECK(message.get<ui::diff_webview_event_intent>()->event.message == u8"{}");
}

TEST_CASE("only the viewer we opened is trusted, ignoring case and slash style", "[diff][webview]")
{
    const std::u8string viewer { u8"file:///C:/Program%20Files/cyella/diff-viewer/index.html" };
    CHECK(ui::is_viewer_document(viewer, viewer));
    CHECK(ui::is_viewer_document(u8"FILE:///c:/program%20files/cyella/diff-viewer/index.html", viewer));
    CHECK_FALSE(ui::is_viewer_document(u8"file:///C:/Program%20Files/cyella/diff-viewer/other.html", viewer));
    CHECK_FALSE(ui::is_viewer_document(u8"file:///C:/evil/index.html", viewer));
    CHECK_FALSE(ui::is_viewer_document(u8"", viewer));
    CHECK_FALSE(ui::is_viewer_document(viewer + u8"#x", viewer));
}

TEST_CASE("with a webview the snapshot skips the line copy and fallback keeps it", "[diff][webview][snapshot]")
{
    opened_diff test {};

    SECTION("fallback text view")
    {
        const view_snapshot view { test.harness.build() };
        REQUIRE(view.repository->diff.has_value());
        const diff_panel_view& diff { *view.repository->diff };
        CHECK_FALSE(diff.use_webview);
        CHECK_FALSE(diff.loading);
        CHECK(diff.lines.size() == 3);
    }

    SECTION("webview surface")
    {
        test.harness.ui.diff_surface.webview_available = true;
        test.harness.ui.diff_surface.content = diff_content::text;
        test.harness.ui.diff_surface.layout = diff_layout::split;
        const view_snapshot view { test.harness.build() };
        const diff_panel_view& diff { *view.repository->diff };
        CHECK(diff.use_webview);
        CHECK(diff.lines.empty());
        CHECK(diff.surface.layout == diff_layout::split);
        CHECK(diff.title == u8"a.cpp");
    }
}

TEST_CASE("the diff area is a webview slot when the webview is usable and a line list otherwise", "[diff][webview][tree]")
{
    opened_diff test {};
    const luil::ui_element_id slot_id { luil::ui_element_kind::webview, u8"diff" };

    SECTION("webview")
    {
        test.harness.ui.diff_surface.webview_available = true;
        test.harness.ui.diff_surface.content = diff_content::text;
        const ui::built_tree built { ui::build_ui_tree(test.harness.build()) };
        REQUIRE(built.tree != nullptr);
        const luil::ui_element* const slot { built.tree->find(slot_id) };
        REQUIRE(slot != nullptr);
        CHECK(dynamic_cast<const luil::webview_element*>(slot) != nullptr);
        CHECK(built.tree->duplicate_ids().empty());
        CHECK(built.tree->unarranged().empty());
        // 머리 막대의 inline·분할 전환은 본문이 텍스트일 때만 선다.
        CHECK(built.tree->find(luil::ui_element_id { ui::kind_layout, u8"diff-header" }) != nullptr);
    }

    SECTION("fallback")
    {
        const ui::built_tree built { ui::build_ui_tree(test.harness.build()) };
        REQUIRE(built.tree != nullptr);
        CHECK(built.tree->find(slot_id) == nullptr);
        CHECK(built.feedback.diff_maximum.has_value());
        CHECK(built.tree->duplicate_ids().empty());
        CHECK(built.tree->unarranged().empty());
    }
}

TEST_CASE("the header offers inline or split for text and a compare mode for images", "[diff][webview][tree]")
{
    opened_diff test {};
    test.harness.ui.diff_surface.webview_available = true;

    const auto has_group = [&test](const char8_t* const owner) {
        const ui::built_tree built { ui::build_ui_tree(test.harness.build()) };
        return built.tree->ids_of_kind(luil::ui_element_kind::choice_group).empty() == false && [&] {
            for (const luil::ui_element_id& id : built.tree->ids_of_kind(luil::ui_element_kind::choice_group))
                if (id.owner == owner)
                    return true;
            return false;
        }();
    };

    test.harness.ui.diff_surface.content = diff_content::text;
    CHECK(has_group(u8"diff-layout"));
    CHECK_FALSE(has_group(u8"diff-image-mode"));

    test.harness.ui.diff_surface.content = diff_content::image;
    CHECK(has_group(u8"diff-image-mode"));
    CHECK_FALSE(has_group(u8"diff-layout"));

    test.harness.ui.diff_surface.content = diff_content::binary;
    CHECK_FALSE(has_group(u8"diff-image-mode"));
    CHECK_FALSE(has_group(u8"diff-layout"));
}
