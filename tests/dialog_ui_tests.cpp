// 대화 상자·메뉴·라벨 칩·충돌 화면의 luil tree다. snapshot에서 배치까지 지은 tree를 읽어, 눌림·끌기·놓기가 element에 서 있는지와
// 자리가 겹치지 않는지를 검증한다 (그림은 보지 않는다).
#include "presentation/dialog_intents.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/cyella_policy.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui_intents.h"

#include "view_snapshot_support.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {
    using namespace cyella;
    using namespace cyella::test::snapshot;

    // 이력 6개에 main(현재)·feature/graph·origin/main·v1.0이 붙은 저장소다.
    struct tree_fixture
    {
        tree_fixture()
        {
            tab = harness.open(u8"C:\\work\\repo", "repo-1");
            load(*harness.sessions["repo-1"], linear_history(6), 0);
        }

        [[nodiscard]] cyella::ui::built_tree build(const float width = 1440.0f)
        {
            view = harness.build(width);
            return cyella::ui::build_ui_tree(view);
        }

        snapshot_harness harness {};
        repository_tab_id tab { 0 };
        view_snapshot view {};
    };

    const luil::ui_element* find_element(const cyella::ui::built_tree& built, const luil::ui_element_kind kind, const std::u8string& owner)
    {
        return built.tree->find(luil::ui_element_id { kind, owner });
    }
} // namespace

TEST_CASE("toolbar controls stay inside the window at every responsive boundary", "[presentation][ui][layout]")
{
    tree_fixture fixture {};
    for (const float width : { 480.0f, 560.0f, 759.0f, 760.0f, 959.0f, 960.0f, 1199.0f, 1200.0f, 1440.0f })
        for (const bool busy : { false, true })
        {
            CAPTURE(width, busy);
            view_snapshot view { fixture.harness.build(width) };
            if (busy)
                view.toolbar.busy_text = u8"원격의 새 커밋을 받는 중";
            const cyella::ui::built_tree built { cyella::ui::build_ui_tree(view) };
            float right { 0.0f };
            const auto check = [&](const luil::ui_element_kind kind, const std::u8string& owner) {
                const luil::ui_element* const control { find_element(built, kind, owner) };
                REQUIRE(control != nullptr);
                const luil::rect_f box { control->bounds() };
                CHECK(box.width > 0.0f);
                CHECK(box.x >= right);
                CHECK(box.x + box.width <= width);
                right = box.x + box.width;
            };
            if (view.toolbar.show_repository_selector)
                check(cyella::ui::kind_selector, u8"repository-selector");
            check(cyella::ui::kind_selector, u8"branch-selector");
            for (const std::u8string owner : { u8"fetch", u8"pull", u8"push", u8"branch", u8"stash", u8"pop" })
                check(cyella::ui::kind_tool_button, owner);
            if (view.toolbar.show_detail_toggle)
                check(cyella::ui::kind_button, u8"detail-toggle");
        }
}

TEST_CASE("splitter capture survives a rebuilt layout and movement outside the pane until release", "[presentation][ui][resize]")
{
    tree_fixture fixture {};
    const cyella::ui::built_tree initial { fixture.build() };
    const luil::ui_element* const splitter { find_element(initial, cyella::ui::kind_splitter, u8"sidebar-splitter") };
    REQUIRE(splitter != nullptr);
    const luil::rect_f box { splitter->bounds() };
    const float x { box.x + box.width / 2.0f };
    const float y { box.y + box.height / 2.0f };
    luil::interaction_controller interaction {};
    interaction.set_tree(initial.tree);
    static_cast<void>(interaction.process(luil::pointer_pressed_event { .x = x, .y = y }));
    CHECK_FALSE(interaction.process(luil::pointer_moved_event { .x = x + 80.0f, .y = y }).empty());
    fixture.harness.ui.sidebar_width += 80.0f;
    const cyella::ui::built_tree moved { fixture.build() };
    interaction.set_tree(moved.tree);
    CHECK_FALSE(interaction.process(luil::pointer_moved_event { .x = x + 160.0f, .y = -40.0f }).empty());
    CHECK(interaction.snapshot().pressed == splitter->id());
    static_cast<void>(interaction.process(luil::pointer_released_event { .x = x + 160.0f, .y = -40.0f }));
    CHECK(interaction.process(luil::pointer_moved_event { .x = x + 200.0f, .y = -40.0f }).empty());
}

