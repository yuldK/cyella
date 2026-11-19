// 대화 상자·메뉴·라벨 칩·충돌 화면의 luil tree다. snapshot에서 배치까지 지은 tree를 읽어, 눌림·끌기·놓기가 element에 서 있는지와
// 자리가 겹치지 않는지와 탭의 삽입 방향 강조를 검증한다.
#include "domain/text_catalog.h"
#include "presentation/commands/command_intents.h"
#include "presentation/commands/command_registry.h"
#include "presentation/dialog_intents.h"
#include "presentation/menu_model.h"
#include "presentation/rebase/rebase_editor_intents.h"
#include "presentation/rebase/rebase_editor_view.h"
#include "presentation/selector_intents.h"
#include "presentation/ui/app_symbol.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/cyella_caption.h"
#include "presentation/ui/cyella_policy.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/graph_row_element.h"
#include "presentation/ui/press_row.h"
#include "presentation/ui/rebase/rebase_editor_ui.h"
#include "presentation/ui/ref_chip_element.h"
#include "presentation/ui/repository_tabs_element.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui/ui_parts.h"
#include "presentation/ui_intents.h"

#include "view_snapshot_support.h"

#include "luil/generated/codicons.h"
#include "luil/ui/image_decode.h"
#include "luil/ui/ui_interaction.h"
#include "luil/ui/ui_platform.h"
#include "luil/ui/virtual_list_element.h"

#include <catch2/catch_test_macros.hpp>

#include "include/core/SkBitmap.h"
#include "include/core/SkCanvas.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

TEST_CASE("overflow refs expose the visible branch and every hidden ref", "[ui][refs]")
{
    using namespace cyella;
    const std::vector<ref_label> labels {
        { .kind = ref_label_kind::local, .text = u8"main", .ref_name = "main", .current = true, .also_remote = true, .remote_name = u8"origin/main" },
        { .kind = ref_label_kind::local, .text = u8"feature/hidden", .ref_name = "feature/hidden" },
        { .kind = ref_label_kind::tag, .text = u8"v1.0", .ref_name = "v1.0" },
    };
    ui::graph_row_data data {};
    data.labels = labels;
    data.row_key = u8"abc";
    data.sha = "abc";
    ui::graph_columns columns {};
    columns.refs = 140.0f;
    auto row { std::make_unique<ui::graph_row_element>(u8"test-row", data, columns) };
    const auto tree { luil::make_arranged_tree(std::move(row), { 0.0f, 0.0f, 600.0f, 28.0f }, 1.0f) };
    const auto* const overflow { tree.find({ ui::kind_ref_overflow, u8"overflow-abc" }) };
    REQUIRE(overflow != nullptr);
    REQUIRE(overflow->visible());
    CHECK(overflow->accessibility().role == luil::access_role::button);
    REQUIRE(overflow->action(luil::ui_trigger::left_click) != nullptr);
    const auto actions { (*overflow->action(luil::ui_trigger::left_click))({}) };
    REQUIRE(actions.size() == 1);
    const auto* const opened { std::get<luil::app_message>(actions.front()).get<open_ref_list_intent>() };
    REQUIRE(opened != nullptr);
    CHECK(opened->sha == "abc");
    // 카드가 꼬리로 가리킬 칩의 사각형 전체를 싣는다.
    CHECK(opened->x == overflow->bounds().x);
    CHECK(opened->y == overflow->bounds().y);
    CHECK(opened->width == overflow->bounds().width);
    CHECK(opened->height == overflow->bounds().height);

    // 칸 어디에 올려도 그 칸의 hover 영역 안이다 (칩 사이 빈 칸 포함). 칸 밖(메시지 열)이나 다른 높이면 없다.
    const luil::rect_f chip { overflow->bounds() };
    const auto regions_at = [&tree](const float x, const float y) {
        std::vector<luil::ui_element_id> ids {};
        for (const luil::ui_element* const element : tree.hover_regions_at(x, y))
            ids.push_back(element->id());
        return ids;
    };
    const std::vector<luil::ui_element_id> cell { ui::ref_cell_id(u8"abc") };
    CHECK(regions_at(4.0f, 14.0f) == cell);
    CHECK(regions_at(chip.x + chip.width / 2.0f, 14.0f) == cell);
    CHECK(regions_at(300.0f, 14.0f).empty());
    CHECK(regions_at(4.0f, 40.0f).empty());

    test::snapshot::snapshot_harness harness {};
    const auto tab { harness.open(u8"C:/work/repo", "repo-1") };
    test::snapshot::load(*harness.sessions["repo-1"], test::snapshot::linear_history(1), 0);
    view_snapshot view { harness.build() };
    auto map { std::make_shared<ref_label_map>() };
    (*map)["abc"] = labels;
    view.repository->graph.labels = map;
    view.ref_list = ref_list_state { .sha = "abc", .x = 140.0f, .y = 200.0f, .width = 24.0f, .height = 20.0f, .row_key = u8"abc", .serial = 3 };
    const auto built { ui::build_ui_tree(view) };
    // 카드는 투명 popup이다. 창 모양이 tree의 알파라 꼬리와 그림자를 tree가 그리고, OS 경계는 긋지 않는다. 주 창 tree에는 없다.
    REQUIRE(built.popups.size() == 1);
    const ui::built_popup& popup { built.popups.front() };
    CHECK(popup.id == u8"ref-list");
    CHECK(popup.ref_list);
    CHECK(popup.transparent);
    CHECK_FALSE(popup.border);
    CHECK(built.tree->find(ui::ref_list_card_id()) == nullptr);
    REQUIRE(popup.tree != nullptr);
    CHECK(popup.tree->duplicate_ids().empty());
    const auto* const card { popup.tree->find(ui::ref_list_card_id()) };
    REQUIRE(card != nullptr);
    // 보조 기술에는 카드 묶음이 popup의 첫 요소로 선다 (뿌리는 구조라 접힌다).
    CHECK(card->accessibility().role == luil::access_role::group);
    CHECK(card->accessibility().name.empty() == false);
    REQUIRE(popup.tree->root() != nullptr);
    CHECK(popup.tree->root()->accessibility().role == luil::access_role::none);
    // 칩 아래에 자리가 넉넉하니 카드는 칩 바로 아래에 선다. popup은 꼬리 끝(칩 아래 2px)에서 시작하고 옆과 아래로 그림자만큼 넓다.
    // 그림자 띠는 입력을 받으므로(알파가 0이 아니다) 옆 3, 아래 4로 짧다. 그 밖의 누름은 아래 행이 받는다.
    CHECK(popup.y == 222.0f);
    CHECK(card->bounds().y == 0.0f);
    CHECK(card->bounds().x == 3.0f);
    CHECK(popup.width == card->bounds().width + 6.0f);
    CHECK(popup.height == card->bounds().height + 4.0f);
    // 묶음 머리는 종류마다 하나다.
    CHECK(popup.tree->find({ luil::ui_element_kind::menu_item, u8"ref-list-group-local" }) != nullptr);
    CHECK(popup.tree->find({ luil::ui_element_kind::menu_item, u8"ref-list-group-remote" }) != nullptr);
    CHECK(popup.tree->find({ luil::ui_element_kind::menu_item, u8"ref-list-group-tag" }) != nullptr);
    CHECK_FALSE(popup.tree->find({ luil::ui_element_kind::menu_item, u8"ref-list-group-local" })->enabled());
    // 호스트가 알린 자리는 이 카드의 번호와 청한 자리를 싣는다.
    REQUIRE(popup.placed != nullptr);
    const luil::app_message placed { popup.placed(luil::rect_f { popup.x + 5.0f, popup.y, popup.width, popup.height }) };
    const auto* const report { placed.get<ref_list_placed_intent>() };
    REQUIRE(report != nullptr);
    CHECK(report->serial == 3);
    CHECK(report->requested_x == popup.x);
    CHECK(report->x == popup.x + 5.0f);
    // 그때의 그림자 여백도 싣는다. 아래로 선 카드라 칩 쪽(위)은 0이다.
    CHECK(report->margin_above == 0.0f);
    CHECK(report->margin_below == 4.0f);
    const std::vector<std::pair<std::u8string, ref_label>> picks {
        { u8"ref-list-l:main", labels[0] },
        { u8"ref-list-rb:origin/main", ref_label { .kind = ref_label_kind::remote, .text = u8"origin/main", .ref_name = "origin/main" } },
        { u8"ref-list-l:feature/hidden", labels[1] },
        { u8"ref-list-t:v1.0", labels[2] },
    };
    for (const auto& [key, label] : picks)
    {
        const auto* const item { popup.tree->find({ luil::ui_element_kind::menu_item, key }) };
        REQUIRE(item != nullptr);
        REQUIRE(item->action(luil::ui_trigger::left_click) != nullptr);
        REQUIRE(item->action(luil::ui_trigger::right_click) != nullptr);
        const auto selected { (*item->action(luil::ui_trigger::left_click))({}) };
        REQUIRE(selected.size() == 1);
        const auto* const menu { std::get<luil::app_message>(selected.front()).get<open_context_menu_intent>() };
        REQUIRE(menu != nullptr);
        CHECK(menu->target.name == label.ref_name);
        CHECK(menu->target.sha == "abc");
        // 카드의 항목과 칩의 우클릭은 같은 조립이 짓는 대상을 연다 (로컬·원격·태그가 서로 다른 종류로 간다).
        const std::optional<menu_target> expected { ui::menu_target_of(label, "abc") };
        REQUIRE(expected.has_value());
        CHECK(menu->target == *expected);
        // 메뉴는 카드가 가리키는 칩 아래에 선다.
        CHECK(menu->x == 140.0f);
        CHECK(menu->y == 220.0f);
        // 짧은 이름은 카드 안에 다 보이므로 툴팁을 달지 않는다 (툴팁은 popup 밖으로 나가지 못한다).
        CHECK_FALSE(item->has_tooltip());
    }
    static_cast<void>(tab);
}

