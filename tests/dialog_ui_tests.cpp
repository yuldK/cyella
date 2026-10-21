// 대화 상자·메뉴·라벨 칩·충돌 화면의 luil tree다. snapshot에서 배치까지 지은 tree를 읽어, 눌림·끌기·놓기가 element에 서 있는지와
// 자리가 겹치지 않는지와 탭의 삽입 방향 강조를 검증한다.
#include "presentation/dialog_intents.h"
#include "presentation/selector_intents.h"
#include "presentation/ui/app_symbol.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/cyella_policy.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"

#include "view_snapshot_support.h"

#include "luil/ui/image_decode.h"

#include <catch2/catch_test_macros.hpp>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"

#include <cmath>
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

TEST_CASE("start actions remain reachable on narrow windows and Git failure has a settings action", "[presentation][ui][start]")
{
    snapshot_harness harness {};
    for (const float width : { 480.0f, 720.0f, 1440.0f })
    {
        view_snapshot view { harness.build(width) };
        const auto built { ui::build_ui_tree(view) };
        for (const std::u8string owner : { u8"start-open", u8"start-clone", u8"start-init" })
        {
            const auto* const button { find_element(built, ui::kind_start_card, owner) };
            REQUIRE(button != nullptr);
            CHECK(button->bounds().x >= 0.0f);
            CHECK(button->bounds().x + button->bounds().width <= width);
            CHECK(button->bounds().width >= 120.0f);
        }
        view.start->git_message = u8"Git 실행 파일을 찾을 수 없습니다.";
        const auto failed { ui::build_ui_tree(view) };
        REQUIRE(find_element(failed, ui::kind_text_button, u8"start-git-settings") != nullptr);
        CHECK(find_element(failed, ui::kind_text_button, u8"start-git-settings")->enabled());
        CHECK_FALSE(find_element(failed, ui::kind_start_card, u8"start-open")->enabled());
    }
}

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
            for (const std::u8string owner : { u8"fetch", u8"pull", u8"push", u8"branch", u8"stash", u8"pop", u8"open-repository" })
                check(cyella::ui::kind_tool_button, owner);
            if (view.toolbar.show_detail_toggle)
                check(cyella::ui::kind_button, u8"detail-toggle");
        }
}

TEST_CASE("repository tabs scroll with their add button and reveal the selected tab", "[presentation][ui][tabs]")
{
    tree_fixture fixture {};
    view_snapshot view { fixture.harness.build(760.0f) };
    view.tabs.clear();
    for (repository_tab_id id { 1 }; id <= 8; ++id)
        view.tabs.push_back({ .id = id, .label = u8"저장소 " + cyella::ui::number_text(id) });
    view.selected_tab = 8;
    view.reveal_selected_tab = true;
    const cyella::ui::built_tree end { cyella::ui::build_ui_tree(view) };
    CHECK(end.tree->duplicate_ids().empty());
    CHECK(end.tree->unarranged().empty());
    const auto* const last { find_element(end, luil::ui_element_kind::tab, u8"tab-8") };
    const auto* const add { find_element(end, cyella::ui::kind_button, u8"new-tab") };
    const auto* const lane { find_element(end, cyella::ui::kind_layout, u8"tabs-lane") };
    const auto* const scroll { find_element(end, luil::ui_element_kind::scroll_area_bar, u8"tabs-scroll") };
    REQUIRE(last != nullptr);
    REQUIRE(add != nullptr);
    REQUIRE(lane != nullptr);
    REQUIRE(scroll != nullptr);
    CHECK(add->bounds().x == last->bounds().x + last->bounds().width);
    CHECK(add->bounds().x + add->bounds().width <= lane->bounds().x + lane->bounds().width);
    REQUIRE(scroll->accessibility().range.has_value());
    CHECK(scroll->accessibility().range->value == scroll->accessibility().range->maximum);
    REQUIRE(scroll->pointer_drag() != nullptr);
    CHECK(scroll->pointer_drag()->follows_pointer);
    CHECK(scroll->key_step()->axis == luil::focus_axis::horizontal);
    const auto changed { scroll->access_actions({ .command = luil::access_command::set_value, .value = 0.0f }) };
    REQUIRE(changed.has_value());
    REQUIRE(changed->size() == 1);
    const auto* const action { std::get_if<luil::app_message>(&changed->front()) };
    REQUIRE(action != nullptr);
    const auto* const intent { action->get<scroll_tabs_intent>() };
    REQUIRE(intent != nullptr);
    CHECK(intent->absolute);
    CHECK(intent->delta == 0.0f);

    view.selected_tab = 1;
    view.tab_scroll = *end.feedback.tab_scroll;
    const cyella::ui::built_tree start { cyella::ui::build_ui_tree(view) };
    CHECK(start.feedback.tab_scroll == 0.0f);
    const auto* const first { find_element(start, luil::ui_element_kind::tab, u8"tab-1") };
    REQUIRE(first != nullptr);
    CHECK(first->bounds().x == 0.0f);
    view.tabs.resize(1);
    const cyella::ui::built_tree short_row { cyella::ui::build_ui_tree(view) };
    CHECK(find_element(short_row, luil::ui_element_kind::scroll_area_bar, u8"tabs-scroll") == nullptr);
}

