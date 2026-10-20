// 셸 controller의 화면 메시지 처리다 (탭 열기·닫기, 최근 목록, 테마, 고르기, 스크롤 다듬기, 입력 초안).
// workspace는 가짜 runner와 수동 dispatcher로 세운다 — job을 실행하지 않고 낸 것만 센다.
#include "application/git_workspace.h"
#include "infrastructure/git_operation_runner.h"
#include "presentation/shell_controller.h"
#include "presentation/ui_intents.h"

#include "fake_git_runner.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {
    using namespace cyella;

    struct controller_harness
    {
        controller_harness()
            : runner { process, u8"C:/Git/cmd/git.exe", files, {} }
            , workspace { runner, dispatcher.dispatcher(), events.sink(), nullptr, {} }
            , controller { make_dependencies(workspace) }
        {
            controller.start({});
        }

        static shell_controller::dependencies make_dependencies(git_workspace& workspace)
        {
            shell_controller::dependencies dependencies {};
            dependencies.workspace = &workspace;
            dependencies.clock = [] { return std::chrono::system_clock::time_point { std::chrono::seconds { 1760000000 } }; };
            return dependencies;
        }

        template<typename intent_type>
        bool send(intent_type intent)
        {
            const bool handled { controller.handle(luil::app_message { std::move(intent) }) };
            controller.after_events();
            return handled;
        }

        test::fake_git_runner process {};
        test::fake_workspace_files files {};
        test::manual_dispatcher dispatcher {};
        test::event_queue events {};
        git_operation_runner runner;
        git_workspace workspace;
        shell_controller controller;
    };

    std::string sha_of(const int index)
    {
        char buffer[41] {};
        std::snprintf(buffer, sizeof(buffer), "%08x%032x", index, index);
        return buffer;
    }
} // namespace

TEST_CASE("opening a repository replaces the start tab and records it as recent", "[presentation][controller]")
{
    controller_harness test {};
    REQUIRE(test.controller.tabs().tabs().size() == 1);
    CHECK(test.controller.tabs().selected().kind == repository_tab_kind::start);

    test.controller.open_repository(u8"C:\\work\\repo");
    REQUIRE(test.controller.tabs().tabs().size() == 1);
    CHECK(test.controller.tabs().selected().kind == repository_tab_kind::repository);
    CHECK_FALSE(test.controller.selected_repository_id().empty());
    REQUIRE(test.controller.settings().recent_repositories.size() == 1);
    CHECK(test.controller.settings().recent_repositories[0].opened_at == u8"2025-10-09T08:53:20Z");
    CHECK(test.controller.settings().open_tabs == std::vector<std::u8string> { u8"C:\\work\\repo" });
    CHECK(test.controller.settings().active_tab == u8"C:\\work\\repo");

    // 같은 저장소(대소문자·구분자 무시)는 새 탭을 만들지 않는다.
    test.controller.open_repository(u8"c:/work/REPO/");
    CHECK(test.controller.tabs().tabs().size() == 1);
}

TEST_CASE("new tab, cycling and closing keep the workspace in step", "[presentation][controller]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\one");
    REQUIRE(test.send(new_tab_intent {}));
    CHECK(test.controller.tabs().tabs().size() == 2);
    CHECK(test.controller.tabs().selected().kind == repository_tab_kind::start);
    test.controller.open_repository(u8"C:\\work\\two");
    CHECK(test.controller.tabs().tabs().size() == 2);
    CHECK(test.workspace.repository_ids().size() == 2);

    REQUIRE(test.send(cycle_tab_intent { true }));
    CHECK(test.controller.tabs().selected().path == u8"C:\\work\\one");
    REQUIRE(test.send(close_selected_tab_intent {}));
    CHECK(test.controller.tabs().tabs().size() == 1);
    CHECK(test.workspace.repository_ids().size() == 1);
    CHECK(test.controller.settings().open_tabs == std::vector<std::u8string> { u8"C:\\work\\two" });
}

TEST_CASE("recent list pin and remove go to settings", "[presentation][controller]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    REQUIRE(test.send(pin_recent_intent { u8"C:\\work\\repo", true }));
    CHECK(test.controller.settings().recent_repositories[0].pinned);
    REQUIRE(test.send(remove_recent_intent { u8"C:\\work\\repo" }));
    CHECK(test.controller.settings().recent_repositories.empty());
}

TEST_CASE("the theme button cycles system, light and dark", "[presentation][controller]")
{
    controller_harness test {};
    REQUIRE(test.send(cycle_theme_intent {}));
    CHECK(test.controller.settings().appearance.theme == theme_preference::light);
    REQUIRE(test.send(cycle_theme_intent {}));
    CHECK(test.controller.settings().appearance.theme == theme_preference::dark);
    REQUIRE(test.send(cycle_theme_intent {}));
    CHECK(test.controller.settings().appearance.theme == theme_preference::system);
}