TEST_CASE("the ref card points its tail at the overflow chip and flips above near the bottom", "[ui][refs]")
{
    using namespace cyella;
    const std::vector<ref_label> labels {
        { .kind = ref_label_kind::local, .text = u8"main", .ref_name = "main" },
        { .kind = ref_label_kind::tag, .text = u8"v1.0", .ref_name = "v1.0" },
    };
    const shell_rect area { 0.0f, 100.0f, 1200.0f, 600.0f };

    // 아래에 자리가 있으면 칩 아래에 서고 꼬리가 위로 칩 가운데를 가리킨다 (배율 2라 상태의 칩은 물리 픽셀이다).
    const ui::placed_ref_list below { ui::build_ref_list_card(ref_list_state { .sha = "abc", .x = 280.0f, .y = 400.0f, .width = 40.0f, .height = 40.0f }, labels, 2.0f, area) };
    REQUIRE(below.popup.tree != nullptr);
    CHECK(below.tail_up);
    CHECK(below.tail_x == 150.0f);
    CHECK(below.rect.y == 222.0f);
    CHECK(below.rect.x < 150.0f);
    CHECK(below.rect.x + below.rect.width > 150.0f);

    // 칩이 본문 바닥 가까이 있으면 칩 위에 서고 꼬리가 아래를 가리킨다. 본문 밖으로 나가지 않는다.
    const ui::placed_ref_list above { ui::build_ref_list_card(ref_list_state { .sha = "abc", .x = 280.0f, .y = 1360.0f, .width = 40.0f, .height = 40.0f }, labels, 2.0f, area) };
    REQUIRE(above.popup.tree != nullptr);
    CHECK_FALSE(above.tail_up);
    CHECK(above.rect.y + above.rect.height == 678.0f);
    CHECK(above.rect.y >= area.y);
    // 위에 선 카드의 popup은 꼬리 끝(칩 위 2px)에서 끝난다. 아래 그림자가 칩 줄을 덮어 칩 누름을 삼키지 않는다.
    CHECK(above.popup.y + above.popup.height == 678.0f);
    CHECK(above.popup.y == above.rect.y - 2.0f);

    // 그림자만 넘쳐 호스트가 밀었으면 칩에서 먼 쪽 여백을 그만큼 깎는다. 카드 자리는 그대로다.
    const ui::placed_ref_list trimmed {
        ui::build_ref_list_card(ref_list_state { .sha = "abc", .x = 280.0f, .y = 400.0f, .width = 40.0f, .height = 40.0f, .shadow_trim = 3.0f }, labels, 2.0f, area)
    };
    REQUIRE(trimmed.popup.tree != nullptr);
    CHECK(trimmed.rect.y == below.rect.y);
    CHECK(trimmed.popup.y == below.popup.y);
    CHECK(trimmed.popup.height == below.popup.height - 3.0f);
    REQUIRE(trimmed.popup.placed != nullptr);
    const luil::app_message trimmed_report { trimmed.popup.placed(luil::rect_f { trimmed.popup.x, trimmed.popup.y, trimmed.popup.width, trimmed.popup.height }) };
    REQUIRE(trimmed_report.get<ref_list_placed_intent>() != nullptr);
    CHECK(trimmed_report.get<ref_list_placed_intent>()->margin_below == 1.0f);
    const ui::placed_ref_list trimmed_above {
        ui::build_ref_list_card(ref_list_state { .sha = "abc", .x = 280.0f, .y = 1360.0f, .width = 40.0f, .height = 40.0f, .shadow_trim = 5.0f }, labels, 2.0f, area)
    };
    REQUIRE(trimmed_above.popup.tree != nullptr);
    CHECK(trimmed_above.popup.y == trimmed_above.rect.y);
    CHECK(trimmed_above.popup.y + trimmed_above.popup.height == 678.0f);

    // 창 오른쪽 끝의 칩이면 카드가 안으로 밀리고 꼬리만 칩에 남는다.
    const ui::placed_ref_list edge { ui::build_ref_list_card(ref_list_state { .sha = "abc", .x = 2300.0f, .y = 400.0f, .width = 30.0f, .height = 40.0f }, labels, 2.0f, area) };
    REQUIRE(edge.popup.tree != nullptr);
    CHECK(edge.rect.x + edge.rect.width <= 1192.0f);
    CHECK(edge.tail_x == 1157.5f);
    CHECK(edge.tail_x <= edge.rect.x + edge.rect.width);

    // 호스트가 popup을 오른쪽으로 10 밀었으면 카드도 그만큼 옮겨 서므로 꼬리를 거꾸로 옮겨 여전히 칩 가운데를 가리킨다.
    const ui::placed_ref_list shifted {
        ui::build_ref_list_card(ref_list_state { .sha = "abc", .x = 280.0f, .y = 400.0f, .width = 40.0f, .height = 40.0f, .placed_shift = 10.0f }, labels, 2.0f, area)
    };
    REQUIRE(shifted.popup.tree != nullptr);
    CHECK(shifted.popup.x == below.popup.x);
    CHECK(shifted.tail_x == 150.0f);
    const auto* const shifted_card { shifted.popup.tree->find(ui::ref_list_card_id()) };
    const auto* const below_card { below.popup.tree->find(ui::ref_list_card_id()) };
    REQUIRE(shifted_card != nullptr);
    REQUIRE(below_card != nullptr);
    // 그림을 견주지 않고, 같은 칩에 대한 두 카드의 꼬리가 밀린 거리만큼 다른 픽셀에 서는지 본다.
    const auto tail_column = [](const luil::ui_element& card) {
        SkBitmap bitmap {};
        const luil::rect_f box { card.bounds() };
        bitmap.allocN32Pixels(static_cast<int>(box.x + box.width) + 4, static_cast<int>(box.y + box.height) + 4);
        bitmap.eraseColor(SK_ColorTRANSPARENT);
        SkCanvas canvas { bitmap };
        const luil::ui_color_palette palette { luil::color_palette_for(luil::color_theme::light) };
        luil::draw_context context { .canvas = canvas, .palette = palette, .scale = 2.0f };
        card.draw(context, luil::interaction_snapshot {});
        // 꼬리 끝 가까운 줄에서 진하게 칠해진 자리의 가운데 x다. 그 높이의 그림자는 옅어 걸리지 않는다.
        const int y { static_cast<int>(box.y) + 4 };
        int first { -1 };
        int last { -1 };
        for (int x { 0 }; x < bitmap.width(); ++x)
            if (SkColorGetA(bitmap.getColor(x, y)) > 128)
            {
                if (first < 0)
                    first = x;
                last = x;
            }
        return (first + last) / 2;
    };
    CHECK(tail_column(*below_card) - tail_column(*shifted_card) == 20);

    // 호스트가 세로로 밀었으면 칩 반대쪽으로 뒤집어 선다.
    const ui::placed_ref_list flipped {
        ui::build_ref_list_card(ref_list_state { .sha = "abc", .x = 280.0f, .y = 400.0f, .width = 40.0f, .height = 40.0f, .flipped = true }, labels, 2.0f, area)
    };
    CHECK_FALSE(flipped.tail_up);
    CHECK(flipped.rect.y + flipped.rect.height == 198.0f);

    // 참조가 없는 커밋이면 카드가 없다.
    CHECK(ui::build_ref_list_card(ref_list_state { .sha = "abc" }, {}, 1.0f, area).popup.tree == nullptr);
}

TEST_CASE("a long ref name gets a tooltip because the card cannot show it whole", "[ui][refs]")
{
    using namespace cyella;
    const std::vector<ref_label> labels {
        { .kind = ref_label_kind::local, .text = u8"feature/a-very-long-branch-name-that-does-not-fit-in-the-card-at-all", .ref_name = "feature/long" },
        { .kind = ref_label_kind::tag, .text = u8"v1.0", .ref_name = "v1.0" },
    };
    const ui::placed_ref_list card { ui::build_ref_list_card(ref_list_state { .sha = "abc", .x = 280.0f, .y = 400.0f, .width = 40.0f, .height = 40.0f }, labels, 1.0f, { 0.0f, 0.0f, 1600.0f, 900.0f }) };
    REQUIRE(card.popup.tree != nullptr);
    const auto* const long_item { card.popup.tree->find({ luil::ui_element_kind::menu_item, u8"ref-list-l:feature/long" }) };
    const auto* const short_item { card.popup.tree->find({ luil::ui_element_kind::menu_item, u8"ref-list-t:v1.0" }) };
    REQUIRE(long_item != nullptr);
    REQUIRE(short_item != nullptr);
    CHECK(long_item->has_tooltip());
    CHECK_FALSE(short_item->has_tooltip());
}

namespace {
    // 커밋 셋 가운데 앞의 둘에 브랜치를 넉넉히 붙여 두 행 모두 "+N"이 서는 화면이다.
    struct overflow_fixture
    {
        overflow_fixture()
        {
            static_cast<void>(harness.open(u8"C:/work/repo", "repo-1"));
            cyella::test::snapshot::load(*harness.sessions["repo-1"], cyella::test::snapshot::linear_history(3), 0);
            auto map { std::make_shared<cyella::ref_label_map>() };
            for (const std::string& sha : { first, second })
                for (int index { 0 }; index < 5; ++index)
                {
                    const std::string name { "topic-" + sha.substr(0, 4) + "-" + std::to_string(index) };
                    (*map)[sha].push_back({ .kind = cyella::ref_label_kind::local, .text = std::u8string { name.begin(), name.end() }, .ref_name = name });
                }
            // 셋째 커밋은 칩이 다 들어가 "+N"이 서지 않는 행이다.
            (*map)[third].push_back({ .kind = cyella::ref_label_kind::tag, .text = u8"v1", .ref_name = "v1" });
            labels = map;
        }

        [[nodiscard]] cyella::ui::built_tree build(const std::optional<cyella::ref_list_state>& card = std::nullopt, const bool dialog = false)
        {
            cyella::view_snapshot view { harness.build() };
            view.repository->graph.labels = labels;
            view.ref_list = card;
            if (dialog)
            {
                cyella::form_view form {};
                form.id = u8"covering-form";
                form.title = u8"확인";
                form.buttons = { { .key = u8"cancel", .label = u8"취소" } };
                form.dismiss_key = u8"cancel";
                view.form = form;
            }
            return cyella::ui::build_ui_tree(view);
        }

        cyella::test::snapshot::snapshot_harness harness {};
        std::string first { cyella::test::snapshot::sha_of(0) };
        std::string second { cyella::test::snapshot::sha_of(1) };
        std::string third { cyella::test::snapshot::sha_of(2) };
        std::shared_ptr<cyella::ref_label_map> labels {};
    };

    template<typename intent_type>
    [[nodiscard]] std::vector<intent_type> messages_of(const std::vector<luil::input_action>& actions)
    {
        std::vector<intent_type> found {};
        for (const luil::input_action& action : actions)
            if (const auto* const message { std::get_if<luil::app_message>(&action) }; message != nullptr)
                if (const auto* const intent { message->get<intent_type>() }; intent != nullptr)
                    found.push_back(*intent);
        return found;
    }

    [[nodiscard]] luil::point_f middle_of(const luil::rect_f& box)
    {
        return { box.x + box.width / 2.0f, box.y + box.height / 2.0f };
    }
} // namespace

