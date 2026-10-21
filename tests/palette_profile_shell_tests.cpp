// 명령 팔레트·단축키 설정·저장소별 사용자 프로필을 셸 controller와 정책, tree에서 본다. 신원 읽기·쓰기는 driver의 일이라 여기서는 함수로 꽂은
// 가짜가 부른 횟수를 센다. git 설정은 어디에서도 읽거나 쓰지 않는다.
#include "application/git_workspace.h"
#include "infrastructure/git_operation_runner.h"
#include "presentation/build_view_snapshot.h"
#include "presentation/commands/command_intents.h"
#include "presentation/graph_geometry.h"
#include "presentation/palette/palette_intents.h"
#include "presentation/settings/profiles_intents.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/settings/shortcuts_intents.h"
#include "presentation/shell_controller.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/cyella_policy.h"
#include "presentation/ui/element_kinds.h"

#include "fake_git_runner.h"
#include "view_snapshot_support.h"

#include "luil/ui/ui_tree.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace {
    using namespace cyella;
    namespace snap = cyella::test::snapshot;

    struct identity_call
    {
        std::string repository_id {};
        std::u8string path {};
    };

    struct apply_call
    {
        std::string repository_id {};
        std::u8string path {};
        std::u8string user_name {};
        std::u8string user_email {};
    };

    struct p6_harness
    {
        explicit p6_harness(const bool with_identity = true)
            : runner { process, u8"C:/Git/cmd/git.exe", files, {} }
            , workspace { runner, dispatcher.dispatcher(), events.sink(), nullptr, {} }
            , controller { make_dependencies(with_identity) }
        {
            controller.start({});
            controller.open_repository(u8"E:\\work\\repo");
            repository = controller.selected_repository_id();
            repository_session* const session { workspace.find_session(repository) };
            REQUIRE(session != nullptr);
            snap::load(*session, snap::linear_history(5), 0);
            controller.after_events();
        }

        shell_controller::dependencies make_dependencies(const bool with_identity)
        {
            shell_controller::dependencies dependencies {};
            dependencies.workspace = &workspace;
            dependencies.clock = [] { return std::chrono::system_clock::time_point { std::chrono::seconds { 1700000000 + 3600 } }; };
            if (with_identity)
            {
                dependencies.request_identity = [this](const std::string& id, const std::u8string& path) { reads.push_back(identity_call { id, path }); };
                dependencies.apply_identity
                    = [this](const std::string& id, const std::u8string& path, const std::u8string& name, const std::u8string& email) { writes.push_back(apply_call { id, path, name, email }); };
            }
            return dependencies;
        }

        template<typename intent_type>
        void send(intent_type intent)
        {
            controller.handle(luil::app_message { std::move(intent) });
            controller.after_events();
        }

        void type(const text_target target, const std::u8string& text)
        {
            luil::text_edit_request clear {};
            clear.target = to_luil_target(target);
            clear.command = luil::text::text_edit_command::clear;
            send(edit_text_intent { clear });
            luil::text_edit_request insert {};
            insert.target = to_luil_target(target);
            insert.command = luil::text::text_edit_command::insert;
            insert.text = text;
            send(edit_text_intent { insert });
        }

        [[nodiscard]] view_snapshot view()
        {
            return controller.make_view(1440.0f, 900.0f, 1.0f);
        }

        [[nodiscard]] std::vector<std::u8string> palette_keys()
        {
            std::vector<std::u8string> keys {};
            const view_snapshot snapshot { view() };
            if (snapshot.palette.has_value())
                for (const palette_item_view& item : snapshot.palette->items)
                    keys.push_back(item.key);
            return keys;
        }

        [[nodiscard]] bool palette_has(const std::u8string_view key)
        {
            const std::vector<std::u8string> keys { palette_keys() };
            return std::ranges::find(keys, key) != keys.end();
        }

        // 마지막으로 낸 operation의 요청이다.
        template<typename request_type>
        [[nodiscard]] const request_type* last_request() const
        {
            const std::deque<operation_id>& order { workspace.operation_order() };
            if (order.empty())
                return nullptr;
            const git_operation* const request { workspace.request_of(order.back()) };
            return request == nullptr ? nullptr : std::get_if<request_type>(request);
        }

        void add_profile(const std::u8string& name, const std::u8string& user_name, const std::u8string& email)
        {
            send(open_settings_intent { settings_page::profiles });
            send(add_profile_intent {});
            type(text_target::settings_profile_name, name);
            type(text_target::settings_profile_user_name, user_name);
            type(text_target::settings_profile_user_email, email);
            send(save_profile_intent {});
        }

        void add_rule(const std::u8string& prefix, const std::u8string& profile)
        {
            send(open_settings_intent { settings_page::profiles });
            type(text_target::settings_rule_prefix, prefix);
            send(choose_rule_profile_intent { profile });
            send(add_profile_rule_intent {});
        }

        void identity_is(const std::u8string& name, const std::u8string& email, const identity_origin origin = identity_origin::global)
        {
            git_identity identity {};
            identity.name = identity_value { name, origin, u8"C:/Users/me/.gitconfig" };
            identity.email = identity_value { email, origin, u8"C:/Users/me/.gitconfig" };
            send(identity_loaded_intent { .repository_id = repository, .identity = identity, .ok = true, .error = {} });
        }

        test::fake_git_runner process {};
        test::fake_workspace_files files {};
        git_operation_runner runner;
        test::manual_dispatcher dispatcher {};
        test::event_queue events {};
        git_workspace workspace;
        shell_controller controller;
        std::string repository {};
        std::vector<identity_call> reads {};
        std::vector<apply_call> writes {};
    };

    template<typename intent_type>
    const intent_type* intent_of(const std::vector<luil::input_action>& actions)
    {
        if (actions.size() != 1 || std::holds_alternative<luil::app_message>(actions[0]) == false)
            return nullptr;
        return std::get<luil::app_message>(actions[0]).get<intent_type>();
    }

    luil::key_pressed_event key_event(const luil::key_code key, const bool control = false, const bool shift = false, const bool alt = false)
    {
        luil::key_pressed_event event {};
        event.key = key;
        event.control = control;
        event.shift = shift;
        event.alt = alt;
        return event;
    }

    // 화면 전체의 tree다 (단축키 표와 팔레트가 실린다).
    std::shared_ptr<const luil::ui_tree> tree_of(const view_snapshot& view)
    {
        return ui::build_ui_tree(view).tree;
    }
} // namespace

