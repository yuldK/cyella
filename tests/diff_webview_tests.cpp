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

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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

    // 뷰어는 `--`로 시작하는 소문자·숫자·`-` 이름만 받는다. 글꼴 외의 값은 CSS 색이다.
    for (const auto& [name, value] : dark_theme.tokens)
    {
        INFO(name);
        REQUIRE(name.starts_with("--"));
        for (const char letter : name.substr(2))
            CHECK(((letter >= 'a' && letter <= 'z') || (letter >= '0' && letter <= '9') || letter == '-'));
        REQUIRE(value.size() >= 7);
        if (name != "--ui-font" && name != "--code-font")
            CHECK(value.front() == '#');
    }
    // 뷰어가 쓰는 diff 줄 바탕과 레인 색이 빠지지 않았다.
    for (const char* const name : { "--diff-add-background", "--diff-add-strong", "--diff-del-background", "--diff-del-strong", "--diff-hunk-background", "--lane-0", "--lane-7", "--success-accent" })
        CHECK(dark_theme.tokens.contains(name));
}

TEST_CASE("the diff theme palette follows the system appearance the way the luil surface does", "[diff][webview][theme]")
{
    const luil::accent_definition blue { luil::accent_for(u8"blue") };
    luil::system_appearance system {};

    // 시스템 선호는 OS의 밝은 모드를 따르고, 명시한 선호는 OS를 보지 않는다.
    system.prefers_light = true;
    CHECK(ui::surface_palette_for(luil::theme_preference::system, system, u8"blue") == luil::color_palette_for(luil::color_theme::light, blue));
    CHECK(ui::surface_palette_for(luil::theme_preference::dark, system, u8"blue") == luil::color_palette_for(luil::color_theme::dark, blue));
    system.prefers_light = false;
    CHECK(ui::surface_palette_for(luil::theme_preference::system, system, u8"blue") == luil::color_palette_for(luil::color_theme::dark, blue));
    CHECK(ui::surface_palette_for(luil::theme_preference::light, system, u8"blue") == luil::color_palette_for(luil::color_theme::light, blue));

    // 고대비는 어느 선호보다 세고, 키 컬러가 아니라 사용자가 고른 대비 색으로 칠한다. 흰 바탕 고대비로 본다 — 기본 대비 색(검정 바탕)은
    // 키 컬러 경로의 고대비 팔레트와 같아 둘을 가리지 못한다.
    system.high_contrast = true;
    system.contrast_colors = luil::high_contrast_colors {
        .window_background = luil::make_ui_color(255, 255, 255),
        .window_foreground = luil::make_ui_color(0, 0, 0),
        .highlight_background = luil::make_ui_color(55, 0, 110),
        .highlight_foreground = luil::make_ui_color(255, 255, 255),
        .emphasis = luil::make_ui_color(0, 0, 159),
        .button_background = luil::make_ui_color(255, 255, 255),
        .button_foreground = luil::make_ui_color(0, 0, 0),
        .disabled_foreground = luil::make_ui_color(96, 0, 0),
    };
    for (const luil::theme_preference preference : { luil::theme_preference::system, luil::theme_preference::light, luil::theme_preference::dark })
    {
        const luil::ui_color_palette palette { ui::surface_palette_for(preference, system, u8"blue") };
        CHECK(palette == luil::high_contrast_palette_for(system.contrast_colors));
        CHECK(palette != luil::color_palette_for(luil::color_theme::high_contrast, blue));
        CHECK(palette.window_background == luil::make_ui_color(255, 255, 255));
    }
    // 뷰어도 흰 바탕을 밝은 테마로 받는다.
    CHECK(ui::make_diff_theme(ui::surface_palette_for(luil::theme_preference::dark, system, u8"blue")).mode == diff_theme_mode::light);
}