TEST_CASE("a refs cell reports entry and exit and its dwell ends on press and drag", "[ui][refs]")
{
    using namespace cyella;
    overflow_fixture fixture {};
    const ui::built_tree built { fixture.build() };
    const luil::ui_element* const first { built.tree->find(ui::ref_cell_id(graph_item_key(fixture.first))) };
    const luil::ui_element* const second { built.tree->find(ui::ref_cell_id(graph_item_key(fixture.second))) };
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    REQUIRE(first->hover_region() != nullptr);
    REQUIRE(first->hover_dwell() != nullptr);
    const luil::ui_element* const chip { built.tree->find({ ui::kind_ref_overflow, u8"overflow-" + graph_item_key(fixture.first) }) };
    REQUIRE(chip != nullptr);
    // 칩 줄의 아래(칩과 행 끝 사이의 빈 칸)다. 칩 위가 아니어도 칸이다.
    const luil::rect_f cell { first->bounds() };
    const luil::point_f gap { cell.x + 3.0f, cell.y + 1.0f };
    const luil::point_f message_column { cell.x + cell.width + 200.0f, cell.y + cell.height / 2.0f };
    const auto t0 { std::chrono::steady_clock::time_point {} + std::chrono::hours { 1 } };

    luil::interaction_controller interaction {};
    interaction.set_tree(built.tree);
    // 칸에 들어오면 영역이 곧바로 알리고, 머무름(지연 0)은 그 시각에 찬다. 칸의 사각형은 "+N" 칩의 자리다.
    const auto entered { messages_of<ref_cell_hover_intent>(interaction.process(luil::pointer_moved_event { .x = gap.x, .y = gap.y, .time = t0 })) };
    REQUIRE(entered.size() == 1);
    CHECK(entered.front().inside);
    CHECK(entered.front().row_key == graph_item_key(fixture.first));
    CHECK(entered.front().sha == fixture.first);
    CHECK(entered.front().x == chip->bounds().x);
    CHECK(entered.front().width == chip->bounds().width);
    CHECK(interaction.snapshot().hovered == first->id());
    const auto dwelt { messages_of<ref_cell_dwell_intent>(interaction.advance(t0)) };
    REQUIRE(dwelt.size() == 1);
    CHECK(dwelt.front().live);

    // 같은 행의 메시지 열로 옮기면 칸을 떠난다.
    const auto out { interaction.process(luil::pointer_moved_event { .x = message_column.x, .y = message_column.y, .time = t0 }) };
    REQUIRE(messages_of<ref_cell_hover_intent>(out).size() == 1);
    CHECK_FALSE(messages_of<ref_cell_hover_intent>(out).front().inside);
    REQUIRE(messages_of<ref_cell_dwell_intent>(out).size() == 1);
    CHECK_FALSE(messages_of<ref_cell_dwell_intent>(out).front().live);

    // 메시지 열을 눌러 커밋을 고른 뒤 같은 행의 칸으로 옮겨도 머무름이 선다 (머무름의 임자가 행이 아니라 칸이다).
    static_cast<void>(interaction.process(luil::pointer_pressed_event { .x = message_column.x, .y = message_column.y, .time = t0 }));
    static_cast<void>(interaction.process(luil::pointer_released_event { .x = message_column.x, .y = message_column.y, .time = t0 }));
    const auto t1 { t0 + std::chrono::seconds { 1 } };
    CHECK(messages_of<ref_cell_hover_intent>(interaction.process(luil::pointer_moved_event { .x = gap.x, .y = gap.y, .time = t1 })).size() == 1);
    const auto again { messages_of<ref_cell_dwell_intent>(interaction.advance(t1)) };
    REQUIRE(again.size() == 1);
    CHECK(again.front().live);

    // 칸 안을 누르면 머무름이 끝나지만 영역은 남는다 (포인터는 칸 자리 안이다). 칸을 떠날 때까지 머무름은 다시 서지 않는다.
    const auto pressed { interaction.process(luil::pointer_pressed_event { .x = gap.x, .y = gap.y, .time = t1 }) };
    REQUIRE(messages_of<ref_cell_dwell_intent>(pressed).size() == 1);
    CHECK_FALSE(messages_of<ref_cell_dwell_intent>(pressed).front().live);
    CHECK(messages_of<ref_cell_hover_intent>(pressed).empty());
    static_cast<void>(interaction.process(luil::pointer_released_event { .x = gap.x, .y = gap.y, .time = t1 }));
    static_cast<void>(interaction.process(luil::pointer_moved_event { .x = gap.x + 1.0f, .y = gap.y, .time = t1 }));
    CHECK(messages_of<ref_cell_dwell_intent>(interaction.advance(t1 + std::chrono::seconds { 1 })).empty());

    // 칩을 쥐고 다른 행의 칸까지 끌면, 지나는 칸의 영역은 들어옴을 알려도 머무름은 서지 않는다 (끄는 동안 hover는 쥔 칩에 남는다).
    const luil::ui_element* branch { nullptr };
    for (const luil::ui_element_id& id : built.tree->ids_of_kind(ui::kind_ref_chip))
        if (const luil::ui_element* const found { built.tree->find(id) }; found != nullptr && found->visible() && found->drag() != nullptr && second->bounds().contains(middle_of(found->bounds()).x, middle_of(found->bounds()).y))
            branch = found;
    REQUIRE(branch != nullptr);
    const luil::point_f grip { middle_of(branch->bounds()) };
    const auto t2 { t1 + std::chrono::seconds { 2 } };
    static_cast<void>(interaction.process(luil::pointer_moved_event { .x = grip.x, .y = grip.y, .time = t2 }));
    static_cast<void>(interaction.advance(t2));
    static_cast<void>(interaction.process(luil::pointer_pressed_event { .x = grip.x, .y = grip.y, .time = t2 }));
    const auto dragged { interaction.process(luil::pointer_moved_event { .x = gap.x, .y = gap.y, .time = t2, .in_contact = true }) };
    REQUIRE(interaction.snapshot().drag.has_value());
    const auto crossed { messages_of<ref_cell_hover_intent>(dragged) };
    CHECK(std::ranges::any_of(crossed, [&](const ref_cell_hover_intent& intent) { return intent.inside && intent.row_key == graph_item_key(fixture.first); }));
    const auto during { messages_of<ref_cell_dwell_intent>(interaction.advance(t2 + std::chrono::seconds { 1 })) };
    CHECK(std::ranges::none_of(during, [](const ref_cell_dwell_intent& intent) { return intent.live; }));
    static_cast<void>(interaction.process(luil::pointer_released_event { .x = gap.x, .y = gap.y, .time = t2 + std::chrono::seconds { 1 } }));

    // 창을 떠나면 들어와 있던 영역이 모두 나간다.
    const auto gone { messages_of<ref_cell_hover_intent>(interaction.process(luil::pointer_left_event {})) };
    REQUIRE_FALSE(gone.empty());
    CHECK(std::ranges::none_of(gone, [](const ref_cell_hover_intent& intent) { return intent.inside; }));
    CHECK(second->hover_region() != nullptr);

    // 대화 상자의 scrim이 행을 덮으면 칸의 영역은 들어옴을 알려도 머무름은 서지 않는다 (controller는 둘이 함께 서야 연다).
    const ui::built_tree covered { fixture.build(std::nullopt, true) };
    luil::interaction_controller blocked {};
    blocked.set_tree(covered.tree);
    const auto t3 { t2 + std::chrono::seconds { 10 } };
    const auto under { messages_of<ref_cell_hover_intent>(blocked.process(luil::pointer_moved_event { .x = gap.x, .y = gap.y, .time = t3 })) };
    REQUIRE(under.size() == 1);
    CHECK(under.front().inside);
    CHECK(messages_of<ref_cell_dwell_intent>(blocked.advance(t3 + std::chrono::seconds { 1 })).empty());
}

TEST_CASE("a press between the chips still selects, opens the menu and takes drops like its row", "[ui][refs]")
{
    // 칸은 칩 사이 빈 칸의 hit를 받으므로 행의 손짓을 같은 액션으로 갖는다. 놓기는 행이 받는다.
    using namespace cyella;
    overflow_fixture fixture {};
    const ui::built_tree built { fixture.build() };
    const luil::ui_element* const cell { built.tree->find(ui::ref_cell_id(graph_item_key(fixture.second))) };
    REQUIRE(cell != nullptr);
    const luil::point_f gap { cell->bounds().x + 3.0f, cell->bounds().y + 1.0f };
    const luil::ui_element* const hit { built.tree->hit_test(gap.x, gap.y) };
    REQUIRE(hit != nullptr);
    CHECK(hit->id() == cell->id());

    ui::cyella_policy policy {};
    luil::interaction_controller interaction { &policy };
    interaction.set_tree(built.tree);
    const auto pressed { messages_of<select_commit_intent>(interaction.process(luil::pointer_pressed_event { .x = gap.x, .y = gap.y, .control = true })) };
    REQUIRE(pressed.size() == 1);
    CHECK(pressed.front().key == graph_item_key(fixture.second));
    CHECK(pressed.front().control);
    static_cast<void>(interaction.process(luil::pointer_released_event { .x = gap.x, .y = gap.y }));
    CHECK(interaction.snapshot().focused == luil::ui_element_id { luil::ui_element_kind::virtual_list, u8"graph" });

    REQUIRE(cell->action(luil::ui_trigger::right_click) != nullptr);
    const auto menu { messages_of<open_context_menu_intent>((*cell->action(luil::ui_trigger::right_click))({ .element = cell->id(), .x = gap.x, .y = gap.y })) };
    REQUIRE(menu.size() == 1);
    CHECK(menu.front().target.kind == menu_target_kind::commit);
    CHECK(menu.front().target.sha == fixture.second);
    REQUIRE(cell->action(luil::ui_trigger::double_click) != nullptr);
    CHECK(messages_of<activate_commit_intent>((*cell->action(luil::ui_trigger::double_click))({ .element = cell->id() })).size() == 1);
    // 누른 채 끌어 고르기도 같다.
    REQUIRE(cell->pointer_drag() != nullptr);

    luil::drag_payload payload {};
    payload.dragged_owner = branch_drag_key(ref_kind::local, "elsewhere");
    const luil::ui_element* const target { built.tree->find_drop_target(gap.x, gap.y, payload) };
    REQUIRE(target != nullptr);
    CHECK(target->id().kind == ui::kind_graph_row);
}

TEST_CASE("the ref card popup reports pointer entry and leaves when it disappears", "[ui][refs]")
{
    using namespace cyella;
    overflow_fixture fixture {};
    const ref_list_state open { .sha = fixture.first, .x = 100.0f, .y = 60.0f, .width = 24.0f, .height = 18.0f, .row_key = graph_item_key(fixture.first), .serial = 7 };
    const ui::built_tree built { fixture.build(open) };
    REQUIRE(built.popups.size() == 1);
    const ui::built_popup& popup { built.popups.front() };
    // 뿌리의 id에 연 번호가 들어간다. 위에서 닫힌 카드의 영역 기록이 다음 카드의 들어옴을 막지 않는다.
    REQUIRE(popup.tree->root() != nullptr);
    CHECK(popup.tree->root()->id().owner == u8"ref-list-root-7");
    const auto* const card { popup.tree->find(ui::ref_list_card_id()) };
    REQUIRE(card != nullptr);

    luil::interaction_controller interaction {};
    interaction.set_tree(built.tree);
    interaction.set_surface_trees({ { u8"ref-list", popup.tree } });
    // 그림자 띠(카드 옆 3 px 여백)도 카드로 친다.
    REQUIRE(card->bounds().x == 3.0f);
    const auto entered { messages_of<ref_card_hover_intent>(interaction.process(luil::pointer_moved_event { .x = 1.0f, .y = 30.0f, .surface = u8"ref-list" })) };
    REQUIRE(entered.size() == 1);
    CHECK(entered.front().inside);
    // 그 자리의 누름은 popup 안의 누름이라 바깥 누름 닫힘이 서지 않는다. 그 누름도 카드를 닫는다.
    static_cast<void>(interaction.process(luil::pointer_pressed_event { .x = 1.0f, .y = 30.0f, .surface = u8"ref-list" }));
    const auto clicked { messages_of<close_ref_list_intent>(interaction.process(luil::pointer_released_event { .x = 1.0f, .y = 30.0f, .surface = u8"ref-list" })) };
    REQUIRE(clicked.size() == 1);
    CHECK_FALSE(clicked.front().pressed_outside);
    // popup이 사라지면 새 표면 목록으로 영역을 다시 재어 나감을 알린다.
    interaction.set_surface_trees({});
    const auto left { messages_of<ref_card_hover_intent>(interaction.refresh_hover_tracking()) };
    REQUIRE(left.size() == 1);
    CHECK_FALSE(left.front().inside);
}