TEST_CASE("working files use a single selection control and preserve file actions in tree view", "[presentation][ui][working]")
{
    tree_fixture fixture {};
    view_snapshot view { fixture.harness.build() };
    REQUIRE(view.repository.has_value());
    working_area_view working {};
    working.files_tree = true;
    working.unstaged.push_back({ .key = u8"u:src/view.cpp", .path = "src/view.cpp", .name = u8"view.cpp", .status = 'M' });
    working.staged.push_back({ .key = u8"s:src/view.cpp", .path = "src/view.cpp", .name = u8"view.cpp", .status = 'M' });
    view.repository->detail = detail_mode::working;
    view.repository->working = working;
    const cyella::ui::built_tree built { cyella::ui::build_ui_tree(view) };
    CHECK(built.tree->duplicate_ids().empty());
    const luil::ui_element* const control { find_element(built, cyella::ui::kind_segmented, u8"working-files-view") };
    REQUIRE(control != nullptr);
    CHECK(control->accessibility().role == luil::access_role::radio_group);
    CHECK(find_element(built, cyella::ui::kind_segment, u8"working-files-view-tree")->accessibility().selected == true);
    CHECK(find_element(built, cyella::ui::kind_button, u8"stage:src/view.cpp") != nullptr);
    CHECK(find_element(built, cyella::ui::kind_button, u8"unstage:src/view.cpp") != nullptr);
    const luil::ui_element* const body { find_element(built, cyella::ui::kind_commit_body_input, u8"commit-body") };
    REQUIRE(body != nullptr);
    REQUIRE(body->text_input().has_value());
    CHECK(body->text_input()->multiline);
    CHECK(body->bounds().height >= 80.0f);
}

TEST_CASE("graph labels are separate elements that can be dragged and dropped on", "[presentation][ui][drag]")
{
    tree_fixture fixture {};
    const cyella::ui::built_tree built { fixture.build() };
    REQUIRE(built.tree != nullptr);

    // main은 가장 새 커밋 줄에, feature/graph·origin/main·v1.0은 가장 오래된 커밋 줄에 붙는다 (같은 줄의 라벨은 서로 자리가 다르다).
    const luil::ui_element* const main_chip { find_element(built, cyella::ui::kind_ref_chip, u8"label-l:main") };
    const luil::ui_element* const feature_chip { find_element(built, cyella::ui::kind_ref_chip, u8"label-l:feature/graph") };
    const luil::ui_element* const remote_chip { find_element(built, cyella::ui::kind_ref_chip, u8"label-rb:origin/main") };
    const luil::ui_element* const tag_chip { find_element(built, cyella::ui::kind_ref_chip, u8"label-t:v1.0") };
    REQUIRE(main_chip != nullptr);
    REQUIRE(feature_chip != nullptr);

    for (const luil::ui_element* chip : { main_chip, feature_chip })
    {
        CHECK(chip->bounds().width > 0.0f);
        CHECK(chip->bounds().height > 0.0f);
        CHECK(chip->drag() != nullptr);
        CHECK(chip->drop() != nullptr);
        CHECK(chip->action(luil::ui_trigger::left_click) != nullptr);
        CHECK(chip->action(luil::ui_trigger::double_click) != nullptr);
        CHECK(chip->action(luil::ui_trigger::right_click) != nullptr);
        CHECK_FALSE(chip->tab_stop());
    }
    // 태그 라벨은 끌지 않지만 눌림과 메뉴는 있다. 칸이 모자라 숨은 라벨도 있을 수 있어 있을 때만 본다.
    if (tag_chip != nullptr && tag_chip->visible())
    {
        CHECK(tag_chip->drag() == nullptr);
        CHECK(tag_chip->action(luil::ui_trigger::right_click) != nullptr);
    }
    if (remote_chip != nullptr && remote_chip->visible())
        CHECK(remote_chip->drag() != nullptr);

    // 칩 한가운데를 누르면 행이 아니라 칩이 눌린다.
    const luil::rect_f box { main_chip->bounds() };
    const luil::ui_element* const hit { built.tree->hit_test(box.x + box.width / 2.0f, box.y + box.height / 2.0f) };
    REQUIRE(hit != nullptr);
    CHECK(hit->id() == main_chip->id());
}