TEST_CASE("diff font preferences quote family names and preserve fallback stacks", "[diff][webview][fonts]")
{
    const diff_theme theme { ui::make_diff_theme(luil::color_palette_for(luil::color_theme::dark), u8"맑은 고딕", u8"Example\"Mono\\Font") };
    CHECK(theme.tokens.at("--ui-font").starts_with("\"맑은 고딕\", "));
    CHECK(theme.tokens.at("--code-font").starts_with("\"Example\\\"Mono\\\\Font\", "));
    CHECK(theme.tokens.at("--code-font").find("monospace") != std::string::npos);
}

TEST_CASE("diff font preferences survive the set_theme message to the viewer", "[diff][webview][fonts]")
{
    // 글꼴 값은 늘 따옴표를 담으므로 직렬화의 토큰 거르기를 통과하는지 메시지까지 따라가 본다.
    const diff_theme theme { ui::make_diff_theme(luil::color_palette_for(luil::color_theme::dark), u8"맑은 고딕", u8"D2Coding") };
    const nlohmann::json message { nlohmann::json::parse(serialize_set_theme(1, 0, theme)) };
    const nlohmann::json& tokens { message.at("theme").at("tokens") };
    REQUIRE(tokens.contains("--ui-font"));
    REQUIRE(tokens.contains("--code-font"));
    CHECK(tokens.at("--ui-font") == theme.tokens.at("--ui-font"));
    CHECK(tokens.at("--code-font") == theme.tokens.at("--code-font"));

    // 기본 글꼴 목록(사용자가 고르지 않은 경우)도 같다.
    const diff_theme fallback { ui::make_diff_theme(luil::color_palette_for(luil::color_theme::light)) };
    const nlohmann::json fallback_message { nlohmann::json::parse(serialize_set_theme(1, 0, fallback)) };
    const nlohmann::json& fallback_tokens { fallback_message.at("theme").at("tokens") };
    CHECK(fallback_tokens.at("--ui-font") == fallback.tokens.at("--ui-font"));
    CHECK(fallback_tokens.at("--code-font") == fallback.tokens.at("--code-font"));
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
    // 표면이 합성 제시를 못 하는 것을 알려 받아야 그동안 텍스트 본문으로 바꾼다.
    CHECK(webview.report_presentation);
    // 자리표를 비우지 않는 위층이라야 첫 그림 전에 창 뒤가 비치지 않는다.
    CHECK(webview.layer == luil::webview_layer::overlay);
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

namespace {
    luil::webview_event webview_event_of(const luil::webview_event_kind kind, std::u8string error = {})
    {
        luil::webview_event event {};
        event.id = std::u8string { ui::diff_webview_id };
        event.kind = kind;
        event.error = std::move(error);
        return event;
    }

    luil::webview_event creation_failure_of(const luil::webview_creation_failure failure, const std::int32_t hresult, std::u8string error)
    {
        luil::webview_event event { webview_event_of(luil::webview_event_kind::creation_failed, std::move(error)) };
        event.failure = failure;
        event.hresult = hresult;
        return event;
    }

    luil::webview_event presentation_of(const bool available)
    {
        luil::webview_event event { webview_event_of(luil::webview_event_kind::presentation_changed) };
        event.presentation_available = available;
        return event;
    }
} // namespace

TEST_CASE("a working diff webview stays in use through its normal events", "[diff][webview][failure]")
{
    ui::diff_webview_health health {};
    const auto start { std::chrono::steady_clock::now() };
    CHECK(health.on_event(webview_event_of(luil::webview_event_kind::navigation_started), start) == ui::diff_webview_action::page_reloading);
    CHECK(health.on_event(webview_event_of(luil::webview_event_kind::navigation_completed), start) == ui::diff_webview_action::none);
    luil::webview_event ready { webview_event_of(luil::webview_event_kind::web_message_received) };
    ready.message = u8"{\"type\":\"ready\",\"protocol\":1}";
    CHECK(health.on_event(ready, start) == ui::diff_webview_action::web_message);
    for (const luil::webview_event_kind kind :
        { luil::webview_event_kind::navigation_blocked, luil::webview_event_kind::permission_denied, luil::webview_event_kind::download_blocked, luil::webview_event_kind::events_dropped })
        CHECK(health.on_event(webview_event_of(kind), start) == ui::diff_webview_action::none);
    // 표시할 수 있다는 알림은 아무것도 시키지 않는다. 표시 여부는 값을 실어 따로 보므로(아래 test) 위 나열에 넣지 않는다 —
    // `webview_event_of`의 기본값은 `presentation_available == false`다.
    CHECK(health.on_event(presentation_of(true), start) == ui::diff_webview_action::none);
    CHECK(health.presentable());
    CHECK_FALSE(health.failed());
    CHECK(health.reason().empty());
}

TEST_CASE("an unpresentable surface shows the text diff without failing and comes back when it can present again", "[diff][webview][webview-presentation]")
{
    const auto now { std::chrono::steady_clock::now() };

    SECTION("before the first value the webview counts as presentable")
    {
        // luil은 컨트롤러가 준비될 때 첫 값을 알린다. 그 전에 텍스트로 두면 정상 PC의 시작이 텍스트로 깜빡인다.
        const ui::diff_webview_health health {};
        CHECK(health.presentable());
        CHECK_FALSE(health.failed());
    }

    SECTION("a first true changes nothing")
    {
        ui::diff_webview_health health {};
        CHECK(health.on_event(presentation_of(true), now) == ui::diff_webview_action::none);
        CHECK(health.presentable());
    }

    SECTION("a first false shows text and tells the user once")
    {
        ui::diff_webview_health health {};
        CHECK(health.on_event(presentation_of(false), now) == ui::diff_webview_action::presentation_lost);
        CHECK_FALSE(health.presentable());
        // 실패가 아니다. 웹뷰는 frame에 남고 까닭(실패 알림의 값)도 없다.
        CHECK_FALSE(health.failed());
        CHECK(health.reason().empty());
        // 같은 값이 또 와도 다시 알리지 않는다.
        CHECK(health.on_event(presentation_of(false), now) == ui::diff_webview_action::none);
        CHECK_FALSE(health.presentable());
        // 숨은 동안에도 페이지는 살아 있다. 메시지와 다시 읽기는 그대로 지난다.
        CHECK(health.on_event(webview_event_of(luil::webview_event_kind::web_message_received), now) == ui::diff_webview_action::web_message);
        CHECK(health.on_event(webview_event_of(luil::webview_event_kind::navigation_started), now) == ui::diff_webview_action::page_reloading);
    }

    SECTION("true then false then true returns to the viewer and tells only the first loss")
    {
        ui::diff_webview_health health {};
        CHECK(health.on_event(presentation_of(true), now) == ui::diff_webview_action::none);
        CHECK(health.on_event(presentation_of(false), now) == ui::diff_webview_action::presentation_lost);
        CHECK_FALSE(health.presentable());
        CHECK(health.on_event(presentation_of(true), now) == ui::diff_webview_action::none);
        CHECK(health.presentable());
        // 다시 잃어도 실행당 한 번만 알린다.
        CHECK(health.on_event(presentation_of(false), now) == ui::diff_webview_action::none);
        CHECK_FALSE(health.presentable());
        CHECK(health.on_event(presentation_of(true), now) == ui::diff_webview_action::none);
        CHECK(health.presentable());
        CHECK_FALSE(health.failed());
    }

    SECTION("a failure while unpresentable still falls back for the run")
    {
        ui::diff_webview_health health {};
        CHECK(health.on_event(presentation_of(false), now) == ui::diff_webview_action::presentation_lost);
        CHECK(health.on_event(webview_event_of(luil::webview_event_kind::browser_process_failed, u8"gone"), now) == ui::diff_webview_action::fall_back);
        CHECK(health.failed());
        CHECK(health.reason() == u8"browser_process_failed: gone");
        // 물러선 뒤에 늦게 온 표시 가능 알림은 버린다. 실패가 이긴다.
        CHECK(health.on_event(presentation_of(true), now) == ui::diff_webview_action::none);
        CHECK(health.failed());
    }

    SECTION("a late presentation event after a failure neither tells the user nor changes the state")
    {
        ui::diff_webview_health health {};
        CHECK(health.on_event(creation_failure_of(luil::webview_creation_failure::controller_failed, 0, u8"x"), now) == ui::diff_webview_action::fall_back);
        CHECK(health.on_event(presentation_of(false), now) == ui::diff_webview_action::none);
        CHECK(health.presentable());
        CHECK(health.failed());
    }
}

TEST_CASE("every kind of creation failure falls back to the text diff and says why", "[diff][webview][failure]")
{
    struct failure_case
    {
        luil::webview_creation_failure failure {};
        std::int32_t hresult { 0 };
        std::u8string expected {};
    };
    // 0x80070005(E_ACCESSDENIED)·0x80004005(E_FAIL)를 int32로 담은 값이다.
    const std::int32_t access_denied { static_cast<std::int32_t>(0x80070005u) };
    const std::int32_t unspecified { static_cast<std::int32_t>(0x80004005u) };
    const std::vector<failure_case> cases {
        { luil::webview_creation_failure::runtime_missing, 0, u8"creation_failed (runtime_missing): detail" },
        { luil::webview_creation_failure::runtime_too_old, 0, u8"creation_failed (runtime_too_old): detail" },
        { luil::webview_creation_failure::environment_failed, access_denied, u8"creation_failed (environment_failed, HRESULT 0x80070005): detail" },
        { luil::webview_creation_failure::controller_failed, unspecified, u8"creation_failed (controller_failed, HRESULT 0x80004005): detail" },
        { luil::webview_creation_failure::not_built, 0, u8"creation_failed (not_built): detail" },
    };
    for (const failure_case& item : cases)
    {
        const std::string expected { item.expected.begin(), item.expected.end() };
        INFO(expected);
        ui::diff_webview_health health {};
        const auto now { std::chrono::steady_clock::now() };
        CHECK(health.on_event(creation_failure_of(item.failure, item.hresult, u8"detail"), now) == ui::diff_webview_action::fall_back);
        CHECK(health.failed());
        CHECK(health.reason() == item.expected);

        // 물러선 뒤에 늦게 온 사건은 판단을 바꾸지도, 다시 알리게 하지도 않는다.
        CHECK(health.on_event(creation_failure_of(item.failure, item.hresult, u8"again"), now) == ui::diff_webview_action::none);
        CHECK(health.on_event(webview_event_of(luil::webview_event_kind::navigation_started), now) == ui::diff_webview_action::none);
        CHECK(health.on_event(webview_event_of(luil::webview_event_kind::web_message_received), now) == ui::diff_webview_action::none);
        CHECK(health.failed());
        CHECK(health.reason() == item.expected);
    }
}

TEST_CASE("a dead browser process or a failed viewer load falls back for the rest of the run", "[diff][webview][failure]")
{
    const auto now { std::chrono::steady_clock::now() };

    ui::diff_webview_health browser {};
    luil::webview_event ready { webview_event_of(luil::webview_event_kind::web_message_received) };
    CHECK(browser.on_event(ready, now) == ui::diff_webview_action::web_message);
    CHECK(browser.on_event(webview_event_of(luil::webview_event_kind::browser_process_failed, u8"browser process exited"), now) == ui::diff_webview_action::fall_back);
    CHECK(browser.failed());
    CHECK(browser.reason() == u8"browser_process_failed: browser process exited");

    ui::diff_webview_health navigation {};
    CHECK(navigation.on_event(webview_event_of(luil::webview_event_kind::navigation_failed, u8"web error status 3"), now) == ui::diff_webview_action::fall_back);
    CHECK(navigation.reason() == u8"navigation_failed: web error status 3");
}

TEST_CASE("a renderer crash lets luil reload once and falls back when it crashes again soon", "[diff][webview][failure]")
{
    using namespace std::chrono_literals;
    const auto start { std::chrono::steady_clock::now() };

    SECTION("a single crash reloads and keeps the webview")
    {
        ui::diff_webview_health health {};
        CHECK(health.on_event(webview_event_of(luil::webview_event_kind::render_process_failed, u8"render process exited"), start) == ui::diff_webview_action::page_reloading);
        // luil의 다시 읽기는 항해로 보이고, 다시 읽힌 페이지의 `ready`가 내용을 다시 받게 한다.
        CHECK(health.on_event(webview_event_of(luil::webview_event_kind::navigation_started), start + 1s) == ui::diff_webview_action::page_reloading);
        CHECK(health.on_event(webview_event_of(luil::webview_event_kind::web_message_received), start + 1s) == ui::diff_webview_action::web_message);
        CHECK_FALSE(health.failed());
    }

    SECTION("crashes far apart each reload")
    {
        ui::diff_webview_health health {};
        CHECK(health.on_event(webview_event_of(luil::webview_event_kind::render_process_failed), start) == ui::diff_webview_action::page_reloading);
        CHECK(health.on_event(webview_event_of(luil::webview_event_kind::render_process_failed), start + ui::diff_webview_health::render_failure_window)
            == ui::diff_webview_action::page_reloading
        );
        CHECK_FALSE(health.failed());
    }

    SECTION("a second crash inside the window falls back even if luil's wording changes")
    {
        ui::diff_webview_health health {};
        CHECK(health.on_event(webview_event_of(luil::webview_event_kind::render_process_failed, u8"render process exited"), start) == ui::diff_webview_action::page_reloading);
        CHECK(health.on_event(webview_event_of(luil::webview_event_kind::render_process_failed, u8"anything"), start + 15s) == ui::diff_webview_action::fall_back);
        CHECK(health.failed());
        CHECK(health.reason() == u8"render_process_failed: anything");
    }
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

TEST_CASE("diff file navigation keeps versions separate and follows list or tree order", "[diff][webview][snapshot]")
{
    opened_diff test {};
    repository_session& session { *test.harness.sessions["repo-1"] };
    repository_snapshot snapshot { make_snapshot(sha_of(2), 3) };
    snapshot.unstaged = {
        { .path = "a.cpp", .status = 'M' },
        { .path = "src/z.cpp", .status = 'M' },
        { .path = "src/a.cpp", .status = '?' },
    };
    const std::uint64_t generation { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { generation, snapshot }));
    REQUIRE(session.apply(commits_loaded { .generation = generation, .commits = linear_history(3), .page_complete = true, .history_complete = true }));
    test.harness.ui.tabs[test.tab].diff = diff_file_request { .path = "src/z.cpp", .context_lines = 25 };

    SECTION("list order keeps untracked and context data")
    {
        const diff_panel_view diff { *test.harness.build().repository->diff };
        CHECK(diff.file_position == 2);
        CHECK(diff.file_count == 3);
        REQUIRE(diff.previous_file.has_value());
        CHECK(diff.previous_file->path == "a.cpp");
        REQUIRE(diff.next_file.has_value());
        CHECK(diff.next_file->path == "src/a.cpp");
        CHECK(diff.next_file->untracked);
        CHECK(diff.next_file->context_lines == 25);
        CHECK(diff.next_file->view == diff_view::unstaged);
    }
    SECTION("tree order includes collapsed folders")
    {
        test.harness.settings.ui.working_files_tree = true;
        test.harness.ui.tabs[test.tab].collapsed_folders.insert("src");
        const diff_panel_view diff { *test.harness.build().repository->diff };
        REQUIRE(diff.previous_file.has_value());
        REQUIRE(diff.next_file.has_value());
        CHECK(diff.previous_file->path == "src/a.cpp");
        CHECK(diff.next_file->path == "a.cpp");
    }
    SECTION("staged navigation stays in the staged group")
    {
        test.harness.ui.tabs[test.tab].diff = diff_file_request { .path = "README.md", .view = diff_view::staged };
        const diff_panel_view diff { *test.harness.build().repository->diff };
        CHECK(diff.file_count == 1);
        CHECK_FALSE(diff.previous_file.has_value());
        CHECK_FALSE(diff.next_file.has_value());
    }
    SECTION("a file removed by refresh has no stale neighbors")
    {
        test.harness.ui.tabs[test.tab].diff->path = "removed.cpp";
        const diff_panel_view diff { *test.harness.build().repository->diff };
        CHECK(diff.file_count == 0);
        CHECK_FALSE(diff.previous_file.has_value());
        CHECK_FALSE(diff.next_file.has_value());
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

TEST_CASE("the text diff header names why the viewer is not shown", "[diff][webview][tree][webview-presentation]")
{
    opened_diff test {};
    const luil::ui_element_id note_id { ui::kind_label, u8"diff-fallback-note" };
    // 폭은 창 폭이다. diff 자리는 사이드바와 상세 패널을 뺀 나머지라 넓은 창에서만 긴 문구가 선다.
    const auto note_at = [&test, &note_id](const float width) {
        const ui::built_tree built { ui::build_ui_tree(test.harness.build(width)) };
        REQUIRE(built.tree != nullptr);
        const luil::ui_element* const note { built.tree->find(note_id) };
        REQUIRE(note != nullptr);
        return note->accessibility().name;
    };

    SECTION("no viewer")
    {
        // Runtime이 없거나 실패해 물러선 경우다. WebView2가 없다고 말한다.
        CHECK(note_at(1920.0f) == tr(u8"diff.fallback_note"));
        CHECK(note_at(480.0f) == tr(u8"diff.fallback_short"));
    }

    SECTION("the surface cannot present")
    {
        // 웹뷰는 섰고 Runtime도 있다. 머리 막대가 Runtime을 설치하라는 뜻으로 읽히지 않게 표면 때문이라고 말한다.
        test.harness.ui.diff_surface.webview_unpresentable = true;
        CHECK(note_at(1920.0f) == tr(u8"diff.fallback_note_unpresentable"));
        CHECK_FALSE(note_at(1920.0f) == tr(u8"diff.fallback_note"));
        CHECK(note_at(480.0f) == tr(u8"diff.fallback_short"));
    }
}

TEST_CASE("diff header actions stay inside the actual pane at small widths", "[diff][webview][tree]")
{
    opened_diff test {};
    test.harness.ui.diff_surface.webview_available = true;
    test.harness.ui.diff_surface.content = diff_content::text;
    test.harness.ui.diff_surface.textconv_driver = "example";
    test.harness.ui.diff_surface.truncated = true;
    for (const float width : { 480.0f, 720.0f, 900.0f, 1440.0f })
    {
        CAPTURE(width);
        view_snapshot view { test.harness.build(width) };
        view.detail_width = maximum_detail_width;
        const ui::built_tree built { ui::build_ui_tree(view) };
        const luil::ui_element* const header { built.tree->find({ ui::kind_panel, u8"diff-header-bar" }) };
        REQUIRE(header != nullptr);
        for (const std::u8string owner : { u8"diff-previous-file", u8"diff-next-file", u8"diff-open-app" })
        {
            const luil::ui_element* button { built.tree->find({ ui::kind_button, owner }) };
            if (button == nullptr)
                button = built.tree->find({ ui::kind_text_button, owner });
            REQUIRE(button != nullptr);
            CHECK(button->bounds().x >= header->bounds().x);
            CHECK(button->bounds().x + button->bounds().width <= header->bounds().x + header->bounds().width + 0.1f);
        }
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
        return built.tree->ids_of_kind(ui::kind_segmented).empty() == false && [&] {
            for (const luil::ui_element_id& id : built.tree->ids_of_kind(ui::kind_segmented))
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