// ------------------------------------------------------------------ 명령 팔레트

TEST_CASE("workspace commands switch views while preserving drafts and require a repository", "[palette][workspace]")
{
    p6_harness test {};
    test.send(run_command_intent { "view.changes" });
    REQUIRE(test.view().repository->detail == detail_mode::working);
    test.type(text_target::commit_subject, u8"명령으로 전환해도 남는 초안");
    test.send(run_command_intent { "view.history" });
    REQUIRE(test.view().repository->detail != detail_mode::working);
    test.send(run_command_intent { "view.changes" });
    CHECK(test.view().repository->working->subject.displayed_text() == u8"명령으로 전환해도 남는 초안");
    test.send(run_command_intent { "tab.new" });
    REQUIRE(test.view().start.has_value());
    test.send(run_command_intent { "view.changes" });
    CHECK(test.view().start.has_value());
}

TEST_CASE("the palette opens from its button intent and lists what the repository can run", "[palette][controller]")
{
    p6_harness test {};
    CHECK_FALSE(test.view().palette.has_value());
    test.send(open_palette_intent {});
    REQUIRE(test.controller.state().palette.has_value());
    const view_snapshot view { test.view() };
    REQUIRE(view.palette.has_value());
    CHECK(view.palette->mode == palette_mode::commands);
    CHECK(view.palette->title == u8"명령 팔레트");
    CHECK(test.palette_has(u8"cmd:app.settings"));
    // 보이는 항목은 상한까지이고 나머지는 "외 N개"로 말한다. 거르면 나머지가 나온다.
    REQUIRE(view.palette->items.size() == palette_visible_limit + 1);
    CHECK(view.palette->items.back().key == palette_more_key);
    CHECK_FALSE(view.palette->items.back().enabled);
    test.type(text_target::palette_filter, u8"fetch");
    CHECK(test.palette_has(u8"cmd:git.fetch"));
    // 커밋 제목이 비어 있으면 커밋은 서지 않는다.
    test.type(text_target::palette_filter, u8"커밋하기");
    CHECK_FALSE(test.palette_has(u8"cmd:git.commit"));
    test.type(text_target::commit_subject, u8"작업 내용");
    CHECK(test.palette_has(u8"cmd:git.commit"));
    // 커서는 첫 활성 항목이다.
    const view_snapshot filtered { test.view() };
    REQUIRE_FALSE(filtered.palette->items.empty());
    CHECK(filtered.palette->cursor == filtered.palette->items.front().key);
    test.send(close_palette_intent {});
    CHECK_FALSE(test.controller.state().palette.has_value());
}

TEST_CASE("a repository without remotes cannot push from either toolbar or commands", "[commands][controller]")
{
    p6_harness test {};
    repository_session& session { *test.workspace.find_session(test.repository) };
    repository_snapshot snapshot { snap::make_snapshot(session.snapshot().summary.head_sha, 0) };
    snapshot.summary.remotes.clear();
    snapshot.summary.has_upstream = false;
    std::vector<commit_info> commits { session.graph_commits() };
    if (session.has_wip_row())
        commits.erase(commits.begin());
    const std::uint64_t generation { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { generation, std::move(snapshot) }));
    REQUIRE(session.apply(commits_loaded { .generation = generation, .commits = std::move(commits), .page_complete = true, .history_complete = true }));
    test.controller.after_events();
    CHECK_FALSE(test.view().toolbar.push_enabled);
    test.send(open_palette_intent {});
    test.type(text_target::palette_filter, u8"push");
    CHECK_FALSE(test.palette_has(u8"cmd:git.push"));
    const std::size_t before { test.dispatcher.pending() };
    test.send(run_command_intent { "git.push" });
    CHECK(test.dispatcher.pending() == before);
    CHECK_FALSE(test.controller.state().dialog.has_value());
}

TEST_CASE("typing initial consonants finds a command and picking it runs it and remembers it", "[palette][controller]")
{
    p6_harness test {};
    test.send(open_palette_intent {});
    test.type(text_target::palette_filter, u8"ㄱㅈㅇㄱ");
    const view_snapshot view { test.view() };
    REQUIRE(view.palette.has_value());
    REQUIRE_FALSE(view.palette->items.empty());
    CHECK(view.palette->items.front().key == u8"cmd:git.fetch");
    CHECK(view.palette->cursor == u8"cmd:git.fetch");

    const std::size_t before { test.dispatcher.pending() };
    test.send(palette_pick_intent { u8"cmd:git.fetch" });
    CHECK_FALSE(test.controller.state().palette.has_value());
    CHECK(test.dispatcher.pending() == before + 1);
    CHECK(test.last_request<fetch_request>() != nullptr);
    REQUIRE_FALSE(test.controller.settings().command_history.empty());
    CHECK(test.controller.settings().command_history.front() == "git.fetch");
}