TEST_CASE("a dragged label is accepted by other branches and refused by itself", "[presentation][ui][drag]")
{
    tree_fixture fixture {};
    const cyella::ui::built_tree built { fixture.build() };
    const luil::ui_element* const main_chip { find_element(built, cyella::ui::kind_ref_chip, u8"label-l:main") };
    const luil::ui_element* const feature_chip { find_element(built, cyella::ui::kind_ref_chip, u8"label-l:feature/graph") };
    REQUIRE(main_chip != nullptr);
    REQUIRE(feature_chip != nullptr);

    const luil::drag_payload payload { feature_chip->drag()->make_payload(luil::ui_action_context { feature_chip->id(), 0.0f, 0.0f }) };
    CHECK(payload.dragged_owner == u8"l:feature/graph");
    CHECK(payload.label == u8"feature/graph");
    CHECK(payload.source == feature_chip->id());
    CHECK(main_chip->drop()->accepts(payload));
    CHECK_FALSE(feature_chip->drop()->accepts(payload));

    // 사이드바 행도 같은 키로 받고 끌 수 있다.
    const luil::ui_element* const row { find_element(built, cyella::ui::kind_sidebar_row, u8"row-l:main") };
    REQUIRE(row != nullptr);
    REQUIRE(row->drop() != nullptr);
    CHECK(row->drop()->accepts(payload));
    const luil::drag_payload from_row { row->drag()->make_payload(luil::ui_action_context { row->id(), 0.0f, 0.0f }) };
    CHECK(from_row.dragged_owner == u8"l:main");
    CHECK(feature_chip->drop()->accepts(from_row));

    // 놓으면 drop_intent가 나가고 좌표를 나른다.
    const std::vector<luil::input_action> actions { main_chip->drop()->on_drop(payload, luil::ui_action_context { main_chip->id(), 120.0f, 80.0f }) };
    REQUIRE(actions.size() == 1);
    const auto* const intent { std::get<luil::app_message>(actions[0]).get<drop_intent>() };
    REQUIRE(intent != nullptr);
    CHECK(intent->source.name == "feature/graph");
    CHECK(intent->destination.name == "main");
    CHECK(intent->x == 120.0f);
}

TEST_CASE("a graph row accepts a branch drop as a commit target and the hint overlay is in the tree", "[presentation][ui][drag]")
{
    tree_fixture fixture {};
    const cyella::ui::built_tree built { fixture.build() };
    REQUIRE(fixture.view.repository.has_value());
    REQUIRE(fixture.view.repository->drop_hints != nullptr);
    CHECK(find_element(built, cyella::ui::kind_drag_hint, u8"drag-hint") != nullptr);

    const luil::ui_element* const row { find_element(built, cyella::ui::kind_graph_row, u8"commit-" + std::u8string { graph_item_key(sha_of(2)) }) };
    REQUIRE(row != nullptr);
    REQUIRE(row->drop() != nullptr);
    luil::drag_payload payload {};
    payload.dragged_owner = u8"l:main";
    CHECK(row->drop()->accepts(payload));
    payload.dragged_owner = u8"t:v1.0";
    CHECK_FALSE(row->drop()->accepts(payload));
    CHECK(row->action(luil::ui_trigger::right_click) != nullptr);
    CHECK(row->action(luil::ui_trigger::double_click) != nullptr);
}

