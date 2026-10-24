// 인터랙티브 rebase 편집기를 셸과 luil tree에서 본다. UI 자동화로 닿지 않는 몸짓(라벨 끌어 놓기, 우클릭 메뉴, 할 일 줄 끌기,
// p·r·s·f·d 글자, Alt+↑↓)은 실제 입력 대신 그 몸짓이 내는 intent·element 콜백·정책을 직접 불러 확인한다.
#include "application/git_workspace.h"
#include "infrastructure/git_operation_runner.h"
#include "presentation/build_view_snapshot.h"
#include "presentation/rebase/rebase_editor_model.h"
#include "presentation/shell_controller.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/cyella_policy.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/rebase/rebase_editor_ui.h"

#include "fake_git_runner.h"
#include "view_snapshot_support.h"

#include "luil/ui/ui_tree.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

namespace {
    using namespace cyella;
    namespace snap = cyella::test::snapshot;

    struct shell_harness
    {
        shell_harness()
            : runner { process, u8"C:/Git/cmd/git.exe", files, {} }
            , workspace { runner, dispatcher.dispatcher(), events.sink(), nullptr, {} }
            , controller { make_dependencies(workspace) }
        {
            controller.start({});
            controller.open_repository(u8"C:\\work\\repo");
            repository = controller.selected_repository_id();
            repository_session* const session { workspace.find_session(repository) };
            REQUIRE(session != nullptr);
            snap::load(*session, snap::linear_history(5), 0);
        }

        static shell_controller::dependencies make_dependencies(git_workspace& workspace)
        {
            shell_controller::dependencies dependencies {};
            dependencies.workspace = &workspace;
            dependencies.clock = [] { return std::chrono::system_clock::time_point { std::chrono::seconds { 1700000000 + 3600 } }; };
            return dependencies;
        }

        template<typename intent_type>
        void send(intent_type intent)
        {
            controller.handle(luil::app_message { std::move(intent) });
            controller.after_events();
        }

        // 편집기가 낸 범위 읽기 요청이다.
        [[nodiscard]] const load_rebase_plan_request* plan_request() const
        {
            const std::optional<rebase_editor_state>& state { controller.rebase_editor().state() };
            if (state.has_value() == false)
                return nullptr;
            const git_operation* const request { workspace.request_of(state->plan_operation) };
            return request == nullptr ? nullptr : std::get_if<load_rebase_plan_request>(request);
        }

        test::fake_git_runner process {};
        test::fake_workspace_files files {};
        git_operation_runner runner;
        test::manual_dispatcher dispatcher {};
        test::event_queue events {};
        git_workspace workspace;
        shell_controller controller;
        std::string repository {};
    };

    drop_party branch_party(const ref_kind kind, std::string name)
    {
        return drop_party { .kind = kind, .name = std::move(name) };
    }

    std::string sha_of(const char letter)
    {
        return std::string(40, letter);
    }

    // 편집 중인 편집기 값이다 (커밋 셋, 둘째를 고름).
    rebase_editor_view editing_view()
    {
        rebase_editor_state state {};
        state.onto_label = u8"main";
        state.branch_label = u8"feature";
        rebase_plan plan {};
        for (const char letter : { 'a', 'b', 'c' })
            plan.commits.push_back(rebase_commit { .sha = sha_of(letter), .subject = std::string { "commit " } + letter });
        start_rebase_editing(state, std::move(plan));
        state.selected = sha_of('b');
        return build_rebase_editor_view(state, std::nullopt);
    }

    std::shared_ptr<const luil::ui_tree> editor_tree(const rebase_editor_view& view)
    {
        return std::make_shared<const luil::ui_tree>(luil::make_arranged_tree(ui::build_rebase_editor(view, 1280.0f, 800.0f), { 0.0f, 0.0f, 1280.0f, 800.0f }, 1.0f));
    }

    template<typename intent_type>
    const intent_type* intent_of(const std::vector<luil::input_action>& actions)
    {
        if (actions.size() != 1 || std::holds_alternative<luil::app_message>(actions[0]) == false)
            return nullptr;
        return std::get<luil::app_message>(actions[0]).get<intent_type>();
    }
} // namespace

