// 셸 controller의 화면 메시지 처리다 (탭 열기·닫기, 최근 목록, 테마, 고르기, 스크롤 다듬기, 입력 초안).
// workspace는 가짜 runner와 수동 dispatcher로 세운다 — job을 실행하지 않고 낸 것만 센다.
#include "application/git_workspace.h"
#include "infrastructure/git_operation_runner.h"
#include "presentation/dialog_intents.h"
#include "presentation/shell_controller.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"

#include "luil/ui/transition.h"

#include "fake_git_runner.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
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
            dependencies.steady_clock = [] { return steady_now; };
            return dependencies;
        }

        // 토스트가 떠오르고 물러나는 시계다. test가 손으로 앞으로 보낸다.
        static inline std::chrono::steady_clock::time_point steady_now { std::chrono::hours { 1 } };

        [[nodiscard]] std::chrono::steady_clock::time_point steady() const noexcept
        {
            return steady_now;
        }

        void advance(const std::chrono::steady_clock::duration delta) noexcept
        {
            steady_now += delta;
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

TEST_CASE("resizing reveals the selected tab while manual scrolling keeps its position", "[presentation][controller][tabs]")
{
    controller_harness test {};
    for (int index { 0 }; index < 7; ++index)
        REQUIRE(test.send(new_tab_intent {}));
    const auto wide { ui::build_ui_tree(test.controller.make_view(1440.0f, 900.0f, 1.0f)) };
    test.controller.apply_feedback(wide.feedback);
    REQUIRE(test.send(scroll_tabs_intent { 0.0f, true }));
    const view_snapshot manual { test.controller.make_view(1440.0f, 900.0f, 1.0f) };
    CHECK_FALSE(manual.reveal_selected_tab);
    CHECK(manual.tab_scroll == 0.0f);
    const auto unchanged { ui::build_ui_tree(manual) };
    CHECK(unchanged.feedback.tab_scroll == 0.0f);
    test.controller.apply_feedback(unchanged.feedback);
    const view_snapshot narrow { test.controller.make_view(480.0f, 900.0f, 1.0f) };
    CHECK(narrow.reveal_selected_tab);
    const auto resized { ui::build_ui_tree(narrow) };
    REQUIRE(resized.feedback.tab_scroll.has_value());
    CHECK(*resized.feedback.tab_scroll > 0.0f);
}

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

TEST_CASE("workspace navigation works for a clean repository and preserves the commit draft", "[presentation][controller][workspace]")
{
    controller_harness test {};
    REQUIRE(test.send(set_repository_layout_intent { false }));
    test.controller.open_repository(u8"C:\\work\\repo");
    REQUIRE(test.send(select_workspace_intent { true }));
    const auto id { test.controller.tabs().selected().id };
    CHECK(test.controller.state().tabs.at(id).selected_sha == "WIP");
    luil::text_edit_request insert {};
    insert.target = to_luil_target(text_target::commit_subject);
    insert.command = luil::text::text_edit_command::insert;
    insert.text = u8"작성 중인 메시지";
    REQUIRE(test.send(edit_text_intent { insert }));
    REQUIRE(test.send(select_workspace_intent { false }));
    CHECK(test.controller.state().tabs.at(id).selected_sha.empty());
    REQUIRE(test.send(select_workspace_intent { true }));
    CHECK(test.controller.state().tabs.at(id).commit_subject.text == u8"작성 중인 메시지");
    CHECK_FALSE(test.controller.state().drawer_open);
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
    REQUIRE(test.send(select_workspace_intent { true }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).selected_sha == "WIP");
    REQUIRE(test.send(select_workspace_intent { false }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).selected_sha == sha_of(1));
}

TEST_CASE("choosing a changed file in the drawer closes it so the diff is not covered", "[presentation][controller]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    repository_session* const session { test.workspace.find_session(test.controller.selected_repository_id()) };
    REQUIRE(session != nullptr);
    const std::uint64_t generation { session->begin_refresh() };
    repository_snapshot snapshot {};
    snapshot.summary.head_sha = sha_of(2);
    REQUIRE(session->apply(snapshot_loaded { generation, snapshot }));
    std::vector<commit_info> commits { { .sha = sha_of(2), .parents = { sha_of(1) } }, { .sha = sha_of(1) } };
    REQUIRE(session->apply(commits_loaded { .generation = generation, .commits = commits, .page_complete = true, .history_complete = true }));

    // medium 배치(논리 폭 1000)에서는 상세가 서랍이다. 커밋을 고르면 서랍이 열린다.
    static_cast<void>(test.controller.make_view(1000.0f, 800.0f, 1.0f));
    REQUIRE(test.send(select_commit_intent { graph_item_key(sha_of(1)) }));
    REQUIRE(test.controller.state().drawer_open);

    // 서랍의 변경 파일을 고르면 그래프 자리에 diff가 열리고 서랍은 닫힌다.
    REQUIRE(test.send(select_file_intent { diff_file_request { .path = "a.txt", .view = diff_view::commit, .sha = sha_of(1) } }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).diff.has_value());
    CHECK_FALSE(test.controller.state().drawer_open);
}

TEST_CASE("the commit file menu opens the diff and copies the path forms", "[presentation][controller]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    const menu_target target { .kind = menu_target_kind::commit_file, .name = "src/app/a.cpp", .sha = sha_of(1) };

    REQUIRE(test.send(open_context_menu_intent { target, 0.0f, 0.0f }));
    REQUIRE(test.send(menu_select_intent { u8"open_diff" }));
    const auto& diff { test.controller.state().tabs.at(test.controller.tabs().selected().id).diff };
    REQUIRE(diff.has_value());
    CHECK(diff->path == "src/app/a.cpp");
    CHECK(diff->view == diff_view::commit);
    CHECK(diff->sha == sha_of(1));

    const auto copied = [&test, &target](const std::u8string& key) {
        REQUIRE(test.send(open_context_menu_intent { target, 0.0f, 0.0f }));
        REQUIRE(test.send(menu_select_intent { key }));
        return test.controller.take_clipboard_text().value_or(u8"");
    };
    CHECK(copied(u8"copy_path") == u8"src/app/a.cpp");
    CHECK(copied(u8"copy_name") == u8"a.cpp");
    const std::string prefix { sha_of(1).substr(0, 7) };
    CHECK(copied(u8"copy_revision_path") == std::u8string { prefix.begin(), prefix.end() } + u8":src/app/a.cpp");
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
    test.controller.apply_feedback({ .working_maximum = 75.0f });
    REQUIRE(test.send(scroll_intent { .target = scroll_target::working_area, .value = 1000.0f, .absolute = true }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).working_scroll == 75.0f);
    REQUIRE(test.send(scroll_intent { .target = scroll_target::working_area, .value = -1000.0f }));
    CHECK(test.controller.state().tabs.at(test.controller.tabs().selected().id).working_scroll == 0.0f);
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
    REQUIRE(test.controller.state().latest_notification() != nullptr);
    CHECK(test.controller.state().latest_notification()->text.find(u8"원격이 없어") != std::u8string::npos);
    // ×로 닫은 토스트는 물러난 뒤 지워진다.
    REQUIRE(test.send(dismiss_notification_intent {}));
    test.controller.tick(test.steady() + toast_leave_duration);
    CHECK(test.controller.state().latest_notification() == nullptr);
}

TEST_CASE("notices rise as toasts, leave smoothly when they time out and stay in the notification center", "[presentation][controller][notifications]")
{
    controller_harness test {};
    REQUIRE(test.send(show_notice_intent { u8"첫 안내", false }));
    const notification* first { test.controller.state().latest_notification() };
    REQUIRE(first != nullptr);
    CHECK(first->severity == notification_severity::info);
    CHECK(first->toast);
    CHECK(first->toast_shown_at == test.steady());
    REQUIRE(first->toast_until.has_value());
    const auto expires { *first->toast_until };

    // 떠오르는 동안은 frame마다 다시 짓고, 다 떠오르면 내려갈 시각까지 잠잔다.
    REQUIRE(test.controller.next_tick().has_value());
    CHECK(*test.controller.next_tick() == test.steady() + luil::transition_frame);
    CHECK(test.controller.make_view(1440.0f, 900.0f, 1.0f).toasts.front().presence == 0.0f);
    test.advance(toast_enter_duration / 2);
    const float rising { test.controller.make_view(1440.0f, 900.0f, 1.0f).toasts.front().presence };
    CHECK(rising > 0.0f);
    CHECK(rising < 1.0f);
    test.advance(toast_enter_duration);
    CHECK(test.controller.make_view(1440.0f, 900.0f, 1.0f).toasts.front().presence == 1.0f);
    REQUIRE(test.controller.next_tick().has_value());
    CHECK(*test.controller.next_tick() == expires);

    // 시간이 되면 물러나기 시작하고, 물러남이 끝나야 토스트에서 빠진다. 센터에는 남는다.
    test.controller.tick(expires);
    REQUIRE(test.controller.state().latest_notification()->toast_leaving_at.has_value());
    CHECK(test.controller.state().latest_notification()->toast);
    test.controller.tick(expires + toast_leave_duration);
    CHECK_FALSE(test.controller.state().latest_notification()->toast);
    CHECK_FALSE(test.controller.next_tick().has_value());
    view_snapshot view { test.controller.make_view(1440.0f, 900.0f, 1.0f) };
    CHECK(view.toasts.empty());
    CHECK(view.status.notifications == 1);
    CHECK(view.status.unread_notifications == 1);

    // 떠 있는 동안 같은 안내가 또 오면 하나로 합치고, 다시 떠오르지 않는다. 내려간 것은 센터에 그대로 남는다.
    REQUIRE(test.send(show_notice_intent { u8"첫 안내", false }));
    const auto risen_at { test.controller.state().latest_notification()->toast_shown_at };
    test.advance(std::chrono::seconds { 1 });
    REQUIRE(test.send(show_notice_intent { u8"첫 안내", false }));
    CHECK(test.controller.state().notifications.size() == 2);
    CHECK(test.controller.state().latest_notification()->toast_shown_at == risen_at);

    // 종을 누르면 센터가 열리고 모두 읽은 것이 되며 토스트는 내려간다.
    REQUIRE(test.send(toggle_notification_center_intent {}));
    view = test.controller.make_view(1440.0f, 900.0f, 1.0f);
    CHECK(view.toasts.empty());
    REQUIRE(view.notification_center.has_value());
    CHECK(view.notification_center->entries.size() == 2);
    CHECK(view.status.unread_notifications == 0);
    const ui::built_tree built { ui::build_ui_tree(view) };
    CHECK(std::any_of(built.popups.begin(), built.popups.end(), [](const ui::built_popup& popup) { return popup.notifications; }));
    // 센터가 열려 있는 동안 온 알림은 토스트로 뜨지 않고 센터에 바로 보인다.
    REQUIRE(test.send(show_notice_intent { u8"새 안내", true }));
    CHECK_FALSE(test.controller.state().latest_notification()->toast);
    CHECK(test.controller.state().latest_notification()->read);

    // Esc는 센터를 먼저 닫는다. 바깥 누름으로 닫힌 바로 뒤의 종 누름은 그 누름이라 다시 열지 않는다.
    REQUIRE(test.send(close_overlays_intent {}));
    CHECK_FALSE(test.controller.state().notifications_open);
    REQUIRE(test.send(toggle_notification_center_intent {}));
    REQUIRE(test.send(close_notification_center_intent { .dismissed = true }));
    REQUIRE(test.send(toggle_notification_center_intent {}));
    CHECK_FALSE(test.controller.state().notifications_open);
    REQUIRE(test.send(toggle_notification_center_intent {}));
    CHECK(test.controller.state().notifications_open);

    // 센터의 항목은 ×로 바로 지운다.
    REQUIRE(test.send(dismiss_notification_intent { test.controller.state().notifications.front().id }));
    CHECK(test.controller.state().notifications.size() == 2);
    REQUIRE(test.send(clear_notifications_intent {}));
    CHECK(test.controller.state().notifications.empty());
}

TEST_CASE("closing a toast lets it leave first and then removes it, and Esc sends every toast away", "[presentation][controller][notifications]")
{
    controller_harness test {};
    REQUIRE(test.send(show_notice_intent { u8"첫 안내", false }));
    REQUIRE(test.send(show_notice_intent { u8"두 번째 안내", true }));
    test.advance(toast_enter_duration);
    const std::uint64_t closed { test.controller.state().notifications.front().id };

    REQUIRE(test.send(dismiss_notification_intent { closed }));
    // 물러나는 동안은 아직 목록에 있고 덜 나와 있다.
    REQUIRE(test.controller.state().notifications.size() == 2);
    test.advance(toast_leave_duration / 2);
    view_snapshot view { test.controller.make_view(1440.0f, 900.0f, 1.0f) };
    REQUIRE(view.toasts.size() == 2);
    CHECK(view.toasts.back().id == closed);
    CHECK(view.toasts.back().presence < 1.0f);
    CHECK(view.toasts.front().presence == 1.0f);
    test.controller.tick(test.steady() + toast_leave_duration);
    REQUIRE(test.controller.state().notifications.size() == 1);
    CHECK(test.controller.state().notifications.front().id != closed);

    // Esc는 남은 토스트를 물러나게 할 뿐 센터에서 지우지 않는다.
    REQUIRE(test.send(close_overlays_intent {}));
    test.controller.tick(test.steady() + toast_leave_duration);
    REQUIRE(test.controller.state().notifications.size() == 1);
    CHECK_FALSE(test.controller.state().notifications.front().toast);
}

TEST_CASE("toasts float above the body as rounded cards and the status bar bell shows unread notifications", "[presentation][controller][notifications]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:\\work\\repo");
    ui::built_tree built { ui::build_ui_tree(test.controller.make_view(1440.0f, 900.0f, 1.0f)) };
    const luil::ui_element* bell { built.tree->find({ ui::kind_button, u8"status-notifications" }) };
    REQUIRE(bell != nullptr);
    CHECK(built.popups.empty());

    REQUIRE(test.send(show_notice_intent { u8"안내입니다.", true }));
    REQUIRE(test.send(show_notice_intent { u8"두 번째 안내입니다.", false }));
    test.advance(toast_enter_duration);
    const view_snapshot view { test.controller.make_view(1440.0f, 900.0f, 1.0f) };
    built = ui::build_ui_tree(view);
    // 토스트는 popup 창이 아니라 주 tree의 카드다 (둥근 모서리와 부드러운 그림자·흐려지기를 그린다).
    CHECK(built.popups.empty());
    REQUIRE(view.toasts.size() == 2);
    const luil::ui_element* newest { built.tree->find({ ui::kind_panel, u8"toast-" + ui::number_text(view.toasts[0].id) }) };
    const luil::ui_element* older { built.tree->find({ ui::kind_panel, u8"toast-" + ui::number_text(view.toasts[1].id) }) };
    REQUIRE(newest != nullptr);
    REQUIRE(older != nullptr);
    // 최신이 상태 줄 가까이(아래)에 오고 앞의 것이 그 위에 쌓인다. 둘 다 오른쪽 끝에서 조금 떨어져 뜬다.
    CHECK(newest->bounds().y > older->bounds().y);
    CHECK(newest->bounds().y + newest->bounds().height < 900.0f - shell_status_height);
    CHECK(newest->bounds().x + newest->bounds().width < 1440.0f);
    CHECK(newest->bounds().x > 720.0f);
    CHECK(built.tree->find({ ui::kind_button, u8"toast-" + ui::number_text(view.toasts[0].id) + u8"-close" }) != nullptr);
    CHECK(built.tree->find({ ui::kind_button, u8"status-notifications" })->tooltip().find(u8"2") != std::u8string::npos);
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

TEST_CASE("changing repository layout preserves the draft and exact diff request without submitting Git work", "[presentation][controller][layout]")
{
    controller_harness test {};
    test.controller.open_repository(u8"C:/work/repo");
    REQUIRE(test.send(select_workspace_intent { true }));
    luil::text_edit_request insert {};
    insert.target = to_luil_target(text_target::commit_subject);
    insert.command = luil::text::text_edit_command::insert;
    insert.text = u8"배치와 무관한 초안";
    REQUIRE(test.send(edit_text_intent { insert }));
    const diff_file_request request { .path = "src/a.cpp", .view = diff_view::staged };
    REQUIRE(test.send(select_file_intent { request }));
    const auto before { test.controller.current_diff_target() };
    const auto pending { test.dispatcher.pending() };
    const auto tab { test.controller.tabs().selected().id };
    for (const bool graph : { false, true, false })
    {
        REQUIRE(test.send(set_repository_layout_intent { graph }));
        CHECK(test.controller.settings().ui.graph_layout == graph);
        REQUIRE(test.controller.current_diff_target().has_value());
        CHECK(test.controller.current_diff_target()->request == before->request);
        CHECK(test.controller.state().tabs.at(tab).commit_subject.text == insert.text);
        CHECK(test.dispatcher.pending() == pending);
        CHECK_FALSE(test.controller.state().drawer_open);
    }
}