TEST_CASE("the WIP row has no menu and refuses drops", "[presentation][ui][drag]")
{
    snapshot_harness harness {};
    harness.open(u8"C:\\work\\repo", "repo-1");
    load(*harness.sessions["repo-1"], linear_history(4), 3);
    const view_snapshot view { harness.build() };
    const cyella::ui::built_tree built { cyella::ui::build_ui_tree(view) };
    const luil::ui_element* const row { find_element(built, cyella::ui::kind_graph_row, u8"commit-" + std::u8string { graph_item_key(std::string { graph_wip_sha }) }) };
    REQUIRE(row != nullptr);
    CHECK(row->drop() == nullptr);
    CHECK(row->action(luil::ui_trigger::right_click) == nullptr);
    CHECK(row->action(luil::ui_trigger::left_click) != nullptr);
}

TEST_CASE("an open menu becomes a popup and a form becomes a modal host", "[presentation][ui][dialogs]")
{
    tree_fixture fixture {};
    fixture.harness.ui.menu = menu_state { .target = menu_target { .kind = menu_target_kind::commit, .sha = sha_of(2) }, .x = 300.0f, .y = 200.0f };
    cyella::ui::built_tree built { fixture.build() };
    REQUIRE(built.popups.size() == 1);
    CHECK(built.popups[0].id == u8"context-menu");
    CHECK(built.popups[0].x == 300.0f);
    CHECK(built.popups[0].width >= 190.0f);
    CHECK(built.popups[0].height > 100.0f);
    REQUIRE(built.popups[0].tree != nullptr);
    CHECK(built.popups[0].tree->duplicate_ids().empty());
    CHECK(built.popups[0].tree->unarranged().empty());
    // 메뉴 항목이 눌리면 menu_select_intent를 낸다.
    const luil::ui_element* const item { built.popups[0].tree->find(luil::ui_element_id { luil::ui_element_kind::menu_item, u8"cherry_pick" }) };
    REQUIRE(item != nullptr);
    CHECK(item->action(luil::ui_trigger::left_click) != nullptr);

    fixture.harness.ui.menu.reset();
    fixture.harness.ui.dialog = dialog_state { .kind = dialog_kind::create_branch, .serial = 1, .subject_label = u8"HEAD (main)", .flag_a = true };
    fixture.harness.ui.form.identity = "dialog:1";
    built = fixture.build();
    CHECK(built.popups.empty());
    CHECK(built.tree->duplicate_ids().empty());
    CHECK(built.tree->unarranged().empty());
    const luil::ui_element* const field { built.tree->find(luil::ui_element_id { cyella::ui::kind_form_input_0, u8"branch-dialog-field-0" }) };
    REQUIRE(field != nullptr);
    CHECK(field->bounds().width > 0.0f);
    // 입력 칸의 UIA 이름은 라벨 글이다 (라벨은 별도 글 요소라서 칸 이름이 비어 있었다).
    REQUIRE(fixture.view.form.has_value());
    REQUIRE(fixture.view.form->fields.empty() == false);
    const std::u8string field_name { field->accessibility().name };
    CHECK_FALSE(field_name.empty());
    CHECK(field_name.starts_with(fixture.view.form->fields[0].label));
    const luil::ui_element* const submit { built.tree->find(luil::ui_element_id { cyella::ui::kind_text_button, u8"branch-dialog-submit" }) };
    REQUIRE(submit != nullptr);
    CHECK_FALSE(submit->enabled());
    const luil::ui_element* const cancel { built.tree->find(luil::ui_element_id { cyella::ui::kind_text_button, u8"branch-dialog-cancel" }) };
    REQUIRE(cancel != nullptr);
    CHECK(cancel->enabled());
}