TEST_CASE("a command that cannot run is ignored even if its key is sent", "[palette][controller]")
{
    p6_harness test {};
    test.send(open_palette_intent {});
    const std::size_t before { test.dispatcher.pending() };
    test.send(palette_pick_intent { u8"cmd:git.stash" });
    test.send(palette_pick_intent { u8"cmd:no.such.command" });
    test.send(palette_pick_intent { u8"unknown-key" });
    CHECK(test.dispatcher.pending() == before);
}

TEST_CASE("arrow keys move the cursor over enabled items and stop at the ends", "[palette][controller]")
{
    p6_harness test {};
    test.send(open_palette_intent {});
    const view_snapshot first { test.view() };
    REQUIRE(first.palette->items.size() > 3);
    const std::u8string top { first.palette->items[0].key };
    test.send(palette_move_intent { 1 });
    CHECK(test.view().palette->cursor == first.palette->items[1].key);
    test.send(palette_move_intent { -1 });
    test.send(palette_move_intent { -1 });
    CHECK(test.view().palette->cursor == top);
    for (int step { 0 }; step < 100; ++step)
        test.send(palette_move_intent { 1 });
    const view_snapshot last { test.view() };
    // 마지막 항목은 "외 N개" 안내 줄이라 꺼져 있다. 커서는 그 앞의 마지막 활성 항목에서 멈춘다.
    const auto last_enabled { std::ranges::find_if(last.palette->items.rbegin(), last.palette->items.rend(), [](const palette_item_view& item) { return item.enabled; }) };
    CHECK(last.palette->cursor == last_enabled->key);
    // 거르는 글이 바뀌면 커서는 맨 위로 돌아간다.
    test.type(text_target::palette_filter, u8"fetch");
    CHECK(test.view().palette->cursor == test.view().palette->items.front().key);
}

TEST_CASE("short palettes scroll, reveal keyboard cursors and reset on filtering", "[palette][ui]")
{
    p6_harness test {};
    test.send(open_palette_intent {});
    const auto build = [&test] { return ui::build_ui_tree(test.controller.make_view(480.0f, 360.0f, 1.0f)); };
    const ui::built_tree first { build() };
    REQUIRE(first.feedback.palette_maximum.has_value());
    CHECK(*first.feedback.palette_maximum > 0.0f);
    CHECK(first.feedback.palette_scroll == 0.0f);
    const luil::ui_element* const help { first.tree->find({ ui::kind_label, u8"palette-help" }) };
    REQUIRE(help != nullptr);
    CHECK(help->bounds().y + help->bounds().height < 360.0f);
    test.controller.apply_feedback(first.feedback);

    test.send(palette_move_intent { 1000 });
    const ui::built_tree last { build() };
    REQUIRE(last.feedback.palette_scroll.has_value());
    CHECK(*last.feedback.palette_scroll > 0.0f);
    test.controller.apply_feedback(last.feedback);
    const auto cursor { test.controller.state().palette->cursor };
    test.send(palette_scroll_intent { .value = 0.0f, .absolute = true });
    CHECK(build().feedback.palette_scroll == 0.0f);
    CHECK(test.controller.state().palette->cursor == cursor);
    test.send(palette_scroll_intent { .value = 100000.0f, .absolute = true });
    CHECK(build().feedback.palette_scroll == last.feedback.palette_maximum);

    test.type(text_target::palette_filter, u8"환경 설정");
    CHECK(build().feedback.palette_scroll == 0.0f);
    CHECK(build().tree->duplicate_ids().empty());
    CHECK(build().tree->unarranged().empty());
}

TEST_CASE("branch mode checks out the picked branch through the normal checkout", "[palette][controller]")
{
    p6_harness test {};
    test.send(run_command_intent { "git.branch.checkout" });
    REQUIRE(test.controller.state().palette.has_value());
    CHECK(test.controller.state().palette->mode == palette_mode::branches);
    CHECK(test.palette_has(u8"branch:feature/graph"));
    // 현재 브랜치는 서 있지만 꺼져 있다.
    const view_snapshot view { test.view() };
    const auto current { std::ranges::find(view.palette->items, std::u8string { u8"branch:main" }, &palette_item_view::key) };
    REQUIRE(current != view.palette->items.end());
    CHECK_FALSE(current->enabled);

    test.type(text_target::palette_filter, u8"fg");
    CHECK(test.view().palette->items.front().key == u8"branch:feature/graph");
    test.send(palette_pick_intent { u8"branch:feature/graph" });
    CHECK_FALSE(test.controller.state().palette.has_value());
    const checkout_branch_request* const checkout { test.last_request<checkout_branch_request>() };
    REQUIRE(checkout != nullptr);
    CHECK(checkout->name == "feature/graph");
}

TEST_CASE("repository mode lists the open tabs and recent repositories", "[palette][controller]")
{
    p6_harness test {};
    test.send(new_tab_intent {});
    test.controller.open_repository(u8"E:\\work\\other");
    REQUIRE(test.controller.tabs().tabs().size() == 2);
    test.send(run_command_intent { "repo.open" });
    CHECK(test.controller.state().palette->mode == palette_mode::repositories);
    CHECK(test.palette_has(u8"cmd:repo.open_folder"));
    const view_snapshot view { test.view() };
    const std::u8string tab_key = [&view] {
        for (const palette_item_view& item : view.palette->items)
            if (item.key.starts_with(u8"tab:") && item.enabled)
                return item.key;
        return std::u8string {};
    }();
    REQUIRE_FALSE(tab_key.empty());
    test.send(palette_pick_intent { tab_key });
    CHECK_FALSE(test.controller.state().palette.has_value());
    CHECK(test.controller.tabs().selected().path == u8"E:\\work\\repo");
}