TEST_CASE("rebase entry: the drop menu item opens the editor for the dragged branch onto the target", "[presentation][rebase][shell]")
{
    shell_harness test {};
    // feature/graph(가장 오래된 커밋)를 main(현재, 가장 새 커밋)에 놓는다.
    test.send(drop_intent { branch_party(ref_kind::local, "feature/graph"), branch_party(ref_kind::local, "main"), 300.0f, 200.0f });
    REQUIRE(test.controller.state().menu.has_value());
    test.send(menu_select_intent { u8"interactive_rebase" });
    CHECK_FALSE(test.controller.state().menu.has_value());
    REQUIRE(test.controller.rebase_editor().is_open());
    const load_rebase_plan_request* const request { test.plan_request() };
    REQUIRE(request != nullptr);
    CHECK(request->onto == "main");
    CHECK(request->branch == "feature/graph");
    const view_snapshot view { test.controller.make_view(1440.0f, 900.0f, 1.0f) };
    REQUIRE(view.rebase_editor.has_value());
    CHECK(view.rebase_editor->title == u8"feature/graph를 main 위로 인터랙티브 rebase");
    CHECK(view.rebase_editor->phase == rebase_editor_phase::loading);
    // 편집기는 tree의 대화 상자로 선다.
    const ui::built_tree built { ui::build_ui_tree(view) };
    CHECK(built.tree->find(luil::ui_element_id { luil::ui_element_kind::modal_host, u8"rebase-editor" }) != nullptr);

    test.send(close_rebase_editor_intent {});
    CHECK_FALSE(test.controller.rebase_editor().is_open());
}

TEST_CASE("rebase entry: the commit menu and the commit panel button edit the commits above that commit", "[presentation][rebase][shell]")
{
    shell_harness test {};
    const std::string oldest { snap::sha_of(0) };
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::commit, .sha = oldest }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"interactive_rebase_here" });
    REQUIRE(test.controller.rebase_editor().is_open());
    CHECK(test.plan_request()->onto == oldest);
    CHECK(test.plan_request()->branch == "main");
    test.send(close_rebase_editor_intent {});

    // 커밋 상세 판의 단추도 같은 일을 한다. HEAD 커밋에는 아무 일도 하지 않는다.
    const std::string head { snap::sha_of(4) };
    test.send(open_interactive_rebase_here_intent { head });
    CHECK_FALSE(test.controller.rebase_editor().is_open());
    test.send(open_interactive_rebase_here_intent { oldest });
    REQUIRE(test.controller.rebase_editor().is_open());
    CHECK(test.plan_request()->onto == oldest);

    // 상세 판의 단추는 고른 커밋에 따라 켜지고 꺼진다.
    test.send(close_rebase_editor_intent {});
    test.send(select_commit_intent { graph_item_key(oldest) });
    const ui::built_tree built { ui::build_ui_tree(test.controller.make_view(1440.0f, 900.0f, 1.0f)) };
    const luil::ui_element* const button { built.tree->find(luil::ui_element_id { ui::kind_text_button, u8"detail-interactive-rebase" }) };
    REQUIRE(button != nullptr);
    CHECK(button->enabled());
}

TEST_CASE("rebase entry: the branch menu rebases the current branch onto the clicked one", "[presentation][rebase][shell]")
{
    shell_harness test {};
    test.send(open_context_menu_intent { menu_target { .kind = menu_target_kind::local_branch, .name = "feature/graph", .sha = snap::sha_of(0) }, 0.0f, 0.0f });
    test.send(menu_select_intent { u8"interactive_rebase_current_onto" });
    REQUIRE(test.controller.rebase_editor().is_open());
    CHECK(test.plan_request()->onto == "feature/graph");
    CHECK(test.plan_request()->branch == "main");
}