TEST_CASE("a refs cell without an overflow chip has no hover region or dwell", "[ui][refs]")
{
    // 칩이 다 들어간 행에는 카드가 없다. 영역이나 머무름이 남으면 그 칸에 머물기만 해도 빈 카드를 열려 한다.
    using namespace cyella;
    overflow_fixture fixture {};
    const ui::built_tree built { fixture.build() };
    const std::u8string row { graph_item_key(fixture.third) };
    const luil::ui_element* const cell { built.tree->find(ui::ref_cell_id(row)) };
    REQUIRE(cell != nullptr);
    const luil::ui_element* const overflow { built.tree->find({ ui::kind_ref_overflow, u8"overflow-" + row }) };
    CHECK((overflow == nullptr || overflow->visible() == false));
    CHECK(cell->hover_region() == nullptr);
    CHECK(cell->hover_dwell() == nullptr);
    const luil::point_f inside { middle_of(cell->bounds()) };
    CHECK(built.tree->hover_regions_at(inside.x, inside.y).empty());

    luil::interaction_controller interaction {};
    interaction.set_tree(built.tree);
    const auto t0 { std::chrono::steady_clock::time_point {} + std::chrono::hours { 1 } };
    CHECK(messages_of<ref_cell_hover_intent>(interaction.process(luil::pointer_moved_event { .x = inside.x, .y = inside.y, .time = t0 })).empty());
    CHECK(messages_of<ref_cell_dwell_intent>(interaction.advance(t0 + std::chrono::seconds { 1 })).empty());
}

TEST_CASE("the ref card popup keeps presses beside its tail and scrolls its list on the wheel", "[ui][refs]")
{
    using namespace cyella;
    overflow_fixture fixture {};
    // 카드 높이를 넘겨 목록이 흐르게 참조를 넉넉히 붙인다.
    for (int index { 0 }; index < 40; ++index)
    {
        const std::string name { "extra-" + std::to_string(index) };
        (*fixture.labels)[fixture.first].push_back({ .kind = ref_label_kind::local, .text = std::u8string { name.begin(), name.end() }, .ref_name = name });
    }
    const ref_list_state open { .sha = fixture.first, .x = 100.0f, .y = 60.0f, .width = 24.0f, .height = 18.0f, .row_key = graph_item_key(fixture.first), .serial = 2 };
    const ui::built_tree built { fixture.build(open) };
    REQUIRE(built.popups.size() == 1);
    const ui::built_popup& popup { built.popups.front() };
    REQUIRE(popup.tree != nullptr);
    const luil::ui_element* const root { popup.tree->root() };
    const luil::ui_element* const card { popup.tree->find(ui::ref_list_card_id()) };
    REQUIRE(root != nullptr);
    REQUIRE(card != nullptr);

    // 카드 자리(꼬리 띠 포함)는 알파가 0이 아니다. 꼬리 옆 빈 자리가 알파 0이면 그 누름이 아래 창으로 가 카드 바로 위의 행을 고른다.
    // 그림자 밖 popup 모서리는 알파 0이라 입력이 아래 창으로 간다.
    SkBitmap bitmap {};
    const luil::rect_f surface { root->bounds() };
    bitmap.allocN32Pixels(static_cast<int>(surface.width), static_cast<int>(surface.height));
    bitmap.eraseColor(SK_ColorTRANSPARENT);
    SkCanvas canvas { bitmap };
    const luil::ui_color_palette palette { luil::color_palette_for(luil::color_theme::light) };
    luil::draw_context context { .canvas = canvas, .palette = palette, .scale = 1.0f };
    root->draw(context, luil::interaction_snapshot {});
    // 아래로 선 카드라 그 점은 꼬리 띠의 왼쪽 끝이다. 몸통·꼬리·그림자가 닿지 않아 앞치마만 칠한 자리임을 옅은 알파로 함께 본다.
    REQUIRE(built.popups.front().y > 0.0f);
    const luil::rect_f box { card->bounds() };
    const unsigned apron { SkColorGetA(bitmap.getColor(static_cast<int>(box.x) + 4, static_cast<int>(box.y) + 2)) };
    CHECK(apron > 0);
    CHECK(apron < 16);
    CHECK(SkColorGetA(bitmap.getColor(bitmap.width() - 1, bitmap.height() - 1)) == 0);

    // 카드 위의 휠은 popup 표면의 목록을 흘린다 (그래프가 아니다).
    luil::interaction_controller interaction {};
    interaction.set_tree(built.tree);
    interaction.set_surface_trees({ { u8"ref-list", popup.tree } });
    const luil::point_f middle { middle_of(box) };
    static_cast<void>(interaction.process(luil::pointer_moved_event { .x = middle.x, .y = middle.y, .surface = u8"ref-list" }));
    const auto wheeled { interaction.process(luil::mouse_wheel_event { .x = middle.x, .y = middle.y, .delta = -120.0f, .surface = u8"ref-list" }) };
    const auto scrolled { messages_of<scroll_ref_list_intent>(wheeled) };
    REQUIRE(scrolled.size() == 1);
    CHECK(scrolled.front().absolute);
    CHECK(scrolled.front().value > 0.0f);
    CHECK(messages_of<scroll_intent>(wheeled).empty());
}

TEST_CASE("the overflow chip closes a pinned card and reports expanded only while its card is open", "[ui][refs]")
{
    using namespace cyella;
    overflow_fixture fixture {};
    const std::u8string row { graph_item_key(fixture.first) };
    const luil::ui_element_id chip_id { ui::kind_ref_overflow, u8"overflow-" + row };
    const auto press_message = [&chip_id](const ui::built_tree& built) {
        const luil::ui_element* const chip { built.tree->find(chip_id) };
        REQUIRE(chip != nullptr);
        REQUIRE(chip->action(luil::ui_trigger::left_click) != nullptr);
        const std::vector<luil::input_action> actions { (*chip->action(luil::ui_trigger::left_click))({ .element = chip_id }) };
        REQUIRE(actions.size() == 1);
        const auto* const message { std::get_if<luil::app_message>(&actions.front()) };
        REQUIRE(message != nullptr);
        return *message;
    };

    // 닫혀 있으면 열기다. 펼침을 알리지 않아 보조 기술에는 Invoke가 선다.
    const ui::built_tree closed { fixture.build() };
    const luil::ui_element* const closed_chip { closed.tree->find(chip_id) };
    REQUIRE(closed_chip != nullptr);
    CHECK_FALSE(closed_chip->accessibility().expanded.has_value());
    const luil::app_message opening { press_message(closed) };
    REQUIRE(opening.get<open_ref_list_intent>() != nullptr);
    CHECK(opening.get<open_ref_list_intent>()->row_key == row);
    const std::optional<std::vector<luil::input_action>> invoked { luil::plan_access_request(*closed_chip, luil::access_request { luil::access_command::invoke }) };
    REQUIRE(invoked.has_value());
    CHECK(messages_of<open_ref_list_intent>(*invoked).size() == 1);

    // 호버로 떠 있으면 같은 열기(= 고정)이고, 펼침을 알린다.
    ref_list_state card { .sha = fixture.first, .x = 100.0f, .y = 60.0f, .width = 24.0f, .height = 18.0f, .row_key = row, .origin = ref_list_origin::hover, .serial = 1 };
    const ui::built_tree hovered { fixture.build(card) };
    CHECK(hovered.tree->find(chip_id)->accessibility().expanded == std::optional<bool> { true });
    CHECK(press_message(hovered).get<open_ref_list_intent>() != nullptr);
    // 다른 행의 "+N"은 펼치지 않는다.
    const luil::ui_element* const other { hovered.tree->find({ ui::kind_ref_overflow, u8"overflow-" + graph_item_key(fixture.second) }) };
    REQUIRE(other != nullptr);
    CHECK_FALSE(other->accessibility().expanded.has_value());

    // 누름으로 고정된 카드면 닫기다. 보조 기술의 접기도 닫기다.
    card.origin = ref_list_origin::press;
    const ui::built_tree pinned { fixture.build(card) };
    const luil::ui_element* const pinned_chip { pinned.tree->find(chip_id) };
    REQUIRE(pinned_chip != nullptr);
    CHECK(press_message(pinned).get<close_ref_list_intent>() != nullptr);
    const std::optional<std::vector<luil::input_action>> collapsed { luil::plan_access_request(*pinned_chip, luil::access_request { luil::access_command::collapse }) };
    REQUIRE(collapsed.has_value());
    CHECK(messages_of<close_ref_list_intent>(*collapsed).size() == 1);

    // 누를 때 펼쳐 있던 "+N"은 누름과 뗌 사이에 닫힌 frame이 와도 누를 때의 tree의 닫기를 뗄 때 낸다 (luil X-4).
    const luil::point_f point { middle_of(pinned_chip->bounds()) };
    luil::interaction_controller interaction {};
    interaction.set_tree(pinned.tree);
    static_cast<void>(interaction.process(luil::pointer_pressed_event { .x = point.x, .y = point.y }));
    interaction.set_tree(closed.tree);
    const auto released { interaction.process(luil::pointer_released_event { .x = point.x, .y = point.y }) };
    CHECK(messages_of<close_ref_list_intent>(released).size() == 1);
    CHECK(messages_of<open_ref_list_intent>(released).empty());

    // 닫힌 frame이 누름보다 먼저 왔어도(누름이 popup을 닫았다) 직전 tree에서 펼쳐 있던 "+N"이면 그 tree의 닫기다.
    luil::interaction_controller late {};
    late.set_tree(pinned.tree);
    late.set_tree(closed.tree);
    static_cast<void>(late.process(luil::pointer_pressed_event { .x = point.x, .y = point.y, .closed_popup = true }));
    const auto late_release { late.process(luil::pointer_released_event { .x = point.x, .y = point.y }) };
    CHECK(messages_of<close_ref_list_intent>(late_release).size() == 1);
    CHECK(messages_of<open_ref_list_intent>(late_release).empty());
}

TEST_CASE("the ref card menu takes arrow keys and Enter from its popup", "[ui][refs]")
{
    // 카드 항목은 Tab 자리가 아니다. 카드가 떠 있는 동안 ↑/↓/Enter는 luil의 메뉴 키 탐색이 popup tree의 메뉴에서 받는다.
    using namespace cyella;
    overflow_fixture fixture {};
    const ref_list_state open { .sha = fixture.first, .x = 100.0f, .y = 60.0f, .width = 24.0f, .height = 18.0f, .row_key = graph_item_key(fixture.first), .serial = 1 };
    const ui::built_tree built { fixture.build(open) };
    REQUIRE(built.popups.size() == 1);
    ui::cyella_policy policy {};
    luil::interaction_controller interaction { &policy };
    interaction.set_tree(built.tree);
    interaction.set_surface_trees({ { u8"ref-list", built.popups.front().tree } });
    static_cast<void>(interaction.process(luil::key_pressed_event { .key = luil::key_code::arrow_down }));
    CHECK(interaction.snapshot().menu_surface == u8"ref-list");
    const auto chosen { messages_of<open_context_menu_intent>(interaction.process(luil::key_pressed_event { .key = luil::key_code::enter })) };
    REQUIRE(chosen.size() == 1);
    CHECK(chosen.front().target.sha == fixture.first);
    // 메뉴는 이 frame에 선 "+N" 칩 아래다 (연 때의 사각형이 아니라 tree의 칩 자리).
    const luil::ui_element* const chip { built.tree->find({ ui::kind_ref_overflow, u8"overflow-" + graph_item_key(fixture.first) }) };
    REQUIRE(chip != nullptr);
    CHECK(chosen.front().x == chip->bounds().x);
    CHECK(chosen.front().y == chip->bounds().y + chip->bounds().height);
}

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