TEST_CASE("the selected commit's menu entries are commands and picking one runs the menu action", "[palette][controller]")
{
    p6_harness test {};
    const std::string sha { snap::sha_of(2) };
    test.send(select_commit_intent { graph_item_key(sha) });
    test.send(open_palette_intent {});
    // 고른 커밋의 메뉴 항목은 명령 뒤에 서므로 기본 목록의 보이는 수 밖이다. 거르면 나온다.
    test.type(text_target::palette_filter, u8"SHA");
    CHECK(test.palette_has(u8"menu:copy_sha"));
    test.type(text_target::palette_filter, u8"SHA 복사");
    CHECK(test.view().palette->items.front().key == u8"menu:copy_sha");
    test.send(palette_pick_intent { u8"menu:copy_sha" });
    CHECK_FALSE(test.controller.state().palette.has_value());
    const std::optional<std::u8string> copied { test.controller.take_clipboard_text() };
    REQUIRE(copied.has_value());
    CHECK(*copied == std::u8string(sha.begin(), sha.end()));
}

TEST_CASE("registry commands run the same actions as the buttons", "[commands][controller]")
{
    p6_harness test {};
    test.send(run_command_intent { "tab.new" });
    CHECK(test.controller.tabs().tabs().size() == 2);
    test.send(run_command_intent { "tab.previous" });
    CHECK(test.controller.tabs().selected().path == u8"E:\\work\\repo");
    test.send(run_command_intent { "app.settings.shortcuts" });
    REQUIRE(test.controller.settings_screen().is_open());
    CHECK(test.controller.settings_screen().state()->page == settings_page::shortcuts);
    test.send(close_settings_intent {});

    // palette.open는 여닫는다.
    test.send(run_command_intent { "palette.open" });
    CHECK(test.controller.state().palette.has_value());
    test.send(run_command_intent { "palette.open" });
    CHECK_FALSE(test.controller.state().palette.has_value());

    // 열린 팔레트는 모드를 여는 명령이 아니면 닫힌다.
    test.send(run_command_intent { "palette.open" });
    test.send(run_command_intent { "app.log" });
    CHECK_FALSE(test.controller.state().palette.has_value());
    CHECK(test.controller.state().operation_log_open);

    const std::size_t before { test.dispatcher.pending() };
    test.send(run_command_intent { "repo.refresh" });
    CHECK(test.dispatcher.pending() > before);
    // 폴더 대화 상자 명령은 UI thread의 일이라 controller는 아무것도 하지 않는다.
    test.send(run_command_intent { "repo.open_folder" });
}

TEST_CASE("the commit command runs only when there is a subject", "[commands][controller]")
{
    p6_harness test {};
    const std::size_t before { test.dispatcher.pending() };
    test.send(run_command_intent { "git.commit" });
    CHECK(test.dispatcher.pending() == before);
    test.type(text_target::commit_subject, u8"작업 내용");
    test.send(run_command_intent { "git.commit" });
    CHECK(test.dispatcher.pending() == before + 1);
    CHECK(test.last_request<commit_request>() != nullptr);
}

// ------------------------------------------------------------------ 키 처리

TEST_CASE("keys dispatch through the registry table carried by the tree", "[commands][policy]")
{
    p6_harness test {};
    const view_snapshot view { test.view() };
    const std::shared_ptr<const luil::ui_tree> tree { tree_of(view) };
    ui::cyella_policy policy {};
    luil::interaction_snapshot snapshot {};

    const auto command_of = [&](const luil::key_pressed_event& event, const luil::interaction_snapshot& state) {
        const std::vector<luil::input_action> actions { policy.on_key(tree.get(), event, state) };
        const run_command_intent* const run { intent_of<run_command_intent>(actions) };
        return run == nullptr ? std::string {} : run->id;
    };
    CHECK(command_of(key_event(luil::key_code::key_p, true, true), snapshot) == "palette.open");
    CHECK(command_of(key_event(luil::key_code::key_p, true), snapshot) == "palette.open");
    CHECK(command_of(key_event(luil::key_code::key_w, true), snapshot) == "tab.close");
    CHECK(command_of(key_event(luil::key_code::tab, true), snapshot) == "tab.next");
    CHECK(command_of(key_event(luil::key_code::tab, true, true), snapshot) == "tab.previous");
    CHECK(command_of(key_event(luil::key_code::f10, false, true), snapshot) == "view.menu");
    CHECK(command_of(key_event(luil::key_code::f5), snapshot) == "repo.refresh");
    CHECK(command_of(key_event(luil::key_code::escape), snapshot) == "view.close");
    // Ctrl+Enter는 커밋 상자에 초점이 있을 때만 커밋이다.
    CHECK(command_of(key_event(luil::key_code::enter, true), snapshot).empty());
    luil::interaction_snapshot in_subject {};
    in_subject.focused_input = luil::ui_element_id { ui::kind_commit_subject_input, u8"commit-subject" };
    CHECK(command_of(key_event(luil::key_code::enter, true), in_subject) == "git.commit");
    in_subject.focused_input = luil::ui_element_id { ui::kind_commit_body_input, u8"commit-body" };
    CHECK(command_of(key_event(luil::key_code::enter, true), in_subject) == "git.commit");
    // 자동 반복은 한 번만 반응한다.
    luil::key_pressed_event repeated { key_event(luil::key_code::f5) };
    repeated.repeat = true;
    CHECK(command_of(repeated, snapshot).empty());
    // 레지스트리에 없는 키는 정책이 건드리지 않는다.
    CHECK(policy.on_key(tree.get(), key_event(luil::key_code::key_k, true), snapshot).empty());
}