TEST_CASE("selecting a commit submits show_commit and WIP does not", "[presentation][controller]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    repository_session* const session { test.workspace.find_session(test.controller.selected_repository_id()) };
    REQUIRE(session != nullptr);
    const std::uint64_t generation { session->begin_refresh() };
    repository_snapshot snapshot {};
    snapshot.summary.head_sha = sha_of(2);
    snapshot.summary.changed_file_count = 1;
    REQUIRE(session->apply(snapshot_loaded { generation, snapshot }));
    std::vector<commit_info> commits { { .sha = sha_of(2), .parents = { sha_of(1) } }, { .sha = sha_of(1) } };
    REQUIRE(session->apply(commits_loaded { .generation = generation, .commits = commits, .page_complete = true, .history_complete = true }));

    REQUIRE(test.send(select_commit_intent { u8"WIP" }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).selected_sha == "WIP");
    // WIP 행은 show_commit을 내지 않는다. (보이는 커밋의 서명을 읽는 자동 operation은 나갈 수 있다.)
    for (const operation_id id : test.workspace.operation_order())
        CHECK(test.workspace.find_operation(id)->kind != operation_kind::show_commit);
    const std::size_t before { test.dispatcher.pending() };

    REQUIRE(test.send(select_commit_intent { graph_item_key(sha_of(1)) }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).selected_sha == sha_of(1));
    CHECK(test.dispatcher.pending() == before + 1);

    // 상세가 서랍이 아닌 wide 배치에서는 서랍을 열지 않는다.
    CHECK_FALSE(test.controller.state().drawer_open);
}

TEST_CASE("scroll is clamped to the last arranged maximum", "[presentation][controller]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    test.controller.apply_feedback({ .graph_maximum = 100.0f, .graph_viewport = 400.0f });
    REQUIRE(test.send(scroll_intent { .target = scroll_target::graph, .value = 80.0f }));
    REQUIRE(test.send(scroll_intent { .target = scroll_target::graph, .value = 80.0f }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).graph_scroll == 100.0f);
    REQUIRE(test.send(scroll_intent { .target = scroll_target::graph, .value = -500.0f }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).graph_scroll == 0.0f);
    REQUIRE(test.send(scroll_intent { .target = scroll_target::graph, .value = 40.0f, .absolute = true }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).graph_scroll == 40.0f);
}

TEST_CASE("text edits land in the targeted draft and stay on one line", "[presentation][controller]")
{
    controller_harness test {};
    luil::text_edit_request insert {};
    insert.target = to_luil_target(text_target::start_search);
    insert.command = luil::text::text_edit_command::insert;
    insert.text = u8"cy\nella";
    REQUIRE(test.send(edit_text_intent { insert }));
    CHECK(test.controller.state().start_search.text == u8"cy ella");

    luil::text_edit_request clear {};
    clear.target = to_luil_target(text_target::start_search);
    clear.command = luil::text::text_edit_command::clear;
    REQUIRE(test.send(edit_text_intent { clear }));
    CHECK(test.controller.state().start_search.text.empty());
}

TEST_CASE("the commit body preserves line breaks and moves to the current line edge", "[presentation][controller][working]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    luil::text_edit_request request {};
    request.target = to_luil_target(text_target::commit_body);
    request.command = luil::text::text_edit_command::insert;
    request.text = u8"first line\nsecond line";
    REQUIRE(test.send(edit_text_intent { request }));
    const auto& tabs { test.controller.state().tabs };
    REQUIRE(tabs.size() == 1);
    CHECK(tabs.begin()->second.commit_body.text == u8"first line\nsecond line");
    request.command = luil::text::text_edit_command::move_line_start;
    request.text.clear();
    REQUIRE(test.send(edit_text_intent { request }));
    CHECK(tabs.begin()->second.commit_body.caret == 11);
    REQUIRE(test.send(commit_body_line_intent { false, false }));
    CHECK(tabs.begin()->second.commit_body.caret == 0);
}

TEST_CASE("toolbar actions without a dialog submit operations and the rest show a notice", "[presentation][controller]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    const std::size_t before { test.dispatcher.pending() };
    REQUIRE(test.send(toolbar_intent { toolbar_action::fetch }));
    CHECK(test.dispatcher.pending() == before + 1);
    // upstream이 없는 Push는 대화 상자가 필요하다. 이 저장소는 원격이 없어 안내가 뜬다.
    REQUIRE(test.send(toolbar_intent { toolbar_action::push }));
    CHECK(test.dispatcher.pending() == before + 1);
    REQUIRE(test.controller.state().notice.has_value());
    CHECK(test.controller.state().notice->text.find(u8"원격이 없어") != std::u8string::npos);
    REQUIRE(test.send(dismiss_notice_intent {}));
    CHECK_FALSE(test.controller.state().notice.has_value());
}