TEST_CASE("rebase editor tree: rows drag onto each other, letters change the action and buttons move the row", "[presentation][rebase][ui]")
{
    const rebase_editor_view view { editing_view() };
    const std::shared_ptr<const luil::ui_tree> tree { editor_tree(view) };
    const luil::ui_element* const list { tree->find(luil::ui_element_id { luil::ui_element_kind::virtual_list, u8"rebase-todo" }) };
    REQUIRE(list != nullptr);

    // p·r·s·f·d는 목록의 글자 탐색 자리가 받는다 (마지막 글자가 이번 키다). 다른 글자는 아무 일도 하지 않는다.
    REQUIRE(list->key_search() != nullptr);
    const auto searched { list->key_search()->on_search(u8"s", true) };
    REQUIRE(searched.has_value());
    const auto* const squash { intent_of<rebase_set_action_intent>(*searched) };
    REQUIRE(squash != nullptr);
    CHECK(squash->action == rebase_action::squash);
    CHECK(squash->row_key.empty());
    const auto again { list->key_search()->on_search(u8"sd", false) };
    REQUIRE(again.has_value());
    CHECK(intent_of<rebase_set_action_intent>(*again)->action == rebase_action::drop);
    CHECK_FALSE(list->key_search()->on_search(u8"x", true).has_value());

    // 줄 끌어 놓기: 다른 줄은 받고 자기 자신은 받지 않는다. 놓으면 reorder intent다.
    const luil::ui_element* const first { tree->find(luil::virtual_list_row_id(u8"rebase-todo", rebase_row_key(sha_of('a')))) };
    const luil::ui_element* const third { tree->find(luil::virtual_list_row_id(u8"rebase-todo", rebase_row_key(sha_of('c')))) };
    REQUIRE(first != nullptr);
    REQUIRE(third != nullptr);
    REQUIRE(third->drag() != nullptr);
    const luil::drag_payload payload { third->drag()->make_payload(luil::ui_action_context { third->id(), 0.0f, 0.0f }) };
    CHECK(payload.dragged_owner == rebase_row_key(sha_of('c')));
    REQUIRE(first->drop() != nullptr);
    CHECK(first->drop()->accepts(payload));
    CHECK_FALSE(third->drop()->accepts(payload));
    const std::vector<luil::input_action> dropped { first->drop()->on_drop(payload, luil::ui_action_context { first->id(), 0.0f, 0.0f }) };
    const auto* const reorder { intent_of<rebase_reorder_intent>(dropped) };
    REQUIRE(reorder != nullptr);
    CHECK(reorder->moved == rebase_row_key(sha_of('c')));
    CHECK(reorder->target == rebase_row_key(sha_of('a')));

    // 끌기와 Alt+↑↓의 자리를 단추로도 둔다 (보조 기술로 닿는다).
    for (const char8_t* owner : { u8"rebase-move-up", u8"rebase-move-down" })
    {
        const luil::ui_element* const button { tree->find(luil::ui_element_id { ui::kind_text_button, owner }) };
        REQUIRE(button != nullptr);
        CHECK(button->enabled());
        REQUIRE(button->action(luil::ui_trigger::left_click) != nullptr);
        const std::vector<luil::input_action> pressed { (*button->action(luil::ui_trigger::left_click))(luil::ui_action_context { button->id(), 0.0f, 0.0f }) };
        const auto* const move { intent_of<rebase_move_intent>(pressed) };
        REQUIRE(move != nullptr);
        CHECK(move->delta == (std::u8string_view { owner } == u8"rebase-move-up" ? -1 : 1));
    }
}