TEST_CASE("a shortcut changed in settings takes effect on the next frame and frees the old key", "[commands][policy]")
{
    p6_harness test {};
    test.send(open_settings_intent { settings_page::shortcuts });
    test.send(edit_shortcut_intent { "tab.close" });
    REQUIRE(test.controller.settings_screen().state()->shortcut_edit.has_value());
    test.type(text_target::settings_shortcut_input, u8"Ctrl+Alt+Q");
    test.send(apply_shortcut_intent {});
    CHECK_FALSE(test.controller.settings_screen().state()->shortcut_edit.has_value());
    CHECK(test.controller.settings().shortcuts.at("tab.close") == std::vector<std::u8string> { u8"Ctrl+Alt+Q" });

    const view_snapshot view { test.view() };
    const std::shared_ptr<const luil::ui_tree> tree { tree_of(view) };
    ui::cyella_policy policy {};
    luil::interaction_snapshot snapshot {};
    const std::vector<luil::input_action> moved { policy.on_key(tree.get(), key_event(luil::key_code::key_q, true, false, true), snapshot) };
    const run_command_intent* const run { intent_of<run_command_intent>(moved) };
    REQUIRE(run != nullptr);
    CHECK(run->id == "tab.close");
    CHECK(policy.on_key(tree.get(), key_event(luil::key_code::key_w, true), snapshot).empty());

    // 기본값으로 되돌리면 Ctrl+W가 돌아온다.
    test.send(reset_shortcut_intent { "tab.close" });
    CHECK(test.controller.settings().shortcuts.empty());
    const std::shared_ptr<const luil::ui_tree> restored { tree_of(test.view()) };
    CHECK(intent_of<run_command_intent>(policy.on_key(restored.get(), key_event(luil::key_code::key_w, true), snapshot)) != nullptr);
}

TEST_CASE("the palette filter takes arrows and Enter from the tree's marker", "[palette][policy]")
{
    p6_harness test {};
    test.send(open_palette_intent {});
    test.type(text_target::palette_filter, u8"ㄱㅈㅇㄱ");
    const view_snapshot view { test.view() };
    const std::shared_ptr<const luil::ui_tree> tree { tree_of(view) };
    ui::cyella_policy policy {};
    luil::interaction_snapshot snapshot {};
    snapshot.focused_input = luil::ui_element_id { ui::kind_palette_filter_input, u8"palette-filter" };

    const std::vector<luil::input_action> down { policy.on_key(tree.get(), key_event(luil::key_code::arrow_down), snapshot) };
    const palette_move_intent* const move { intent_of<palette_move_intent>(down) };
    REQUIRE(move != nullptr);
    CHECK(move->delta == 1);
    const std::vector<luil::input_action> up { policy.on_key(tree.get(), key_event(luil::key_code::arrow_up), snapshot) };
    REQUIRE(intent_of<palette_move_intent>(up) != nullptr);
    CHECK(intent_of<palette_move_intent>(up)->delta == -1);

    // Enter는 커서 항목(여기서는 fetch)을 고르는 액션이다.
    const std::vector<luil::input_action> enter { policy.on_key(tree.get(), key_event(luil::key_code::enter), snapshot) };
    const palette_pick_intent* const pick { intent_of<palette_pick_intent>(enter) };
    REQUIRE(pick != nullptr);
    CHECK(pick->key == u8"cmd:git.fetch");
    // 다른 칸에서는 이 키들이 정책의 몫이 아니다.
    luil::interaction_snapshot elsewhere {};
    CHECK(policy.on_key(tree.get(), key_event(luil::key_code::arrow_down), elsewhere).empty());
}

TEST_CASE("the palette tree has its filter, list rows and the palette button", "[palette][ui]")
{
    p6_harness test {};
    test.send(open_palette_intent {});
    const view_snapshot view { test.view() };
    const std::shared_ptr<const luil::ui_tree> tree { tree_of(view) };
    CHECK(tree->find(luil::ui_element_id { ui::kind_palette_filter_input, u8"palette-filter" }) != nullptr);
    CHECK(tree->find(luil::ui_element_id { luil::ui_element_kind::virtual_list, u8"palette-list" }) != nullptr);
    // 단추는 팔레트가 열려도 tree에 남는다 (UIA가 이름 'command-palette'로 찾는다).
    CHECK(tree->find(luil::ui_element_id { ui::kind_button, u8"command-palette" }) != nullptr);
}

// ------------------------------------------------------------------ 환경 설정 화면

TEST_CASE("editing a shortcut rejects bad and conflicting keys and applies a good one", "[shortcuts][controller]")
{
    p6_harness test {};
    test.send(open_settings_intent { settings_page::shortcuts });
    test.send(edit_shortcut_intent { "git.fetch" });
    auto page = [&test] { return test.view().settings->shortcuts; };

    test.type(text_target::settings_shortcut_input, u8"Ctrl+W");
    REQUIRE(page().has_value());
    REQUIRE(page()->edit.has_value());
    CHECK_FALSE(page()->edit->can_apply);
    CHECK(page()->edit->error.find(u8"현재 탭 닫기") != std::u8string::npos);
    test.send(apply_shortcut_intent {});
    CHECK(test.controller.settings().shortcuts.empty());

    test.type(text_target::settings_shortcut_input, u8"K");
    CHECK_FALSE(page()->edit->can_apply);
    test.type(text_target::settings_shortcut_input, u8"Ctrl+Alt+K");
    CHECK(page()->edit->can_apply);
    test.send(apply_shortcut_intent {});
    CHECK(test.controller.settings().shortcuts.at("git.fetch") == std::vector<std::u8string> { u8"Ctrl+Alt+K" });
    CHECK(test.view().settings->message.find(u8"바꿨습니다") != std::u8string::npos);
    bool customized { false };
    const std::optional<shortcuts_page_view> current_page { page() };
    for (const shortcut_group_view& group : current_page->groups)
        for (const shortcut_row_view& row : group.rows)
            if (row.command == "git.fetch")
                customized = row.customized && row.keys == u8"Ctrl+Alt+K";
    CHECK(customized);
    CHECK(page()->any_customized);

    // 비우고 적용하면 키 없음이다.
    test.send(edit_shortcut_intent { "tab.close" });
    test.type(text_target::settings_shortcut_input, u8"");
    test.send(apply_shortcut_intent {});
    CHECK(test.controller.settings().shortcuts.at("tab.close").empty());
    test.send(reset_all_shortcuts_intent {});
    CHECK(test.controller.settings().shortcuts.empty());
}