TEST_CASE("the conflict screen replaces the graph with per file actions and a footer", "[presentation][ui][conflict]")
{
    tree_fixture fixture {};
    repository_session& session { *fixture.harness.sessions["repo-1"] };
    repository_snapshot snapshot { session.snapshot() };
    snapshot.summary.operation_state = repository_operation_state::rebase;
    snapshot.summary.in_progress
        = in_progress_operation { .kind = repository_operation_state::rebase, .step = 1, .total = 2, .current_summary = "subject", .onto = sha_of(4), .head_name = "refs/heads/feature" };
    snapshot.conflicts = { { .path = "src/a.cpp", .status = 'U', .side = change_side::unstaged, .conflict_code = "UU" } };
    const std::uint64_t generation { session.begin_refresh() };
    REQUIRE(session.apply(snapshot_loaded { generation, snapshot }));
    REQUIRE(session.apply(commits_loaded { .generation = generation, .first_index = 0, .commits = linear_history(6), .page_complete = true, .history_complete = true }));
    const cyella::ui::built_tree built { fixture.build() };
    REQUIRE(fixture.view.repository->conflict.has_value());
    CHECK(built.tree->duplicate_ids().empty());
    CHECK(built.tree->unarranged().empty());
    CHECK(find_element(built, luil::application_element_kind(0), u8"graph-area") == nullptr);
    CHECK(find_element(built, cyella::ui::kind_text_button, u8"conflict-ours:src/a.cpp") != nullptr);
    CHECK(find_element(built, cyella::ui::kind_text_button, u8"conflict-theirs:src/a.cpp") != nullptr);
    CHECK(find_element(built, cyella::ui::kind_text_button, u8"conflict-resolved:src/a.cpp") != nullptr);
    const luil::ui_element* const proceed { find_element(built, cyella::ui::kind_text_button, u8"conflict-continue") };
    REQUIRE(proceed != nullptr);
    CHECK_FALSE(proceed->enabled());
    const luil::ui_element* const abort_button { find_element(built, cyella::ui::kind_text_button, u8"conflict-abort") };
    REQUIRE(abort_button != nullptr);
    CHECK(abort_button->enabled());
}