TEST_CASE("repository tabs shrink first, then step one tab at a time and reveal the selected tab", "[presentation][ui][tabs]")
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
    const auto* const back { find_element(end, luil::ui_element_kind::tab_step_back, u8"tabs") };
    const auto* const forward { find_element(end, luil::ui_element_kind::tab_step_forward, u8"tabs") };
    const auto* const list { find_element(end, luil::ui_element_kind::tab_overflow, u8"tabs") };
    REQUIRE(last != nullptr);
    REQUIRE(add != nullptr);
    REQUIRE(back != nullptr);
    REQUIRE(forward != nullptr);
    REQUIRE(list != nullptr);
    // 넘치면 탭은 최소 폭까지 줄어 있고, 양 끝 단추와 ▾·새 탭 단추가 선다. 고른 마지막 탭은 끝까지 흘려 드러난다.
    CHECK(back->visible());
    CHECK(forward->visible());
    CHECK(last->bounds().width == 120.0f);
    REQUIRE(end.feedback.tab_scroll.has_value());
    CHECK(*end.feedback.tab_scroll > 0.0f);
    CHECK(last->bounds().x + last->bounds().width <= forward->bounds().x);
    CHECK(add->bounds().x >= list->bounds().x + list->bounds().width);

    // ▾는 모든 탭의 목록을 지금 탭 기준으로 연다.
    const auto opened { list->action(luil::ui_trigger::left_click) };
    REQUIRE(opened != nullptr);
    const auto opened_actions { (*opened)(luil::ui_action_context { list->id(), 10.0f, 20.0f }) };
    REQUIRE(opened_actions.size() == 1);
    const auto* const open_message { std::get_if<luil::app_message>(&opened_actions.front()) };
    REQUIRE(open_message != nullptr);
    const auto* const open_intent { open_message->get<open_context_menu_intent>() };
    REQUIRE(open_intent != nullptr);
    CHECK(open_intent->target.kind == menu_target_kind::tab_list);
    CHECK(open_intent->target.tab == 8);

    // 다른 탭을 고르면 흘린 값이 그 탭을 드러내는 자리로 다듬어진다.
    view.selected_tab = 1;
    view.tab_scroll = *end.feedback.tab_scroll;
    const cyella::ui::built_tree start { cyella::ui::build_ui_tree(view) };
    CHECK(start.feedback.tab_scroll == 0.0f);

    // 줄이면 다 들어가는 수면 넘치지 않는다. 탭은 고른 폭보다 좁고, 새 탭 단추는 마지막 탭 바로 뒤다.
    view.tabs.resize(4);
    const cyella::ui::built_tree fitted { cyella::ui::build_ui_tree(view) };
    const auto* const fourth { find_element(fitted, luil::ui_element_kind::tab, u8"tab-4") };
    const auto* const fitted_add { find_element(fitted, cyella::ui::kind_button, u8"new-tab") };
    REQUIRE(fourth != nullptr);
    REQUIRE(fitted_add != nullptr);
    CHECK(fourth->bounds().width < 180.0f);
    CHECK(fourth->bounds().width >= 120.0f);
    CHECK(fitted_add->bounds().x == fourth->bounds().x + fourth->bounds().width);
    CHECK_FALSE(find_element(fitted, luil::ui_element_kind::tab_step_back, u8"tabs")->visible());
}

TEST_CASE("tab dragging marks the gap on the hovered half and places the tab there", "[presentation][ui][tabs][drag]")
{
    tree_fixture fixture {};
    view_snapshot view { fixture.harness.build() };
    view.tabs = { { .id = 1, .label = u8"하나" }, { .id = 2, .label = u8"둘" }, { .id = 3, .label = u8"셋" } };
    view.selected_tab = 2;
    const cyella::ui::built_tree built { cyella::ui::build_ui_tree(view) };
    const auto* const strip { find_element(built, cyella::ui::kind_layout, u8"repository-tabs") };
    const auto* const one { find_element(built, luil::ui_element_kind::tab, u8"tab-1") };
    const auto* const two { find_element(built, luil::ui_element_kind::tab, u8"tab-2") };
    const auto* const three { find_element(built, luil::ui_element_kind::tab, u8"tab-3") };
    REQUIRE(strip != nullptr);
    REQUIRE(one != nullptr);
    REQUIRE(two != nullptr);
    REQUIRE(three != nullptr);
    REQUIRE(one->drag() != nullptr);
    REQUIRE(three->drop() != nullptr);

    // 그리는 것은 이 줄이므로 트리의 기본 상자와 대상 강조는 꺼진 payload다.
    const luil::rect_f first { one->bounds() };
    const luil::drag_payload payload { one->drag()->make_payload({ .element = one->id(), .x = first.x + 4.0f, .y = first.y + 4.0f }) };
    CHECK(payload.custom_visual);
    CHECK(payload.suppress_drop_highlight);

    const luil::ui_color_palette palette { luil::color_palette_for(luil::color_theme::dark) };
    auto now { std::chrono::steady_clock::now() };
    // 처음 뜬 표시는 옅게 시작하므로 다 짙어진 뒤의 그림을 본다.
    const auto paint = [&](const luil::ui_element& over, const float x) {
        luil::interaction_snapshot interaction {};
        interaction.drag = luil::drag_visual { .payload = payload, .x = x, .y = first.y + first.height / 2.0f, .hovered_drop_target = over.id() };
        SkBitmap bitmap {};
        bitmap.allocN32Pixels(1440, 100);
        for (int frame { 0 }; frame < 2; ++frame)
        {
            bitmap.eraseColor(palette.window_background);
            SkCanvas canvas { bitmap };
            luil::draw_context context { .canvas = canvas, .palette = palette, .now = now };
            strip->draw(context, interaction);
            now += std::chrono::seconds { 1 };
        }
        return bitmap;
    };
    const auto placed = [&](const luil::ui_element& over, const float x) -> std::optional<reorder_tab_intent> {
        const std::vector<luil::input_action> actions { over.drop()->on_drop(payload, { .element = over.id(), .x = x, .y = first.y }) };
        if (actions.empty())
            return std::nullopt;
        const auto* const action { std::get_if<luil::app_message>(&actions.front()) };
        REQUIRE(action != nullptr);
        const auto* const intent { action->get<reorder_tab_intent>() };
        REQUIRE(intent != nullptr);
        return *intent;
    };

    const luil::rect_f box { three->bounds() };
    const int left { static_cast<int>(box.x) };
    const int right { static_cast<int>(box.x + box.width) - 1 };
    const int y { static_cast<int>(box.y + box.height / 2.0f) };
    for (const bool after : { false, true })
    {
        const float x { box.x + box.width * (after ? 0.75f : 0.25f) };
        const SkBitmap bitmap { paint(*three, x) };
        CHECK(bitmap.getColor(after ? right : left, y) == palette.accent);
        CHECK(bitmap.getColor(after ? left : right, y) != palette.accent);
        const std::optional<reorder_tab_intent> intent { placed(*three, x) };
        REQUIRE(intent.has_value());
        CHECK(intent->moved == 1);
        CHECK(intent->target == 3);
        CHECK(intent->after == after);
    }

    // 바로 옆 탭의 가까운 반쪽은 제자리라 표시도 놓기도 없다.
    const luil::rect_f beside { two->bounds() };
    const float x { beside.x + beside.width * 0.25f };
    const SkBitmap bitmap { paint(*two, x) };
    CHECK(bitmap.getColor(static_cast<int>(beside.x), y) != palette.accent);
    CHECK(bitmap.getColor(static_cast<int>(beside.x) - 1, y) != palette.accent);
    CHECK_FALSE(placed(*two, x).has_value());
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
    // 다시 지은 tree에서 판 밖에서 떼도 끝 알림이 한 번 나온다. controller가 그때 폭을 저장한다.
    const std::vector<luil::input_action> released { interaction.process(luil::pointer_released_event { .x = x + 160.0f, .y = -40.0f }) };
    const auto finished = [](const luil::input_action& action) {
        const auto* const message { std::get_if<luil::app_message>(&action) };
        return message != nullptr && message->get<cyella::finish_pane_resize_intent>() != nullptr;
    };
    CHECK(std::ranges::count_if(released, finished) == 1);
    CHECK(interaction.process(luil::pointer_moved_event { .x = x + 200.0f, .y = -40.0f }).empty());
}