TEST_CASE("pane resizing is clamped", "[presentation][controller]")
{
    controller_harness test {};
    REQUIRE(test.send(resize_pane_intent { .sidebar = true, .delta = 10000.0f }));
    CHECK(test.controller.state().sidebar_width == maximum_sidebar_width);
    REQUIRE(test.send(resize_pane_intent { .sidebar = false, .delta = -10000.0f }));
    CHECK(test.controller.state().detail_width == minimum_detail_width);
}

TEST_CASE("pane widths are saved after the drag settles and restored on start", "[presentation][controller]")
{
    controller_harness test {};
    REQUIRE(test.send(resize_pane_intent { .sidebar = true, .delta = 40.0f }));
    REQUIRE(test.send(resize_pane_intent { .sidebar = false, .delta = -30.0f }));
    // 끄는 동안에는 쓰지 않는다. 쓸 시각만 예약한다.
    CHECK_FALSE(test.controller.settings().ui.pane_widths.sidebar.has_value());
    REQUIRE(test.controller.next_tick().has_value());
    test.controller.tick(*test.controller.next_tick() - std::chrono::milliseconds { 1 });
    CHECK_FALSE(test.controller.settings().ui.pane_widths.sidebar.has_value());

    test.controller.tick(*test.controller.next_tick());
    CHECK_FALSE(test.controller.next_tick().has_value());
    CHECK(test.controller.settings().ui.pane_widths.sidebar == default_sidebar_width + 40.0f);
    CHECK(test.controller.settings().ui.pane_widths.detail == default_detail_width - 30.0f);

    // 앱을 닫을 때는 기다리지 않고 바로 쓴다.
    REQUIRE(test.send(resize_pane_intent { .sidebar = true, .delta = 8.0f }));
    test.controller.flush();
    CHECK(test.controller.settings().ui.pane_widths.sidebar == default_sidebar_width + 48.0f);
    CHECK_FALSE(test.controller.next_tick().has_value());
}

namespace {
    // 정해 둔 설정을 돌려주고 저장한 마지막 값을 기억하는 저장소다.
    class memory_settings_store final : public cyella::app_settings_store
    {
    public:
        [[nodiscard]] cyella::app_settings_load_result load(std::u8string_view) noexcept override
        {
            cyella::app_settings_load_result result {};
            result.settings = initial;
            return result;
        }

        [[nodiscard]] cyella::app_settings_save_result save(std::u8string_view, const cyella::app_settings& settings, std::u8string_view) noexcept override
        {
            saved = settings;
            cyella::app_settings_save_result result {};
            result.succeeded = true;
            return result;
        }

        cyella::app_settings initial {};
        cyella::app_settings saved {};
    };
} // namespace

TEST_CASE("saved pane widths are restored and clamped on start", "[presentation][controller]")
{
    using namespace cyella;
    memory_settings_store store {};
    store.initial.ui.pane_widths.sidebar = 300.0f;
    store.initial.ui.pane_widths.detail = 9999.0f;
    app_settings_service service { store, u8"memory" };
    static_cast<void>(service.load());

    shell_controller::dependencies dependencies {};
    dependencies.settings = &service;
    shell_controller controller { std::move(dependencies) };
    controller.start({});
    CHECK(controller.state().sidebar_width == 300.0f);
    CHECK(controller.state().detail_width == maximum_detail_width);

    REQUIRE(controller.handle(luil::app_message { resize_pane_intent { .sidebar = true, .delta = 10.0f } }));
    controller.flush();
    CHECK(store.saved.ui.pane_widths.sidebar == 310.0f);
}

TEST_CASE("the open diff of the selected tab is the webview's wanted target", "[presentation][controller][diff]")
{
    controller_harness test {};
    CHECK_FALSE(test.controller.current_diff_target().has_value());

    test.controller.open_repository(u8"C:\\work\\repo");
    CHECK_FALSE(test.controller.current_diff_target().has_value());

    const diff_file_request request { .path = "src/a.cpp", .view = diff_view::staged };
    REQUIRE(test.send(select_file_intent { request }));
    const std::optional<diff_target> target { test.controller.current_diff_target() };
    REQUIRE(target.has_value());
    CHECK(target->repository_id == test.controller.selected_repository_id());
    CHECK(target->repository_path == u8"C:\\work\\repo");
    CHECK(target->request == request);

    // 다른 탭(시작 탭)으로 가면 그 탭에는 열린 diff가 없다. 돌아오면 다시 있다.
    REQUIRE(test.send(new_tab_intent {}));
    CHECK_FALSE(test.controller.current_diff_target().has_value());

    REQUIRE(test.send(close_diff_intent {}));
    CHECK_FALSE(test.controller.current_diff_target().has_value());
}

TEST_CASE("the diff surface is stored for the header bar", "[presentation][controller][diff]")
{
    controller_harness test {};
    diff_surface_state surface {};
    surface.webview_available = true;
    surface.layout = diff_layout::split;
    surface.content = diff_content::image;
    test.controller.set_diff_surface(surface);
    CHECK(test.controller.state().diff_surface == surface);
}