TEST_CASE("a narrow rebase editor scrolls full width content and retains footer actions", "[presentation][rebase][ui]")
{
    rebase_editor_view view { editing_view() };
    view.show_autosquash = true;
    view.show_autostash = true;
    view.needs_flatten_confirm = true;
    view.warnings = { u8"범위에 병합 커밋이 있습니다.", u8"원격에 있는 커밋의 이력을 고칩니다." };
    view.message_editable = true;
    const auto build = [&view] { return luil::make_arranged_tree(ui::build_rebase_editor(view, 480.0f, 333.0f), { 0.0f, 0.0f, 480.0f, 333.0f }, 1.0f); };
    const luil::ui_tree top { build() };
    CHECK(top.duplicate_ids().empty());
    CHECK(top.unarranged().empty());
    const auto* const list { top.find({ luil::ui_element_kind::virtual_list, u8"rebase-todo" }) };
    REQUIRE(list != nullptr);
    CHECK(list->bounds().width >= 380.0f);
    CHECK(list->bounds().height > 0.0f);
    for (const char8_t* owner : { u8"rebase-cancel", u8"rebase-start" })
    {
        const auto* const button { top.find({ ui::kind_text_button, owner }) };
        REQUIRE(button != nullptr);
        CHECK(button->bounds().x >= 16.0f);
        CHECK(button->bounds().x + button->bounds().width <= 464.0f);
        CHECK(button->bounds().y + button->bounds().height <= 317.0f);
    }
    const auto* const bar { top.find({ luil::ui_element_kind::scroll_area_bar, u8"rebase-side" }) };
    REQUIRE(bar != nullptr);
    REQUIRE(bar->accessibility().range.has_value());
    view.side_scroll = bar->accessibility().range->maximum;
    CHECK(view.side_scroll > 0.0f);
    const luil::ui_tree bottom { build() };
    const auto before { top.find({ ui::kind_text_button, u8"rebase-start" })->bounds() };
    const auto after { bottom.find({ ui::kind_text_button, u8"rebase-start" })->bounds() };
    CHECK(after.x == before.x);
    CHECK(after.y == before.y);
    CHECK(after.width == before.width);
    CHECK(after.height == before.height);
    CHECK(bottom.duplicate_ids().empty());
    CHECK(bottom.unarranged().empty());
}

TEST_CASE("rebase editor policy: Alt+arrows move the selected row and the body takes Enter as a line break", "[presentation][rebase][ui]")
{
    const std::shared_ptr<const luil::ui_tree> tree { editor_tree(editing_view()) };
    ui::cyella_policy policy {};
    luil::interaction_snapshot snapshot {};
    luil::key_pressed_event down {};
    down.key = luil::key_code::arrow_down;
    down.alt = true;
    const std::vector<luil::input_action> moved { policy.on_key(tree.get(), down, snapshot) };
    const auto* const move { intent_of<rebase_move_intent>(moved) };
    REQUIRE(move != nullptr);
    CHECK(move->delta == 1);
    CHECK(move->row_key.empty());
    // 편집기가 없으면 Alt+↓는 이 정책의 것이 아니다.
    CHECK(policy.on_key(nullptr, down, snapshot).empty());

    // 본문 칸에 초점이 있으면 Enter는 줄바꿈 입력이고 ↑는 윗줄로 간다.
    snapshot.focused_input = luil::ui_element_id { ui::kind_rebase_body_input, u8"rebase-body" };
    luil::key_pressed_event enter {};
    enter.key = luil::key_code::enter;
    const std::vector<luil::input_action> entered { policy.on_key(tree.get(), enter, snapshot) };
    const auto* const edit { intent_of<edit_text_intent>(entered) };
    REQUIRE(edit != nullptr);
    CHECK(edit->request.text == u8"\n");
    CHECK(from_luil_target(edit->request.target) == text_target::rebase_message_body);
    luil::key_pressed_event up {};
    up.key = luil::key_code::arrow_up;
    up.shift = true;
    const std::vector<luil::input_action> lined { policy.on_key(tree.get(), up, snapshot) };
    const auto* const line { intent_of<rebase_body_line_intent>(lined) };
    REQUIRE(line != nullptr);
    CHECK_FALSE(line->down);
    CHECK(line->extend);
    // 두 칸은 앱의 텍스트 대상이다.
    CHECK(policy.text_target_of(ui::kind_rebase_subject_input) == to_luil_target(text_target::rebase_message_subject));
    CHECK(policy.text_target_of(ui::kind_rebase_body_input) == to_luil_target(text_target::rebase_message_body));
}