TEST_CASE("a pressed key is captured into the shortcut input", "[shortcuts][policy]")
{
    p6_harness test {};
    test.send(open_settings_intent { settings_page::shortcuts });
    test.send(edit_shortcut_intent { "git.fetch" });
    ui::cyella_policy policy {};
    luil::interaction_snapshot snapshot {};
    snapshot.focused_input = luil::ui_element_id { ui::kind_shortcut_input, u8"settings-shortcut-input" };
    const std::shared_ptr<const luil::ui_tree> tree { tree_of(test.view()) };

    // Ctrl+W는 탭을 닫지 않고 칸에 적힌다.
    const std::vector<luil::input_action> captured { policy.on_key(tree.get(), key_event(luil::key_code::key_w, true), snapshot) };
    const capture_shortcut_intent* const capture { intent_of<capture_shortcut_intent>(captured) };
    REQUIRE(capture != nullptr);
    test.send(*capture);
    CHECK(test.controller.settings_screen().state()->shortcut_edit->text.text == u8"Ctrl+W");
    CHECK_FALSE(test.view().settings->shortcuts->edit->can_apply);

    const std::vector<luil::input_action> function_key { policy.on_key(tree.get(), key_event(luil::key_code::f7), snapshot) };
    REQUIRE(intent_of<capture_shortcut_intent>(function_key) != nullptr);
    test.send(*intent_of<capture_shortcut_intent>(function_key));
    CHECK(test.view().settings->shortcuts->edit->can_apply);
    // 수정키 없는 Enter는 적용이다.
    CHECK(intent_of<apply_shortcut_intent>(policy.on_key(tree.get(), key_event(luil::key_code::enter), snapshot)) != nullptr);
    test.send(apply_shortcut_intent {});
    CHECK(test.controller.settings().shortcuts.at("git.fetch") == std::vector<std::u8string> { u8"F7" });
}

TEST_CASE("the shortcut and profile pages build with unique ids", "[settings][ui]")
{
    p6_harness test {};
    test.add_profile(u8"회사", u8"홍길동", u8"hong@corp.example");
    test.send(edit_shortcut_intent { "git.fetch" });
    for (const settings_page page : { settings_page::shortcuts, settings_page::profiles })
    {
        test.send(select_settings_page_intent { page });
        const view_snapshot view { test.view() };
        REQUIRE(view.settings.has_value());
        const ui::built_tree built { ui::build_ui_tree(view) };
        REQUIRE(built.tree != nullptr);
        CHECK(built.tree->find(luil::ui_element_id { luil::ui_element_kind::list_row, u8"settings-page-profiles" }) != nullptr);
        CHECK(built.tree->find(luil::ui_element_id { luil::ui_element_kind::list_row, u8"settings-page-shortcuts" }) != nullptr);
    }
}

// ------------------------------------------------------------------ 저장소별 사용자 프로필

TEST_CASE("profiles are added, edited and removed through the settings page", "[profiles][controller]")
{
    p6_harness test {};
    test.send(open_settings_intent { settings_page::profiles });
    test.send(add_profile_intent {});
    test.type(text_target::settings_profile_name, u8"회사");
    test.type(text_target::settings_profile_user_name, u8"홍길동");
    // 이메일이 틀리면 저장할 수 없다.
    test.type(text_target::settings_profile_user_email, u8"no-at-sign");
    REQUIRE(test.view().settings->profiles->edit.has_value());
    CHECK_FALSE(test.view().settings->profiles->edit->can_save);
    test.send(save_profile_intent {});
    CHECK(test.controller.settings().profiles.empty());
    test.type(text_target::settings_profile_user_email, u8"hong@corp.example");
    test.type(text_target::settings_profile_signing_key, u8"ABCD1234");
    test.send(save_profile_intent {});
    REQUIRE(test.controller.settings().profiles.size() == 1);
    CHECK(test.controller.settings().profiles[0].signing_key == u8"ABCD1234");
    CHECK_FALSE(test.controller.settings_screen().state()->profile_edit.has_value());

    test.send(edit_profile_intent { 0 });
    test.type(text_target::settings_profile_user_name, u8"길동");
    test.send(save_profile_intent {});
    CHECK(test.controller.settings().profiles[0].user_name == u8"길동");

    test.add_rule(u8"E:\\work", u8"회사");
    REQUIRE(test.controller.settings().profile_rules.size() == 1);
    CHECK(test.controller.settings().profile_rules[0].profile == u8"회사");
    test.send(remove_profile_rule_intent { 0 });
    CHECK(test.controller.settings().profile_rules.empty());
    test.send(remove_profile_intent { 0 });
    CHECK(test.controller.settings().profiles.empty());
}