TEST_CASE("every tree the dialogs add has unique ids and complete layout", "[presentation][ui]")
{
    tree_fixture fixture {};
    const cyella::ui::built_tree plain { fixture.build() };
    CHECK(plain.tree->duplicate_ids().empty());
    CHECK(plain.tree->unarranged().empty());
    const cyella::ui::built_tree narrow { fixture.build(700.0f) };
    CHECK(narrow.tree->duplicate_ids().empty());
    CHECK(narrow.tree->unarranged().empty());
}
TEST_CASE("the diff header shows a textconv badge only for converted files", "[presentation][ui][diff]")
{
    tree_fixture fixture {};
    repository_session& session { *fixture.harness.sessions["repo-1"] };
    const luil::ui_element_id badge { cyella::ui::kind_label, u8"diff-textconv-badge" };
    const auto open_diff = [&](const std::string& textconv) {
        fixture.harness.ui.tabs[fixture.tab].diff = diff_file_request { .path = "doc.docx", .view = diff_view::unstaged };
        fixture.harness.ui.diff_surface.textconv_driver = textconv;
        return fixture.build();
    };
    static_cast<void>(session);

    cyella::ui::built_tree plain { open_diff("") };
    REQUIRE(plain.tree != nullptr);
    CHECK(plain.tree->find(badge) == nullptr);

    cyella::ui::built_tree converted { open_diff("word") };
    REQUIRE(converted.tree != nullptr);
    const luil::ui_element* const found { converted.tree->find(badge) };
    REQUIRE(found != nullptr);
    CHECK(found->bounds().width > 0.0f);
    CHECK(converted.tree->duplicate_ids().empty());
    CHECK(converted.tree->unarranged().empty());
}
TEST_CASE("an open selector becomes a popup with a filter field and menu items under the toolbar box", "[presentation][ui][selector]")
{
    tree_fixture fixture {};
    const cyella::ui::built_tree closed { fixture.build() };
    REQUIRE(closed.tree != nullptr);
    CHECK(closed.popups.empty());
    // 상자는 눌리고 UIA에는 combo box로 보인다.
    const luil::ui_element* const box { closed.tree->find(luil::ui_element_id { cyella::ui::kind_selector, u8"branch-selector" }) };
    REQUIRE(box != nullptr);
    CHECK(box->action(luil::ui_trigger::left_click) != nullptr);
    CHECK(box->accessibility().role == luil::access_role::combo_box);
    CHECK(box->accessibility().expanded == std::optional<bool> { false });

    fixture.harness.ui.selector = selector_state { .kind = selector_kind::branch, .x = 330.0f, .y = 90.0f, .width = 200.0f, .filter = {} };
    const cyella::ui::built_tree open { fixture.build() };
    REQUIRE(open.popups.size() == 1);
    const cyella::ui::built_popup& popup { open.popups.front() };
    CHECK(popup.id == u8"selector");
    CHECK(popup.selector);
    CHECK(popup.x == 330.0f);
    CHECK(popup.y > 90.0f);
    CHECK(popup.width >= 280.0f);
    REQUIRE(popup.tree != nullptr);
    CHECK(popup.tree->duplicate_ids().empty());
    CHECK(popup.tree->unarranged().empty());
    const luil::ui_element* const filter { popup.tree->find(luil::ui_element_id { cyella::ui::kind_selector_filter_input, u8"selector-filter" }) };
    REQUIRE(filter != nullptr);
    CHECK(filter->bounds().height > 0.0f);
    const luil::ui_element* const item { popup.tree->find(luil::ui_element_id { luil::ui_element_kind::menu_item, u8"branch:feature/graph" }) };
    REQUIRE(item != nullptr);
    CHECK(item->action(luil::ui_trigger::left_click) != nullptr);
    // 현재 브랜치 항목은 눌리지 않는다.
    const luil::ui_element* const current { popup.tree->find(luil::ui_element_id { luil::ui_element_kind::menu_item, u8"branch:main" }) };
    REQUIRE(current != nullptr);
    CHECK_FALSE(current->enabled());
    // 열린 상자는 펼침으로 알린다.
    const luil::ui_element* const opened_box { open.tree->find(luil::ui_element_id { cyella::ui::kind_selector, u8"branch-selector" }) };
    REQUIRE(opened_box != nullptr);
    CHECK(opened_box->accessibility().expanded == std::optional<bool> { true });
}

TEST_CASE("the repository selector list ends with the folder picker that runs a UI thread command", "[presentation][ui][selector]")
{
    tree_fixture fixture {};
    fixture.harness.ui.selector = selector_state { .kind = selector_kind::repository, .x = 20.0f, .y = 90.0f, .width = 150.0f, .filter = {} };
    const cyella::ui::built_tree open { fixture.build() };
    REQUIRE(open.popups.size() == 1);
    REQUIRE(open.popups.front().tree != nullptr);
    const luil::ui_element* const folder { open.popups.front().tree->find(luil::ui_element_id { luil::ui_element_kind::menu_item, u8"open_folder" }) };
    REQUIRE(folder != nullptr);
    CHECK(folder->enabled());
    const luil::ui_action* const click { folder->action(luil::ui_trigger::left_click) };
    REQUIRE(click != nullptr);
    const std::vector<luil::input_action> actions { (*click)(luil::ui_action_context {}) };
    REQUIRE(actions.size() == 1);
    CHECK(std::holds_alternative<luil::app_ui_command>(actions.front()));
}