TEST_CASE("tab dragging paints the insertion edge that matches the reorder direction", "[presentation][ui][tabs][drag]")
{
    tree_fixture fixture {};
    view_snapshot view { fixture.harness.build() };
    view.tabs = { { .id = 1, .label = u8"하나" }, { .id = 2, .label = u8"둘" }, { .id = 3, .label = u8"셋" } };
    view.selected_tab = 2;
    const cyella::ui::built_tree built { cyella::ui::build_ui_tree(view) };
    const auto* const strip { find_element(built, cyella::ui::kind_layout, u8"repository-tabs") };
    REQUIRE(strip != nullptr);
    const luil::ui_color_palette palette { luil::color_palette_for(luil::color_theme::dark) };
    for (const bool forward : { true, false })
    {
        const auto* const target { find_element(built, luil::ui_element_kind::tab, forward ? u8"tab-3" : u8"tab-1") };
        REQUIRE(target != nullptr);
        luil::interaction_snapshot interaction {};
        interaction.drag = luil::drag_visual { .payload = { .dragged_owner = forward ? u8"tab-1" : u8"tab-3" }, .hovered_drop_target = target->id() };
        SkBitmap bitmap {};
        bitmap.allocN32Pixels(1440, 100);
        bitmap.eraseColor(palette.window_background);
        SkCanvas canvas { bitmap };
        luil::draw_context context { .canvas = canvas, .palette = palette };
        strip->draw(context, interaction);
        const luil::rect_f box { target->bounds() };
        const int left { static_cast<int>(box.x) };
        const int right { static_cast<int>(box.x + box.width) - 1 };
        const int y { static_cast<int>(box.y) + 6 };
        CHECK(bitmap.getColor(forward ? right : left, y) == palette.accent);
        CHECK(bitmap.getColor(forward ? left : right, y) != palette.accent);
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

TEST_CASE("short working panes keep the file viewport and allow scrolling to commit", "[presentation][ui][working]")
{
    tree_fixture fixture {};
    view_snapshot view { fixture.harness.build(480.0f) };
    view.graph_layout = false;
    view.height = view.logical_height = 333.0f;
    view.mode = layout_mode::narrow;
    view.repository->detail = detail_mode::working;
    view.repository->working = working_area_view {};
    view.repository->working->unstaged.push_back({ .key = u8"u:src/view.cpp", .path = "src/view.cpp", .name = u8"view.cpp", .status = 'M' });
    for (const bool warning : { false, true })
    {
        view.repository->working->amend_warning = warning;
        view.repository->working->scroll = 0.0f;
        const ui::built_tree top { ui::build_ui_tree(view) };
        REQUIRE(top.feedback.working_maximum.has_value());
        CHECK(*top.feedback.working_maximum > 0.0f);
        const luil::ui_element* const files { find_element(top, luil::ui_element_kind::virtual_list, u8"working-files") };
        REQUIRE(files != nullptr);
        CHECK(files->bounds().height >= 150.0f);
        view.repository->working->scroll = *top.feedback.working_maximum;
        const ui::built_tree bottom { ui::build_ui_tree(view) };
        const luil::ui_element* const submit { find_element(bottom, ui::kind_text_button, u8"commit-button") };
        REQUIRE(submit != nullptr);
        const luil::ui_element* const viewport { find_element(bottom, luil::ui_element_kind::scroll_area, u8"working-scroll") };
        const luil::ui_element* const status { find_element(bottom, ui::kind_panel, u8"status-bar") };
        REQUIRE(viewport != nullptr);
        REQUIRE(status != nullptr);
        CHECK(submit->bounds().y >= viewport->bounds().y);
        CHECK(submit->bounds().y + submit->bounds().height <= status->bounds().y);
        CHECK(bottom.tree->duplicate_ids().empty());
        CHECK(bottom.tree->unarranged().empty());
    }
}

TEST_CASE("clone inputs and all footer actions retain margins in a minimum window", "[presentation][ui][clone]")
{
    tree_fixture fixture {};
    view_snapshot view { fixture.harness.build(480.0f) };
    view.height = view.logical_height = 333.0f;
    view.clone = clone_dialog_view {};
    for (const int state : { 0, 1, 2 })
    {
        view.clone->running = state == 2;
        view.clone->error = state == 1 ? u8"원격에 연결하지 못했습니다. URL과 인증을 확인하세요." : u8"";
        view.clone->phase = u8"객체를 받는 중";
        const ui::built_tree built { ui::build_ui_tree(view) };
        for (const char8_t* owner : { u8"clone-close", u8"clone-submit", u8"clone-browse" })
        {
            const auto* const button { find_element(built, ui::kind_text_button, owner) };
            REQUIRE(button != nullptr);
            CHECK(button->bounds().x >= 16.0f);
            CHECK(button->bounds().y >= 16.0f);
            CHECK(button->bounds().x + button->bounds().width <= 464.0f);
            CHECK(button->bounds().y + button->bounds().height <= 317.0f);
        }
        CHECK(built.tree->duplicate_ids().empty());
        CHECK(built.tree->unarranged().empty());
    }
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

TEST_CASE("long forms keep captions and actions visible while content scrolls", "[presentation][ui][dialogs]")
{
    snapshot_harness harness {};
    for (const float width : { 480.0f, 900.0f })
    {
        CAPTURE(width);
        view_snapshot view { harness.build(width) };
        view.height = 360.0f;
        view.logical_height = 360.0f;
        form_view form {};
        form.id = u8"long-form";
        form.title = u8"변경 내용과 실행 범위 확인";
        for (int line { 0 }; line < 40; ++line)
            form.notes.push_back({ .text = u8"실행 전에 대상과 변경 내용을 확인하는 긴 안내입니다." });
        form.buttons = {
            { .key = u8"cancel", .label = u8"취소" },
            { .key = u8"keep", .label = u8"현재 변경을 임시 보관하고 선택한 브랜치로 이동" },
            { .key = u8"discard", .label = u8"현재 변경을 버리고 선택한 브랜치로 이동", .tone = form_tone::danger },
        };
        form.dismiss_key = u8"cancel";
        view.form = form;
        const ui::built_tree built { ui::build_ui_tree(view) };
        REQUIRE(built.feedback.form_maximum.has_value());
        CHECK(*built.feedback.form_maximum > 0.0f);
        const luil::ui_element* const caption { built.tree->find({ luil::ui_element_kind::dialog_caption, form.id }) };
        REQUIRE(caption != nullptr);
        CHECK(caption->bounds().y >= 15.0f);
        for (const std::u8string owner : { u8"long-form-cancel", u8"long-form-keep", u8"long-form-discard" })
        {
            const luil::ui_element* const button { find_element(built, ui::kind_text_button, owner) };
            REQUIRE(button != nullptr);
            CHECK(button->bounds().x >= 0.0f);
            CHECK(button->bounds().x + button->bounds().width <= width);
            CHECK(button->bounds().y + button->bounds().height <= 345.0f);
        }
        const auto before { find_element(built, ui::kind_text_button, u8"long-form-cancel")->bounds() };
        view.form->scroll = 10000.0f;
        const ui::built_tree scrolled { ui::build_ui_tree(view) };
        const auto after { find_element(scrolled, ui::kind_text_button, u8"long-form-cancel")->bounds() };
        CHECK(before.x == after.x);
        CHECK(before.y == after.y);
        CHECK(scrolled.tree->duplicate_ids().empty());
        CHECK(scrolled.tree->unarranged().empty());
    }
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
    for (const float width : { 480.0f, 900.0f, 1440.0f })
    {
        const auto responsive { fixture.build(width) };
        const auto* const ours { find_element(responsive, ui::kind_text_button, u8"conflict-ours:src/a.cpp") };
        const auto* const theirs { find_element(responsive, ui::kind_text_button, u8"conflict-theirs:src/a.cpp") };
        REQUIRE(ours != nullptr);
        REQUIRE(theirs != nullptr);
        CHECK(ours->accessibility().name == u8"기준(ours)");
        CHECK(theirs->accessibility().name == u8"적용 커밋(theirs)");
        CHECK(ours->bounds().x >= 0.0f);
        CHECK(theirs->bounds().x + theirs->bounds().width <= width);
        REQUIRE(find_element(responsive, ui::kind_label, u8"conflict-continue-help") != nullptr);
    }
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

TEST_CASE("repository layouts share working controls and preserve their intended panel positions", "[presentation][ui][layout]")
{
    tree_fixture fixture {};
    load(*fixture.harness.sessions["repo-1"], linear_history(6), 2);
    fixture.harness.ui.tabs[fixture.tab].selected_sha = std::string { graph_wip_sha };
    for (const float width : { 480.0f, 900.0f, 1440.0f })
        for (const bool graph : { true, false })
        {
            CAPTURE(width, graph);
            view_snapshot view { fixture.harness.build(width) };
            view.graph_layout = graph;
            view.drawer_open = graph && view.mode != layout_mode::wide;
            const ui::built_tree built { ui::build_ui_tree(view) };
            CHECK(built.tree->duplicate_ids().empty());
            CHECK(built.tree->unarranged().empty());
            const auto* const layout { find_element(built, ui::kind_segmented, u8"repository-layout") };
            REQUIRE(layout != nullptr);
            CHECK(layout->bounds().x >= 272.0f);
            CHECK(layout->bounds().x + layout->bounds().width <= width);
            // 배치 전환 컨트롤은 작업 탭 줄의 세로 가운데에 선다.
            const auto* const navigation { find_element(built, ui::kind_panel, u8"workspace-navigation-bar") };
            REQUIRE(navigation != nullptr);
            const float layout_middle { layout->bounds().y + layout->bounds().height / 2.0f };
            const float navigation_middle { navigation->bounds().y + navigation->bounds().height / 2.0f };
            CHECK(std::abs(layout_middle - navigation_middle) <= 1.0f);
            const auto* const commit { find_element(built, ui::kind_commit_subject_input, u8"commit-subject") };
            REQUIRE(commit != nullptr);
            CHECK(commit->bounds().width > 100.0f);
            const auto* const sidebar { find_element(built, ui::kind_panel, u8"sidebar-panel") };
            if (graph && view.mode != layout_mode::narrow)
                CHECK(sidebar != nullptr);
            if (graph == false)
                CHECK(sidebar == nullptr);
            if (width >= wide_layout_minimum_width && graph)
                CHECK(commit->bounds().x > width / 2.0f);
            else if (graph == false)
                CHECK(commit->bounds().x < width / 2.0f);
        }
}

TEST_CASE("Git version stays in the application footer while the start page scrolls", "[presentation][ui][start]")
{
    snapshot_harness harness {};
    view_snapshot view { harness.build(720.0f) };
    view.git_version = u8"git version 2.52.0.windows.1";
    view.start->scroll = 200.0f;
    view.height = view.logical_height = 400.0f;
    const auto built { ui::build_ui_tree(view) };
    const auto* const version { find_element(built, ui::kind_label, u8"status-git") };
    const auto* const status { find_element(built, ui::kind_panel, u8"status-bar") };
    REQUIRE(version != nullptr);
    REQUIRE(status != nullptr);
    CHECK(version->bounds().y >= status->bounds().y);
    CHECK(version->bounds().y + version->bounds().height <= view.height);
    CHECK(find_element(built, ui::kind_label, u8"start-git") == nullptr);
}

TEST_CASE("the open button asks for the app list and the list opens leftward from the button's right edge", "[presentation][ui][selector]")
{
    tree_fixture fixture {};
    const cyella::ui::built_tree closed { fixture.build() };
    const luil::ui_element* const button { find_element(closed, cyella::ui::kind_tool_button, u8"open-repository") };
    REQUIRE(button != nullptr);
    CHECK(button->enabled());
    const luil::ui_action* const click { button->action(luil::ui_trigger::left_click) };
    REQUIRE(click != nullptr);
    const std::vector<luil::input_action> actions { (*click)(luil::ui_action_context {}) };
    REQUIRE(actions.size() == 1);
    const auto* const message { std::get_if<luil::app_message>(&actions.front()) };
    REQUIRE(message != nullptr);
    const auto* const intent { message->get<open_selector_intent>() };
    REQUIRE(intent != nullptr);
    CHECK(intent->kind == selector_kind::repository_app);
    CHECK(intent->x == button->bounds().x);
    CHECK(intent->width == button->bounds().width);

    fixture.harness.ui.selector = selector_state { .kind = selector_kind::repository_app, .x = 1360.0f, .y = 90.0f, .width = 72.0f, .filter = {} };
    const cyella::ui::built_tree open { fixture.build() };
    REQUIRE(open.popups.size() == 1);
    const cyella::ui::built_popup& popup { open.popups.front() };
    CHECK(popup.width >= 280.0f);
    CHECK(popup.x + popup.width == 1360.0f + 72.0f);
    REQUIRE(popup.tree != nullptr);
    CHECK(popup.tree->find(luil::ui_element_id { luil::ui_element_kind::menu_item, u8"repository-app:0" }) != nullptr);
    CHECK(popup.tree->find(luil::ui_element_id { luil::ui_element_kind::menu_item, u8"repository-app:1" }) != nullptr);
}

TEST_CASE("the start page heading draws the title calligraphy when its images are set", "[presentation][ui][start]")
{
    // 타이틀 그림은 앱 전체가 함께 쓰는 값이라 이 test가 끝나면 비워 둔다.
    struct title_reset
    {
        ~title_reset()
        {
            ui::set_app_title({}, {});
        }
    } reset {};
    const std::vector<std::uint8_t> pixels(4u * 2u * 4u, 255u);
    ui::set_app_title(luil::make_rgba_image(4, 2, pixels), luil::make_rgba_image(4, 2, pixels));
    REQUIRE(ui::app_title_available());
    CHECK(ui::app_title_aspect() == 2.0f);

    snapshot_harness harness {};
    const auto titled { ui::build_ui_tree(harness.build(1440.0f)) };
    const luil::ui_element* const title { find_element(titled, ui::kind_image, u8"start-title") };
    REQUIRE(title != nullptr);
    CHECK(title->accessibility().role == luil::access_role::image);
    CHECK(title->accessibility().name == u8"cyella");
    CHECK(title->bounds().height > 0.0f);
    CHECK(title->bounds().width == 2.0f * title->bounds().height);
    CHECK(titled.tree->duplicate_ids().empty());

    ui::set_app_title({}, {});
    const auto plain { ui::build_ui_tree(harness.build(1440.0f)) };
    CHECK(find_element(plain, ui::kind_image, u8"start-title") == nullptr);
}

TEST_CASE("a right click on a tab asks for that tab's menu", "[presentation][ui][tabs][menu]")
{
    tree_fixture fixture {};
    const cyella::ui::built_tree built { fixture.build() };
    const luil::ui_element* const tab { find_element(built, luil::ui_element_kind::tab, u8"tab-" + cyella::ui::number_text(fixture.tab)) };
    REQUIRE(tab != nullptr);
    const luil::ui_action* const right { tab->action(luil::ui_trigger::right_click) };
    REQUIRE(right != nullptr);
    const std::vector<luil::input_action> actions { (*right)(luil::ui_action_context { .x = 12.0f, .y = 34.0f }) };
    REQUIRE(actions.size() == 1);
    const auto* const intent { std::get<luil::app_message>(actions[0]).get<open_context_menu_intent>() };
    REQUIRE(intent != nullptr);
    CHECK(intent->target.kind == menu_target_kind::tab);
    CHECK(intent->target.tab == fixture.tab);
    CHECK(intent->x == 12.0f);
    CHECK(intent->y == 34.0f);
}

TEST_CASE("the app symbol and title SVG assets read as vector images", "[presentation][ui][assets]")
{
    // 앱은 이 SVG를 실행 파일 resource로 실어 그대로 읽는다. luil의 SVG 모듈이 같은 파일을 읽는지 본다.
    const std::string assets { std::string { CYELLA_TEST_DATA_DIRECTORY } + "/../../assets/" };
    struct expected_size
    {
        const char* file;
        int width;
        int height;
    };
    for (const expected_size& expected : { expected_size { "cyella.svg", 1024, 1024 }, expected_size { "cyella-title.svg", 1990, 980 }, expected_size { "cyella-title-sea.svg", 1990, 980 } })
    {
        CAPTURE(expected.file);
        const std::string path { assets + expected.file };
        luil::image_decode_error error {};
        const luil::ui_image image { luil::load_svg_file(std::u8string { path.begin(), path.end() }, error) };
        REQUIRE(image.valid());
        CHECK(image.width() == expected.width);
        CHECK(image.height() == expected.height);
    }
}