TEST_CASE("the selected repository's identity is requested once and shown with its source", "[profiles][controller]")
{
    p6_harness test {};
    REQUIRE(test.reads.size() == 1);
    CHECK(test.reads[0].repository_id == test.repository);
    CHECK(test.reads[0].path == u8"E:\\work\\repo");
    // 결과가 오기 전에 또 청하지 않는다.
    test.send(open_palette_intent {});
    test.send(close_palette_intent {});
    CHECK(test.reads.size() == 1);

    view_snapshot loading { test.view() };
    CHECK(loading.status.identity_visible);
    CHECK(loading.status.identity_text == u8"작성자: 읽는 중…");

    test.identity_is(u8"Global Gal", u8"g@example.com");
    const view_snapshot view { test.view() };
    CHECK(view.status.identity_text == u8"작성자: Global Gal <g@example.com> · 전역");
    CHECK_FALSE(view.status.identity_warning);

    // 이름과 이메일이 다른 파일에서 왔으면 각각 말한다.
    git_identity mixed {};
    mixed.name = identity_value { u8"Global Gal", identity_origin::global, u8"g" };
    mixed.email = identity_value { u8"work@corp.example", identity_origin::include, u8"w" };
    test.send(identity_loaded_intent { .repository_id = test.repository, .identity = mixed, .ok = true, .error = {} });
    CHECK(test.view().status.identity_text == u8"작성자: Global Gal <work@corp.example> · 이름 전역 · 이메일 includeIf");

    // 이메일이 없으면 경고 표시다.
    git_identity partial {};
    partial.name = identity_value { u8"Solo", identity_origin::local, u8".git/config" };
    test.send(identity_loaded_intent { .repository_id = test.repository, .identity = partial, .ok = true, .error = {} });
    CHECK(test.view().status.identity_warning);
}

TEST_CASE("a failed identity read says so instead of staying on reading", "[profiles][controller]")
{
    p6_harness test {};
    test.send(identity_loaded_intent { .repository_id = test.repository, .identity = {}, .ok = false, .error = u8"unknown option --show-scope" });
    const view_snapshot view { test.view() };
    CHECK(view.status.identity_text == u8"작성자: 읽지 못함 · unknown option --show-scope");
    CHECK(view.status.identity_warning);
    // 읽지 못한 신원으로는 커밋을 막지 않는다.
    test.type(text_target::commit_subject, u8"작업");
    const std::size_t before { test.dispatcher.pending() };
    test.send(commit_intent {});
    CHECK(test.dispatcher.pending() == before + 1);
}

TEST_CASE("without a worker for identities nothing is read and the status stays empty", "[profiles][controller]")
{
    p6_harness test { false };
    CHECK(test.reads.empty());
    CHECK_FALSE(test.view().status.identity_visible);
}

TEST_CASE("a matching rule only proposes a profile; nothing is written until it is applied", "[profiles][controller]")
{
    p6_harness test {};
    test.add_profile(u8"회사", u8"홍길동", u8"hong@corp.example");
    test.add_rule(u8"E:\\work", u8"회사");
    test.send(close_settings_intent {});
    test.identity_is(u8"Global Gal", u8"g@example.com");

    REQUIRE(test.controller.state().notice.has_value());
    const notice_state& notice { *test.controller.state().notice };
    CHECK(notice.text.find(u8"회사") != std::u8string::npos);
    CHECK(notice.text.find(u8"전역") != std::u8string::npos);
    REQUIRE(notice.actions.size() == 1);
    CHECK(notice.actions[0].key == u8"apply_profile:회사");
    // 제안만 했다. 어떤 설정도 쓰지 않았다.
    CHECK(test.writes.empty());
    CHECK(test.controller.settings().profile_assignments.empty());
    const view_snapshot view { test.view() };
    REQUIRE(view.banner.has_value());
    REQUIRE(view.banner->actions.size() == 1);
    CHECK(view.banner->actions[0].key == u8"apply_profile:회사");

    // 같은 저장소에는 한 번만 제안한다.
    test.send(dismiss_notice_intent {});
    test.identity_is(u8"Global Gal", u8"g@example.com");
    CHECK_FALSE(test.controller.state().notice.has_value());

    // [적용]을 누르면 로컬 설정에 쓰라고 worker에게 시킨다.
    test.send(banner_action_intent { u8"apply_profile:회사", 0 });
    REQUIRE(test.writes.size() == 1);
    CHECK(test.writes[0].repository_id == test.repository);
    CHECK(test.writes[0].path == u8"E:\\work\\repo");
    CHECK(test.writes[0].user_name == u8"홍길동");
    CHECK(test.writes[0].user_email == u8"hong@corp.example");
    // worker가 성공을 알리면 적용 기록이 남고 신원을 다시 읽는다.
    const std::size_t reads_before { test.reads.size() };
    test.send(identity_applied_intent { .repository_id = test.repository, .ok = true, .message = u8"설정했습니다." });
    REQUIRE(test.controller.settings().profile_assignments.size() == 1);
    CHECK(test.controller.settings().profile_assignments[0].profile == u8"회사");
    CHECK(test.reads.size() == reads_before + 1);
    CHECK(test.controller.state().notice->text.find(u8"적용했습니다") != std::u8string::npos);
}

TEST_CASE("no proposal when the identity already matches the rule's profile or a chosen profile", "[profiles][controller]")
{
    {
        p6_harness test {};
        test.add_profile(u8"회사", u8"홍길동", u8"hong@corp.example");
        test.add_rule(u8"E:\\work", u8"회사");
        test.send(close_settings_intent {});
        test.identity_is(u8"홍길동", u8"hong@corp.example", identity_origin::local);
        CHECK_FALSE(test.controller.state().notice.has_value());
    }
    {
        // 규칙은 회사를 권하지만 사용자가 개인 프로필을 골라 두었고 신원이 그대로 개인이다.
        p6_harness test {};
        test.add_profile(u8"회사", u8"홍길동", u8"hong@corp.example");
        test.add_profile(u8"개인", u8"길동", u8"me@example.com");
        test.add_rule(u8"E:\\work", u8"회사");
        test.send(close_settings_intent {});
        test.send(apply_profile_intent { u8"개인" });
        test.send(identity_applied_intent { .repository_id = test.repository, .ok = true, .message = {} });
        test.send(dismiss_notice_intent {});
        test.identity_is(u8"길동", u8"me@example.com", identity_origin::local);
        CHECK_FALSE(test.controller.state().notice.has_value());
    }
}