TEST_CASE("pane splitters announce the end of a resize so the widths are saved on release", "[presentation][ui][resize]")
{
    // 판 폭은 손잡이를 놓는 순간 저장한다. 그 신호가 끄는 몸짓과 키 둘 다에서 나와야 한다.
    const auto finishes = [](const std::vector<luil::input_action>& actions) {
        return std::ranges::count_if(actions, [](const luil::input_action& action) {
            const auto* const message { std::get_if<luil::app_message>(&action) };
            return message != nullptr && message->get<cyella::finish_pane_resize_intent>() != nullptr;
        });
    };
    const auto check_splitter = [&finishes](const cyella::ui::built_tree& built, const std::u8string& owner, const std::u8string& access_name) {
        const std::string name { owner.begin(), owner.end() };
        INFO(name);
        const luil::ui_element* const splitter { find_element(built, cyella::ui::kind_splitter, owner) };
        REQUIRE(splitter != nullptr);
        // 손잡이는 Tab 자리다. 보조 기술에 이름 없는 Thumb로 서지 않고, 이름은 화면에 뜨지 않는 접근 이름이다 (tooltip이 아니다).
        CHECK_FALSE(access_name.empty());
        CHECK(splitter->accessibility().name == access_name);
        CHECK(splitter->tooltip().empty());
        // 앱이 `follows_pointer`를 세우려고 끌기 역할을 복사해 다시 둔다. 그 뒤에도 끝 알림이 남아 있어야 한다.
        REQUIRE(splitter->pointer_drag() != nullptr);
        REQUIRE(splitter->pointer_drag()->on_release != nullptr);
        CHECK(finishes(splitter->pointer_drag()->on_release({}, luil::drag_velocity {})) == 1);
        // 키 한 걸음은 바꾸기 뒤에 바로 끝을 알린다.
        REQUIRE(splitter->key_step() != nullptr);
        REQUIRE(splitter->key_step()->on_step != nullptr);
        const std::optional<std::vector<luil::input_action>> stepped { splitter->key_step()->on_step(luil::value_step::increase) };
        REQUIRE(stepped.has_value());
        REQUIRE(stepped->size() == 2);
        CHECK(finishes(*stepped) == 1);
        CHECK(std::get<luil::app_message>(stepped->back()).get<cyella::finish_pane_resize_intent>() != nullptr);

        const luil::rect_f box { splitter->bounds() };
        const float x { box.x + box.width / 2.0f };
        const float y { box.y + box.height / 2.0f };
        cyella::ui::cyella_policy policy {};
        luil::interaction_controller interaction { &policy };
        interaction.set_tree(built.tree);
        static_cast<void>(interaction.process(luil::pointer_pressed_event { .x = x, .y = y }));
        // 끄는 동안에는 끝을 알리지 않는다.
        CHECK(finishes(interaction.process(luil::pointer_moved_event { .x = x + 4.0f, .y = y + 4.0f })) == 0);
        CHECK(finishes(interaction.process(luil::pointer_released_event { .x = x + 4.0f, .y = y + 4.0f })) == 1);
    };

    tree_fixture fixture {};
    const cyella::ui::built_tree built { fixture.build() };
    check_splitter(built, u8"sidebar-splitter", tr(u8"tree.splitter.sidebar"));
    check_splitter(built, u8"detail-splitter", tr(u8"tree.splitter.detail"));

    view_snapshot view { fixture.harness.build() };
    REQUIRE(view.repository.has_value());
    // 작업 영역을 그래프 대신 펼친 배치에서는 같은 손잡이가 다른 이름으로 선다.
    view.graph_layout = false;
    view.repository->detail = detail_mode::working;
    view.repository->working = working_area_view {};
    check_splitter(cyella::ui::build_ui_tree(view), u8"working-splitter", tr(u8"tree.splitter.working"));
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
        // 칩을 누르면 누른 순간 그 커밋을 고른다 (`press_row_element`). 뗀 순간의 클릭은 없다.
        CHECK(dynamic_cast<const cyella::ui::press_row_element*>(chip)->pressable());
        CHECK(chip->action(luil::ui_trigger::left_click) == nullptr);
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
    CHECK(dynamic_cast<const cyella::ui::press_row_element*>(row)->pressable());
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
    // 입력 칸의 UIA 이름은 라벨 글 그대로다 (라벨은 별도 글 요소라서 칸 이름이 비어 있었다). 안내 글에는 라벨을 다시 싣지 않는다.
    REQUIRE(fixture.view.form.has_value());
    REQUIRE(fixture.view.form->fields.empty() == false);
    const std::u8string field_name { field->accessibility().name };
    CHECK_FALSE(field_name.empty());
    CHECK(field_name == fixture.view.form->fields[0].label);
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

TEST_CASE("the selector focus request names the popup surface and a focusable filter field in its tree", "[presentation][ui][selector][focus]")
{
    tree_fixture fixture {};
    // 요청만 있고 popup이 없으면 싣지 않는다. 없는 표면을 기다리다 버려지는 요청이 남는다.
    fixture.harness.ui.popup_focus = popup_focus_request { .id = 7, .target = popup_focus_target::selector_filter };
    CHECK_FALSE(fixture.build().focus.has_value());

    fixture.harness.ui.selector = selector_state { .kind = selector_kind::branch, .x = 330.0f, .y = 90.0f, .width = 200.0f, .filter = {} };
    const cyella::ui::built_tree open { fixture.build() };
    REQUIRE(open.focus.has_value());
    CHECK(open.focus->id == 7);
    REQUIRE(open.popups.size() == 1);
    const cyella::ui::built_popup& popup { open.popups.front() };
    CHECK(open.focus->surface == popup.id);
    REQUIRE(popup.tree != nullptr);
    // luil은 대상이 그 표면 tree에 서고, 초점을 받을 수 있고, 보이는 크기를 가질 때 이행한다.
    const luil::ui_element* const target { popup.tree->find(open.focus->target) };
    REQUIRE(target != nullptr);
    CHECK(target->id().kind == cyella::ui::kind_selector_filter_input);
    CHECK(target->focusable());
    CHECK(target->bounds().width > 0.0f);
    CHECK(target->bounds().height > 0.0f);

    // 다른 popup을 가리키는 요청은 선택 상자에 싣지 않는다.
    fixture.harness.ui.popup_focus = popup_focus_request { .id = 8, .target = popup_focus_target::ui_font_search };
    CHECK_FALSE(fixture.build().focus.has_value());
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
    // 창 캡션에서는 그림이 제목 글자를 대신한다. 캡션이 그리는 글자는 비고, 그림은 아이콘 칸 오른쪽에 캡션 높이 안으로 선다.
    if (luil::current_ui_platform().window_caption)
    {
        const luil::ui_element* const caption_title { find_element(titled, ui::kind_image, u8"caption-title") };
        REQUIRE(caption_title != nullptr);
        CHECK(caption_title->accessibility().name == u8"cyella");
        const luil::caption_config caption { ui::cyella_caption() };
        const luil::ui_element* const caption_symbol { find_element(titled, ui::kind_image, u8"caption-symbol") };
        if (caption_symbol != nullptr)
            CHECK(caption_title->bounds().x > caption_symbol->bounds().x + caption_symbol->bounds().width);
        CHECK(caption_title->bounds().y >= 0.0f);
        CHECK(caption_title->bounds().y + caption_title->bounds().height <= static_cast<float>(caption.metrics.height));
    }

    ui::set_app_title({}, {});
    const auto plain { ui::build_ui_tree(harness.build(1440.0f)) };
    CHECK(find_element(plain, ui::kind_image, u8"start-title") == nullptr);
    CHECK(find_element(plain, ui::kind_image, u8"caption-title") == nullptr);
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

TEST_CASE("tab status goes to the chosen spot and the repository icon stays", "[presentation][ui][tabs]")
{
    luil::tab_item badge_item {};
    cyella::ui::apply_tab_marker(badge_item, tab_marker::changes, 3, tab_marker_place::badge);
    REQUIRE(badge_item.badge.has_value());
    CHECK(badge_item.badge->text == u8"3");
    CHECK_FALSE(badge_item.indicator.has_value());

    luil::tab_item running {};
    cyella::ui::apply_tab_marker(running, tab_marker::running, 0, tab_marker_place::badge);
    REQUIRE(running.badge.has_value());
    CHECK(running.badge->spinning);
    CHECK(running.badge->tone == luil::badge_tone::accent);

    luil::tab_item corner {};
    cyella::ui::apply_tab_marker(corner, tab_marker::needs_input, 0, tab_marker_place::icon_corner);
    REQUIRE(corner.indicator.has_value());
    CHECK(corner.indicator->kind == luil::tab_indicator_kind::glyph);
    CHECK(corner.indicator->tone == luil::badge_tone::warning);
    CHECK_FALSE(corner.badge.has_value());

    luil::tab_item quiet {};
    cyella::ui::apply_tab_marker(quiet, tab_marker::none, 0, tab_marker_place::close_slot);
    CHECK_FALSE(quiet.badge.has_value());
    CHECK_FALSE(quiet.indicator.has_value());

    CHECK(cyella::ui::repository_tab_style(tab_shape::flat, tab_marker_place::close_slot).variant == luil::tab_bar_variant::flat);
    CHECK(cyella::ui::repository_tab_style(tab_shape::flat, tab_marker_place::close_slot).indicator_placement == luil::tab_indicator_placement::close_slot);
    CHECK(cyella::ui::repository_tab_style(tab_shape::connected, tab_marker_place::badge).variant == luil::tab_bar_variant::connected);

    // 탭 줄의 아이콘은 상태와 무관하게 저장소 아이콘이고, 바뀐 파일 수는 배지가 말한다.
    tree_fixture fixture {};
    view_snapshot view { fixture.harness.build() };
    view.tabs = { { .id = 1, .label = u8"하나", .marker = tab_marker::changes, .changed_files = 2 } };
    view.selected_tab = 1;
    const cyella::ui::built_tree built { cyella::ui::build_ui_tree(view) };
    const auto* const tab { find_element(built, luil::ui_element_kind::tab, u8"tab-1") };
    REQUIRE(tab != nullptr);
    CHECK(tab->accessibility().name.find(u8"2") != std::u8string::npos);
}

TEST_CASE("the all-tabs list checks the current tab and selects the chosen one", "[presentation][menu][tabs]")
{
    repository_tabs tabs {};
    const repository_tab_id first { tabs.open_repository(u8"C:\\work\\one").id };
    const repository_tab_id second { tabs.open_repository(u8"C:\\work\\two").id };
    const menu_model model { build_tab_list_menu(tabs, second) };
    CHECK(model.title == tr(u8"menu.all_tabs"));
    REQUIRE(model.entries.size() == tabs.tabs().size());
    const auto entry_of
        = [&model](const repository_tab_id id) { return std::find_if(model.entries.begin(), model.entries.end(), [id](const menu_entry& entry) { return tab_list_entry_id(entry.key) == id; }); };
    REQUIRE(entry_of(first) != model.entries.end());
    REQUIRE(entry_of(second) != model.entries.end());
    CHECK(entry_of(second)->icon == luil::codicons::icon_check);
    CHECK(entry_of(first)->icon == luil::codicons::icon_repo);
    CHECK_FALSE(tab_list_entry_id(u8"close").has_value());
    CHECK_FALSE(tab_list_entry_id(u8"tab-").has_value());
    CHECK_FALSE(tab_list_entry_id(u8"tab-3x").has_value());
}

namespace {
    // 누른 순간과 뗀 순간에 나온 메시지다. 행은 누른 순간 고르고 뗀 순간에는 다시 고르지 않는다.
    struct press_result
    {
        std::vector<luil::input_action> pressed {};
        std::vector<luil::input_action> released {};
    };

    press_result press_at(const cyella::ui::built_tree& built, const luil::ui_element& element, const bool control = false)
    {
        cyella::ui::cyella_policy policy {};
        luil::interaction_controller interaction { &policy };
        interaction.set_tree(built.tree);
        const luil::rect_f box { element.bounds() };
        const float x { box.x + box.width / 2.0f };
        const float y { box.y + box.height / 2.0f };
        press_result result {};
        result.pressed = interaction.process(luil::pointer_pressed_event { .x = x, .y = y, .control = control });
        result.released = interaction.process(luil::pointer_released_event { .x = x, .y = y });
        return result;
    }

    template<typename intent_type>
    const intent_type* intent_at(const std::vector<luil::input_action>& actions, const std::size_t index)
    {
        if (index >= actions.size())
            return nullptr;
        const auto* const message { std::get_if<luil::app_message>(&actions[index]) };
        return message == nullptr ? nullptr : message->get<intent_type>();
    }
} // namespace

TEST_CASE("graph and sidebar rows select when pressed and right-click selects an unselected row first", "[presentation][ui][select]")
{
    tree_fixture fixture {};
    const cyella::ui::built_tree built { fixture.build() };
    const std::u8string newest { graph_item_key(sha_of(5)) };
    const std::u8string oldest { graph_item_key(sha_of(0)) };
    const luil::ui_element* const row { find_element(built, cyella::ui::kind_graph_row, u8"commit-" + oldest) };
    REQUIRE(row != nullptr);
    CHECK(row->action(luil::ui_trigger::left_click) == nullptr);

    // Ctrl을 누른 채 누르면 누른 순간 Ctrl+고르기다 (여러 커밋 비교).
    const press_result pressed { press_at(built, *row, true) };
    REQUIRE(pressed.pressed.size() == 1);
    const auto* const chosen { intent_at<select_commit_intent>(pressed.pressed, 0) };
    REQUIRE(chosen != nullptr);
    CHECK(chosen->key == oldest);
    CHECK(chosen->control);
    CHECK(pressed.released.empty());

    // 고르지 않은 행의 우클릭은 그 행을 먼저 고르고 메뉴를 연다. 고른 행(가장 새 커밋)은 메뉴만이다.
    const std::vector<luil::input_action> other { (*row->action(luil::ui_trigger::right_click))(luil::ui_action_context { row->id() }) };
    REQUIRE(other.size() == 2);
    REQUIRE(intent_at<select_commit_intent>(other, 0) != nullptr);
    CHECK(intent_at<select_commit_intent>(other, 0)->key == oldest);
    CHECK(intent_at<open_context_menu_intent>(other, 1) != nullptr);
    fixture.harness.ui.tabs[fixture.tab].selected_sha = sha_of(5);
    const cyella::ui::built_tree selected { fixture.build() };
    const luil::ui_element* const head { find_element(selected, cyella::ui::kind_graph_row, u8"commit-" + newest) };
    REQUIRE(head != nullptr);
    const std::vector<luil::input_action> same { (*head->action(luil::ui_trigger::right_click))(luil::ui_action_context { head->id() }) };
    REQUIRE(same.size() == 1);
    CHECK(intent_at<open_context_menu_intent>(same, 0) != nullptr);

    // 사이드바의 브랜치 행도 누른 순간 고른다.
    const luil::ui_element* const branch { find_element(built, cyella::ui::kind_sidebar_row, u8"row-l:feature/graph") };
    REQUIRE(branch != nullptr);
    const press_result sidebar { press_at(built, *branch) };
    REQUIRE(sidebar.pressed.size() == 1);
    REQUIRE(intent_at<select_sidebar_item_intent>(sidebar.pressed, 0) != nullptr);
    CHECK(intent_at<select_sidebar_item_intent>(sidebar.pressed, 0)->key == u8"l:feature/graph");
    CHECK(sidebar.released.empty());
}

TEST_CASE("working files pick with modifiers on press and the unstaged header discards everything", "[presentation][ui][working]")
{
    tree_fixture fixture {};
    view_snapshot view { fixture.harness.build() };
    REQUIRE(view.repository.has_value());
    working_area_view working {};
    working.unstaged.push_back({ .key = u8"u:src/a.cpp", .path = "src/a.cpp", .name = u8"a.cpp", .status = 'M', .selected = true });
    working.unstaged.push_back({ .key = u8"u:src/b.cpp", .path = "src/b.cpp", .name = u8"b.cpp", .status = 'M' });
    working.unstaged.push_back({ .key = u8"u:new.txt", .path = "new.txt", .name = u8"new.txt", .status = '?', .untracked = true });
    working.selection_keys = { u8"u:src/a.cpp" };
    view.repository->detail = detail_mode::working;
    view.repository->working = working;
    const cyella::ui::built_tree built { cyella::ui::build_ui_tree(view) };
    CHECK(built.tree->duplicate_ids().empty());

    const luil::ui_element* const other { find_element(built, cyella::ui::kind_file_row, u8"file-u:src/b.cpp") };
    REQUIRE(other != nullptr);
    const press_result pressed { press_at(built, *other, true) };
    REQUIRE(pressed.pressed.size() == 1);
    const auto* const chosen { intent_at<select_file_intent>(pressed.pressed, 0) };
    REQUIRE(chosen != nullptr);
    CHECK(chosen->key == u8"u:src/b.cpp");
    CHECK(chosen->control);
    CHECK(chosen->request.path == "src/b.cpp");
    CHECK(pressed.released.empty());

    // 고른 파일의 우클릭은 선택을 둔 채 메뉴만 열고, 고르지 않은 파일은 그 파일만 고른 뒤 연다.
    const luil::ui_element* const picked { find_element(built, cyella::ui::kind_file_row, u8"file-u:src/a.cpp") };
    REQUIRE(picked != nullptr);
    const std::vector<luil::input_action> kept { (*picked->action(luil::ui_trigger::right_click))(luil::ui_action_context { picked->id() }) };
    REQUIRE(kept.size() == 1);
    CHECK(intent_at<open_context_menu_intent>(kept, 0) != nullptr);
    luil::ui_action_context with_control { other->id() };
    with_control.control = true;
    const std::vector<luil::input_action> moved { (*other->action(luil::ui_trigger::right_click))(with_control) };
    REQUIRE(moved.size() == 2);
    REQUIRE(intent_at<select_file_intent>(moved, 0) != nullptr);
    CHECK_FALSE(intent_at<select_file_intent>(moved, 0)->control);
    CHECK(intent_at<open_context_menu_intent>(moved, 1) != nullptr);

    // 스테이징 전 머리의 모두 버리기는 추적 중인 파일과 추적하지 않는 파일을 갈라 한 확인 요청으로 낸다.
    const luil::ui_element* const discard_all { find_element(built, cyella::ui::kind_button, u8"discard-all") };
    REQUIRE(discard_all != nullptr);
    CHECK(discard_all->enabled());
    REQUIRE(discard_all->action(luil::ui_trigger::left_click) != nullptr);
    const std::vector<luil::input_action> discard { (*discard_all->action(luil::ui_trigger::left_click))(luil::ui_action_context { discard_all->id() }) };
    const auto* const intent { intent_at<discard_files_intent>(discard, 0) };
    REQUIRE(intent != nullptr);
    CHECK(intent->paths == std::vector<std::string> { "src/a.cpp", "src/b.cpp" });
    CHECK(intent->untracked == std::vector<std::string> { "new.txt" });

    // 머리 줄의 글자 단추도 라벨과 동작을 갖고 선다 (그룹 머리를 짓는 builder가 캡처한 단추를 옮겨 가지 않는다).
    const luil::ui_element* const stage_all { find_element(built, cyella::ui::kind_text_button, u8"stage-all") };
    REQUIRE(stage_all != nullptr);
    CHECK(stage_all->enabled());
    CHECK(stage_all->accessibility().name == tr(u8"repo.working.stage_all"));
    CHECK(stage_all->action(luil::ui_trigger::left_click) != nullptr);

    // 파일 행을 누르면 초점이 파일 목록에 선다. ↓는 다음 파일, Enter·Space는 커서 파일(고른 a.cpp)의 diff를 연다.
    cyella::ui::cyella_policy policy {};
    luil::interaction_controller interaction { &policy };
    interaction.set_tree(built.tree);
    const luil::rect_f box { picked->bounds() };
    static_cast<void>(interaction.process(luil::pointer_pressed_event { .x = box.x + box.width / 3.0f, .y = box.y + box.height / 2.0f }));
    static_cast<void>(interaction.process(luil::pointer_released_event { .x = box.x + box.width / 3.0f, .y = box.y + box.height / 2.0f }));
    CHECK(interaction.snapshot().focused == luil::ui_element_id { luil::ui_element_kind::virtual_list, u8"working-files" });
    for (const luil::key_code key : { luil::key_code::enter, luil::key_code::space })
    {
        const std::vector<luil::input_action> opened { interaction.process(luil::key_pressed_event { .key = key }) };
        REQUIRE(intent_at<select_file_intent>(opened, 0) != nullptr);
        CHECK(intent_at<select_file_intent>(opened, 0)->request.path == "src/a.cpp");
    }
    const std::vector<luil::input_action> down { interaction.process(luil::key_pressed_event { .key = luil::key_code::arrow_down }) };
    REQUIRE(intent_at<select_file_intent>(down, 0) != nullptr);
    CHECK(intent_at<select_file_intent>(down, 0)->request.path == "src/b.cpp");
}

TEST_CASE("pressing a graph row gives the list the keyboard so arrows and Escape work", "[presentation][ui][select]")
{
    tree_fixture fixture {};
    fixture.harness.ui.tabs[fixture.tab].selected_sha = sha_of(3);
    // 단축키 표는 controller가 view에 싣는다. harness의 view에는 없어 여기서 싣는다.
    view_snapshot view { fixture.harness.build() };
    view.shortcuts = resolve_shortcut_bindings(app_settings {});
    const cyella::ui::built_tree built { cyella::ui::build_ui_tree(view) };
    const luil::ui_element* const row { find_element(built, cyella::ui::kind_graph_row, u8"commit-" + graph_item_key(sha_of(3))) };
    REQUIRE(row != nullptr);
    cyella::ui::cyella_policy policy {};
    luil::interaction_controller interaction { &policy };
    interaction.set_tree(built.tree);
    const luil::rect_f box { row->bounds() };
    static_cast<void>(interaction.process(luil::pointer_pressed_event { .x = box.x + box.width / 2.0f, .y = box.y + box.height / 2.0f }));
    static_cast<void>(interaction.process(luil::pointer_released_event { .x = box.x + box.width / 2.0f, .y = box.y + box.height / 2.0f }));
    // 행 내용을 눌러도 초점은 그래프 목록에 선다 (행 내용은 초점 자리가 아니다).
    CHECK(interaction.snapshot().focused == luil::ui_element_id { luil::ui_element_kind::virtual_list, u8"graph" });

    const std::vector<luil::input_action> down { interaction.process(luil::key_pressed_event { .key = luil::key_code::arrow_down }) };
    const auto* const moved { intent_at<select_commit_intent>(down, 0) };
    REQUIRE(moved != nullptr);
    CHECK(moved->key == graph_item_key(sha_of(2)));

    const std::vector<luil::input_action> escape { interaction.process(luil::key_pressed_event { .key = luil::key_code::escape }) };
    const auto* const closed { intent_at<run_command_intent>(escape, 0) };
    REQUIRE(closed != nullptr);
    CHECK(closed->id == "view.close");

    // 그래프의 Space·Enter는 아무것도 하지 않는다 (체크아웃은 두 번 누름·메뉴의 것이다).
    CHECK(interaction.process(luil::key_pressed_event { .key = luil::key_code::enter }).empty());
    CHECK(interaction.process(luil::key_pressed_event { .key = luil::key_code::space }).empty());
}

TEST_CASE("shift+arrows and dragging over graph rows extend the selection", "[presentation][ui][select]")
{
    tree_fixture fixture {};
    fixture.harness.ui.tabs[fixture.tab].selected_sha = sha_of(3);
    view_snapshot view { fixture.harness.build() };
    view.shortcuts = resolve_shortcut_bindings(app_settings {});
    const cyella::ui::built_tree built { cyella::ui::build_ui_tree(view) };
    const luil::ui_element* const row { find_element(built, cyella::ui::kind_graph_row, u8"commit-" + graph_item_key(sha_of(3))) };
    REQUIRE(row != nullptr);
    cyella::ui::cyella_policy policy {};
    luil::interaction_controller interaction { &policy };
    interaction.set_tree(built.tree);
    const luil::rect_f box { row->bounds() };
    const float x { box.x + box.width / 2.0f };
    const float y { box.y + box.height / 2.0f };

    SECTION("shift+arrow is a shift+click on the next row and ctrl+arrow leaves the selection alone")
    {
        static_cast<void>(interaction.process(luil::pointer_pressed_event { .x = x, .y = y }));
        static_cast<void>(interaction.process(luil::pointer_released_event { .x = x, .y = y }));
        const std::vector<luil::input_action> extended { interaction.process(luil::key_pressed_event { .key = luil::key_code::arrow_down, .shift = true }) };
        const auto* const range { intent_at<select_commit_intent>(extended, 0) };
        REQUIRE(range != nullptr);
        CHECK(range->key == graph_item_key(sha_of(2)));
        CHECK(range->shift);
        CHECK_FALSE(range->control);
        const std::vector<luil::input_action> kept { interaction.process(luil::key_pressed_event { .key = luil::key_code::arrow_down, .control = true }) };
        CHECK(intent_at<select_commit_intent>(kept, 0) == nullptr);
    }

    SECTION("dragging past the start distance sweeps with the pointer position")
    {
        const std::vector<luil::input_action> pressed { interaction.process(luil::pointer_pressed_event { .x = x, .y = y }) };
        REQUIRE(intent_at<select_commit_intent>(pressed, 0) != nullptr);
        // 끌기 거리 안의 이동은 아직 누름이다.
        CHECK(intent_at<graph_sweep_intent>(interaction.process(luil::pointer_moved_event { .x = x, .y = y + 2.0f }), 0) == nullptr);
        const float below { y + box.height * 2.0f };
        const std::vector<luil::input_action> moved { interaction.process(luil::pointer_moved_event { .x = x, .y = below }) };
        const graph_sweep_intent* sweep { nullptr };
        for (std::size_t index { 0 }; index < moved.size() && sweep == nullptr; ++index)
            sweep = intent_at<graph_sweep_intent>(moved, index);
        REQUIRE(sweep != nullptr);
        CHECK(sweep->y == below);
        // 끌기 뒤의 뗌은 클릭이 아니다 (다시 고르지 않는다).
        CHECK(intent_at<select_commit_intent>(interaction.process(luil::pointer_released_event { .x = x, .y = below }), 0) == nullptr);
    }
}

TEST_CASE("the graph header divides its columns at the row width so author and date titles sit above their cells", "[presentation][ui][graph]")
{
    // 커밋이 적으면 막대가 서지 않아 행이 머리줄과 같은 폭이고, 목록 높이를 넘으면 행이 막대 폭만큼 좁아진다.
    for (const std::size_t count : { std::size_t { 6 }, std::size_t { 120 } })
    {
        CAPTURE(count);
        tree_fixture fixture {};
        load(*fixture.harness.sessions["repo-1"], linear_history(count), 0);
        const ui::built_tree built { fixture.build() };
        const auto* const header { dynamic_cast<const ui::graph_header_element*>(find_element(built, ui::kind_graph_header, u8"graph-header")) };
        REQUIRE(header != nullptr);
        const std::vector<luil::ui_element_id> rows { built.tree->ids_of_kind(ui::kind_graph_row) };
        REQUIRE(rows.empty() == false);
        const luil::ui_element* const row { built.tree->find(rows.front()) };
        REQUIRE(row != nullptr);
        const float columns { header->columns_width(1.0f) };
        CHECK(std::abs(header->bounds().x - row->bounds().x) < 0.01f);
        CHECK(std::abs(columns - row->bounds().width) < 0.01f);
        if (count > 100)
            CHECK(columns < header->bounds().width);
        else
            CHECK(columns == header->bounds().width);
    }
}

TEST_CASE("the graph list shares the cached model instead of copying it every frame", "[presentation][ui][graph]")
{
    tree_fixture fixture {};
    const ui::built_tree built { fixture.build() };
    REQUIRE(fixture.view.repository.has_value());
    const graph_view& graph { fixture.view.repository->graph };
    REQUIRE(graph.items != nullptr);
    const auto* const list { dynamic_cast<const luil::virtual_list_element*>(built.tree->find({ luil::ui_element_kind::virtual_list, u8"graph" })) };
    REQUIRE(list != nullptr);
    CHECK(list->model().data() == graph.items->data());
}

TEST_CASE("narrow rail buttons are named after their section and not only by the count", "[presentation][ui][rail]")
{
    tree_fixture fixture {};
    const ui::built_tree built { fixture.build(480.0f) };
    const luil::ui_element* const local { find_element(built, ui::kind_tool_button, u8"rail-로컬 브랜치") };
    const luil::ui_element* const tags { find_element(built, ui::kind_tool_button, u8"rail-태그") };
    REQUIRE(local != nullptr);
    REQUIRE(tags != nullptr);
    const std::u8string local_name { local->accessibility().name };
    const std::u8string tags_name { tags->accessibility().name };
    CHECK(local_name.find(tr(u8"repo.section.local")) != std::u8string::npos);
    CHECK(tags_name.find(tr(u8"repo.section.tags")) != std::u8string::npos);
    CHECK(local_name != tags_name);
}

TEST_CASE("the branch selector list hangs from the bottom edge of its pill at every scale", "[presentation][ui][selector]")
{
    for (const float scale : { 1.0f, 2.0f })
    {
        CAPTURE(scale);
        tree_fixture fixture {};
        view_snapshot view { fixture.harness.build() };
        // harness는 배율 1로 짓는다. 같은 논리 크기를 물리 픽셀로 키워 배율만 바꾼다.
        view.scale = scale;
        view.width = view.logical_width * scale;
        view.height = view.logical_height * scale;
        const ui::built_tree built { ui::build_ui_tree(view) };
        const luil::ui_element* const box { find_element(built, ui::kind_selector, u8"branch-selector") };
        REQUIRE(box != nullptr);
        const luil::ui_action* const click { box->action(luil::ui_trigger::left_click) };
        REQUIRE(click != nullptr);
        const std::vector<luil::input_action> actions { (*click)(luil::ui_action_context {}) };
        const auto* const intent { intent_at<open_selector_intent>(actions, 0) };
        REQUIRE(intent != nullptr);
        // 알약의 아래 끝은 상자 바닥에서 5 논리 픽셀 위다. 물리 픽셀로는 배율을 곱한 만큼이다.
        CHECK(intent->y == box->bounds().y + box->bounds().height - 5.0f * scale);
    }
}

TEST_CASE("file and commit rows tell assistive technology what their picture shows", "[presentation][ui][access]")
{
    tree_fixture fixture {};
    view_snapshot view { fixture.harness.build() };
    REQUIRE(view.repository.has_value());
    working_area_view working {};
    working.conflicts.push_back({ .key = u8"c:clash.ts", .path = "clash.ts", .name = u8"clash.ts", .status = 'U', .conflicted = true });
    working.unstaged.push_back({ .key = u8"u:note.ts", .path = "note.ts", .name = u8"note.ts", .status = 'M', .added = u8"+3", .removed = u8"-1" });
    working.unstaged.push_back({ .key = u8"u:new.txt", .path = "new.txt", .name = u8"new.txt", .status = '?', .untracked = true });
    working.staged.push_back({ .key = u8"s:made.txt", .path = "made.txt", .name = u8"made.txt", .status = 'A', .side = change_side::staged });
    view.repository->detail = detail_mode::working;
    view.repository->working = working;
    const ui::built_tree built { ui::build_ui_tree(view) };
    const auto name_of = [&built](const std::u8string& key) {
        const luil::ui_element* const row { find_element(built, ui::kind_file_row, u8"file-" + key) };
        REQUIRE(row != nullptr);
        return row->accessibility().name;
    };
    const auto contains = [](const std::u8string& text, const std::u8string& part) { return text.find(part) != std::u8string::npos; };

    const std::u8string modified { name_of(u8"u:note.ts") };
    CHECK(modified.starts_with(u8"note.ts"));
    CHECK(contains(modified, tr(u8"diff_viewer.status_modified")));
    CHECK(contains(modified, u8"+3"));
    CHECK(contains(modified, u8"-1"));
    // 충돌 파일은 일반 파일과 다르게 읽힌다.
    const std::u8string conflicted { name_of(u8"c:clash.ts") };
    CHECK(contains(conflicted, tr(u8"list.file_status_conflicted")));
    CHECK_FALSE(contains(conflicted, tr(u8"diff_viewer.status_modified")));
    CHECK(contains(name_of(u8"u:new.txt"), tr(u8"list.file_status_untracked")));
    CHECK(contains(name_of(u8"s:made.txt"), tr(u8"diff_viewer.status_added")));

    // 그래프 행은 제목 뒤에 작성자와 시각까지 읽는다.
    const ui::built_tree history { fixture.build() };
    const luil::ui_element* const commit { find_element(history, ui::kind_graph_row, u8"commit-" + graph_item_key(sha_of(2))) };
    REQUIRE(commit != nullptr);
    const std::u8string commit_name { commit->accessibility().name };
    CHECK(commit_name.starts_with(u8"commit 3"));
    CHECK(contains(commit_name, u8"Morgan, "));
}

TEST_CASE("tabs expand to four spaces before text is measured or drawn", "[presentation][ui][text]")
{
    CHECK(ui::expand_tabs(u8"a\tb") == u8"a    b");
    CHECK(ui::expand_tabs(u8"\tfile.txt") == u8"    file.txt");
    CHECK(ui::expand_tabs(u8"first\r\nsecond") == u8"first\nsecond");
    CHECK(ui::expand_tabs(u8"plain text") == u8"plain text");
    CHECK(ui::expand_tabs(std::u8string {}).empty());

    // 그리는 글이 곧 이름이다. 줄 나누기 측정도 같은 글로 한다.
    const auto body { ui::make_wrapped_text(u8"body", u8"\tindented code", ui::text_style {}, 18.0f) };
    CHECK(body->accessibility().name == u8"    indented code");
    const auto detail { ui::make_text(u8"detail", u8"\tfile.txt", ui::text_style {}) };
    CHECK(detail->accessibility().name == u8"    file.txt");
}

TEST_CASE("the rebase todo list scrolls to absolute positions inside its range even after the wheel ran past the end", "[presentation][ui][rebase]")
{
    rebase_editor_view view {};
    view.title = u8"rebase";
    view.phase = rebase_editor_phase::editing;
    for (std::size_t index { 0 }; index < 30; ++index)
    {
        rebase_row_view row {};
        row.key = u8"rebase-row:" + ui::number_text(index);
        row.number = index + 1;
        row.subject = u8"commit " + ui::number_text(index + 1);
        row.label = ui::number_text(index + 1) + u8"  pick  " + row.subject;
        view.rows.push_back(std::move(row));
    }
    // 앱 값이 끝을 한참 넘은 채로 짓는다 (휠을 끝 너머로 굴려 쌓인 상태). 화면은 끝에 붙는다.
    view.list_scroll = 100000.0f;
    const auto tree { luil::make_arranged_tree(ui::build_rebase_editor(view, 1280.0f, 800.0f), { 0.0f, 0.0f, 1280.0f, 800.0f }, 1.0f) };
    const auto* const list { dynamic_cast<const luil::virtual_list_element*>(tree.find({ luil::ui_element_kind::virtual_list, u8"rebase-todo" })) };
    REQUIRE(list != nullptr);
    const float maximum { list->metrics().maximum_scroll };
    REQUIRE(maximum > 0.0f);
    CHECK(list->metrics().scroll_offset == maximum);

    const luil::rect_f box { list->bounds() };
    const float x { box.x + box.width / 2.0f };
    const float y { box.y + box.height / 2.0f };
    const auto wheel = [&](const float delta) {
        const std::vector<luil::input_action> actions { luil::route_wheel(tree, x, y, delta) };
        const auto* const intent { intent_at<rebase_scroll_intent>(actions, 0) };
        REQUIRE(intent != nullptr);
        return *intent;
    };
    // 위로 한 칸은 쌓인 값이 아니라 화면이 선 끝에서 한 칸 올라간 절대 값이다.
    const rebase_scroll_intent up { wheel(-18.0f) };
    CHECK(up.absolute);
    CHECK_FALSE(up.side);
    CHECK(up.value == maximum - 18.0f);
    // 끝 너머로 더 굴려도 끝을 넘지 않고, 처음 너머로도 0 밑으로 가지 않는다.
    CHECK(wheel(5000.0f).value == maximum);
    CHECK(wheel(-100000.0f).value == 0.0f);
}