TEST_CASE("the first commit warns when the identity is missing or differs from the rule, once per repository", "[profiles][controller]")
{
    p6_harness test {};
    test.add_profile(u8"회사", u8"홍길동", u8"hong@corp.example");
    test.add_rule(u8"E:\\work", u8"회사");
    test.send(close_settings_intent {});
    test.identity_is(u8"Global Gal", u8"g@example.com");
    test.send(dismiss_notice_intent {});
    test.type(text_target::commit_subject, u8"작업 내용");
    const std::size_t before { test.dispatcher.pending() };

    test.send(commit_intent {});
    // 커밋하지 않고 경고했다.
    CHECK(test.dispatcher.pending() == before);
    REQUIRE(test.controller.state().notice.has_value());
    CHECK(test.controller.state().notice->warning);
    REQUIRE(test.controller.state().notice->actions.size() == 2);
    CHECK(test.controller.state().notice->actions[0].key == u8"commit_apply:회사");
    CHECK(test.controller.state().notice->actions[1].key == u8"commit_anyway");

    // 두 번째 시도는 경고 없이 커밋한다.
    test.send(commit_intent {});
    CHECK(test.dispatcher.pending() == before + 1);
    CHECK(test.last_request<commit_request>() != nullptr);
}

TEST_CASE("the warning's apply-and-commit applies the profile first and commits when the worker says it worked", "[profiles][controller]")
{
    p6_harness test {};
    test.add_profile(u8"회사", u8"홍길동", u8"hong@corp.example");
    test.add_rule(u8"E:\\work", u8"회사");
    test.send(close_settings_intent {});
    test.identity_is(u8"Global Gal", u8"g@example.com");
    test.send(dismiss_notice_intent {});
    test.type(text_target::commit_subject, u8"작업 내용");
    test.send(commit_intent {});
    const std::size_t before { test.dispatcher.pending() };

    test.send(banner_action_intent { u8"commit_apply:회사", 0 });
    REQUIRE(test.writes.size() == 1);
    // 적용이 끝나기 전에는 커밋하지 않는다.
    CHECK(test.dispatcher.pending() == before);
    test.send(identity_applied_intent { .repository_id = test.repository, .ok = true, .message = {} });
    CHECK(test.dispatcher.pending() == before + 1);
    CHECK(test.last_request<commit_request>() != nullptr);
}

TEST_CASE("a failed apply neither records the profile nor commits", "[profiles][controller]")
{
    p6_harness test {};
    test.add_profile(u8"회사", u8"홍길동", u8"hong@corp.example");
    test.send(close_settings_intent {});
    test.identity_is(u8"Global Gal", u8"g@example.com");
    test.type(text_target::commit_subject, u8"작업 내용");
    test.send(apply_profile_intent { u8"회사" });
    test.send(identity_applied_intent { .repository_id = test.repository, .ok = false, .message = u8"lock 파일이 있습니다" });
    CHECK(test.controller.settings().profile_assignments.empty());
    REQUIRE(test.controller.state().notice.has_value());
    CHECK(test.controller.state().notice->warning);
    CHECK(test.controller.state().notice->text.find(u8"lock 파일") != std::u8string::npos);
}

TEST_CASE("commit anyway skips the warning and commits", "[profiles][controller]")
{
    p6_harness test {};
    test.identity_is(u8"Solo", u8"");
    test.type(text_target::commit_subject, u8"작업 내용");
    const std::size_t before { test.dispatcher.pending() };
    test.send(commit_intent {});
    CHECK(test.dispatcher.pending() == before);
    REQUIRE(test.controller.state().notice.has_value());
    CHECK(test.controller.state().notice->text.find(u8"작성자 설정이 없어") != std::u8string::npos);
    test.send(banner_action_intent { u8"commit_anyway", 0 });
    CHECK(test.dispatcher.pending() == before + 1);
    CHECK_FALSE(test.controller.state().notice.has_value());
}

TEST_CASE("profile mode of the palette applies the picked profile", "[profiles][palette][controller]")
{
    p6_harness test {};
    test.add_profile(u8"회사", u8"홍길동", u8"hong@corp.example");
    test.send(close_settings_intent {});
    test.identity_is(u8"Global Gal", u8"g@example.com");
    test.send(run_command_intent { "profile.apply" });
    REQUIRE(test.controller.state().palette.has_value());
    CHECK(test.controller.state().palette->mode == palette_mode::profiles);
    CHECK(test.palette_has(u8"profile:회사"));
    CHECK(test.palette_has(palette_manage_profiles_key));
    test.send(palette_pick_intent { u8"profile:회사" });
    CHECK_FALSE(test.controller.state().palette.has_value());
    REQUIRE(test.writes.size() == 1);
    CHECK(test.writes[0].user_email == u8"hong@corp.example");
    // 프로필 관리는 설정 화면의 해당 쪽을 연다.
    test.send(run_command_intent { "profile.apply" });
    test.send(palette_pick_intent { std::u8string { palette_manage_profiles_key } });
    REQUIRE(test.controller.settings_screen().is_open());
    CHECK(test.controller.settings_screen().state()->page == settings_page::profiles);
}

TEST_CASE("applying with no repository or no worker says why", "[profiles][controller]")
{
    {
        p6_harness test { false };
        test.add_profile(u8"회사", u8"홍길동", u8"hong@corp.example");
        test.send(apply_profile_intent { u8"회사" });
        REQUIRE(test.controller.state().notice.has_value());
        CHECK(test.controller.state().notice->warning);
    }
    {
        p6_harness test {};
        test.send(apply_profile_intent { u8"없는 프로필" });
        REQUIRE(test.controller.state().notice.has_value());
        CHECK(test.controller.state().notice->warning);
        CHECK(test.writes.empty());
    }
}
