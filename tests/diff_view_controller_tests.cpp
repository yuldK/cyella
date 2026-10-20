#include "application/diff_view_controller.h"
#include "domain/patch_builder.h"
#include "infrastructure/git_diff_parser.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <initializer_list>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {
    using namespace cyella;
    using json = nlohmann::json;

    std::string join_lines(const std::initializer_list<std::string_view> lines)
    {
        std::string text {};
        for (const std::string_view line : lines)
        {
            text.append(line);
            text.push_back('\n');
        }
        return text;
    }

    // 줄 위치: 0 a, 1 -b, 2 +B, 3 +B2, 4 c, 5 d, 6 -e, 7 +E, 8 f, 9 g
    std::string modified_text(const std::string_view path = "f.txt")
    {
        const std::string name { path };
        return join_lines({
            "diff --git a/" + name + " b/" + name,
            "--- a/" + name,
            "+++ b/" + name,
            "@@ -1,7 +1,8 @@",
            " a",
            "-b",
            "+B",
            "+B2",
            " c",
            " d",
            "-e",
            "+E",
            " f",
            " g",
        });
    }

    file_diff make_diff(const std::string& text, const std::string& path, const diff_view view = diff_view::unstaged, const bool untracked = false)
    {
        file_diff diff {};
        diff.request.path = path;
        diff.request.view = view;
        diff.request.untracked = untracked;
        diff.files = parse_git_diff(text);
        return diff;
    }

    file_diff modified_diff(const diff_view view = diff_view::unstaged)
    {
        return make_diff(modified_text(), "f.txt", view);
    }

    const std::string ready_message { R"({"type":"ready","protocol":1})" };

    // 페이지가 준비된 컨트롤러다. 준비 직후의 첫 메시지(clear)는 소비해 둔다.
    struct page
    {
        diff_view_controller controller;

        explicit page(diff_view_options options = {})
            : controller { options }
        {
            REQUIRE(controller.handle_web_message(ready_message).accepted);
            REQUIRE(controller.poll());
        }

        json latest() const
        {
            return json::parse(controller.latest_message());
        }

        // 지금 보이는 diff의 seq를 실어 동작 메시지를 만든다.
        std::string action(json message) const
        {
            message["seq"] = controller.content_seq();
            return message.dump();
        }
    };

    template<typename effect_type>
    const effect_type& effect_of(const web_message_result& result)
    {
        REQUIRE(result.effects.size() == 1);
        REQUIRE(std::holds_alternative<effect_type>(result.effects.front()));
        return std::get<effect_type>(result.effects.front());
    }
} // namespace

TEST_CASE("nothing is posted until the page says it is ready", "[diff][controller]")
{
    diff_view_controller controller {};
    CHECK_FALSE(controller.poll());
    static_cast<void>(controller.show(modified_diff(), u8"C:\\repo"));
    CHECK_FALSE(controller.poll());
    CHECK(controller.post_revision() == 0);

    CHECK(controller.handle_web_message(ready_message).accepted);
    CHECK(controller.page_ready());
    REQUIRE(controller.poll());
    CHECK(controller.post_revision() == 1);
    const json message { json::parse(controller.latest_message()) };
    CHECK(message.at("type") == "show_diff");
    CHECK_FALSE(controller.poll());
}

TEST_CASE("a ready page with nothing to show receives a clear carrying the view state", "[diff][controller]")
{
    diff_view_controller controller {};
    controller.set_layout(diff_layout::split);
    REQUIRE(controller.handle_web_message(ready_message).accepted);
    REQUIRE(controller.poll());
    const json message { json::parse(controller.latest_message()) };
    CHECK(message.at("type") == "clear");
    CHECK(message.at("layout") == "split");
    CHECK(message.at("seq") == 1);
}

TEST_CASE("every message has a larger seq than the one before", "[diff][controller]")
{
    page ui {};
    std::uint64_t previous { ui.latest().at("seq").get<std::uint64_t>() };

    static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
    REQUIRE(ui.controller.poll());
    CHECK(ui.latest().at("seq").get<std::uint64_t>() > previous);
    previous = ui.latest().at("seq").get<std::uint64_t>();
    CHECK(ui.controller.content_seq() == previous);

    ui.controller.set_layout(diff_layout::split);
    REQUIRE(ui.controller.poll());
    CHECK(ui.latest().at("type") == "set_layout");
    CHECK(ui.latest().at("seq").get<std::uint64_t>() > previous);
    // set_layout은 diff 메시지가 아니므로 동작이 되돌려야 하는 seq는 그대로다.
    CHECK(ui.controller.content_seq() == previous);
}

TEST_CASE("changes in one frame are folded into the single latest message", "[diff][controller]")
{
    page ui {};
    SECTION("a new diff carries layout and theme with it")
    {
        diff_theme theme {};
        theme.mode = diff_theme_mode::light;
        ui.controller.set_theme(theme);
        ui.controller.set_layout(diff_layout::split);
        static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
        REQUIRE(ui.controller.poll());
        const json message { ui.latest() };
        CHECK(message.at("type") == "show_diff");
        CHECK(message.at("layout") == "split");
        CHECK(message.at("theme").at("mode") == "light");
        CHECK_FALSE(ui.controller.poll());
    }

    SECTION("a single layout change is a small message")
    {
        static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
        REQUIRE(ui.controller.poll());
        ui.controller.set_layout(diff_layout::split);
        REQUIRE(ui.controller.poll());
        CHECK(ui.latest() == json::parse(R"({"type":"set_layout","seq":3,"layout":"split"})"));
    }

    SECTION("a single theme change is a small message")
    {
        static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
        REQUIRE(ui.controller.poll());
        diff_theme theme {};
        theme.mode = diff_theme_mode::light;
        ui.controller.set_theme(theme);
        REQUIRE(ui.controller.poll());
        CHECK(ui.latest().at("type") == "set_theme");
    }

    SECTION("two state changes at once resend the content so neither is lost")
    {
        static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
        REQUIRE(ui.controller.poll());
        const std::uint64_t before { ui.controller.content_seq() };
        diff_theme theme {};
        theme.mode = diff_theme_mode::light;
        ui.controller.set_theme(theme);
        ui.controller.set_layout(diff_layout::split);
        REQUIRE(ui.controller.poll());
        const json message { ui.latest() };
        CHECK(message.at("type") == "show_diff");
        CHECK(message.at("layout") == "split");
        CHECK(message.at("theme").at("mode") == "light");
        CHECK(ui.controller.content_seq() > before);
    }

    SECTION("setting the same value again changes nothing")
    {
        ui.controller.set_layout(diff_layout::inline_view);
        ui.controller.set_theme(diff_theme {});
        ui.controller.set_image_mode(image_compare_mode::side_by_side);
        CHECK_FALSE(ui.controller.poll());
    }
}

TEST_CASE("a page that reloads gets the whole state again", "[diff][controller]")
{
    page ui {};
    static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
    REQUIRE(ui.controller.poll());
    const std::uint64_t old_seq { ui.controller.content_seq() };

    ui.controller.page_reloading();
    CHECK_FALSE(ui.controller.page_ready());
    ui.controller.set_layout(diff_layout::split);
    CHECK_FALSE(ui.controller.poll());

    REQUIRE(ui.controller.handle_web_message(ready_message).accepted);
    REQUIRE(ui.controller.poll());
    CHECK(ui.latest().at("type") == "show_diff");
    CHECK(ui.latest().at("layout") == "split");
    CHECK(ui.controller.content_seq() > old_seq);
}

TEST_CASE("ready with a different protocol version is refused", "[diff][controller]")
{
    diff_view_controller controller {};
    const web_message_result result = controller.handle_web_message(R"({"type":"ready","protocol":2})");
    CHECK_FALSE(result.accepted);
    CHECK_FALSE(controller.page_ready());
}

TEST_CASE("staging a hunk produces the apply request exactly once", "[diff][controller]")
{
    page ui {};
    static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
    REQUIRE(ui.controller.poll());
    CHECK(ui.latest().at("can_stage") == true);
    CHECK(ui.latest().at("can_unstage") == false);

    const web_message_result result = ui.controller.handle_web_message(ui.action({ { "type", "stage_hunk" }, { "hunk", 0 } }));
    REQUIRE(result.accepted);
    const apply_patch_effect& effect { effect_of<apply_patch_effect>(result) };
    CHECK(effect.request.cached);
    CHECK_FALSE(effect.request.reverse);
    CHECK(effect.request.recount);
    CHECK(effect.whole_hunk);
    CHECK(effect.command == diff_hunk_command::stage);
    CHECK(effect.request.patch == *build_hunk_patch(modified_diff().files.front(), 0, patch_direction::stage));

    // 결과를 받기 전에 같은 동작이 또 오면 거절한다.
    const web_message_result again = ui.controller.handle_web_message(ui.action({ { "type", "stage_hunk" }, { "hunk", 0 } }));
    CHECK_FALSE(again.accepted);
    CHECK(again.effects.empty());

    // 적용에 실패하면 다시 받고, 웹뷰의 잠긴 단추를 풀도록 같은 diff를 새 seq로 다시 보낸다.
    const std::uint64_t before_failure { ui.controller.content_seq() };
    ui.controller.action_failed();
    REQUIRE(ui.controller.poll());
    CHECK(ui.controller.content_seq() > before_failure);
    CHECK(ui.controller.handle_web_message(ui.action({ { "type", "stage_hunk" }, { "hunk", 0 } })).accepted);

    // 새 diff를 읽으면 새 seq로 다시 받는다.
    static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
    REQUIRE(ui.controller.poll());
    CHECK(ui.controller.handle_web_message(ui.action({ { "type", "stage_hunk" }, { "hunk", 0 } })).accepted);
}

TEST_CASE("line selections become a partial patch for the matching git apply mode", "[diff][controller]")
{
    SECTION("stage lines")
    {
        page ui {};
        static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
        REQUIRE(ui.controller.poll());
        // 구간 [1,2]는 -b, +B이고, 구간 [4,6]의 context는 건너뛰고 -e만 고른다.
        const web_message_result result
            = ui.controller.handle_web_message(ui.action({ { "type", "stage_lines" }, { "hunk", 0 }, { "ranges", json::array({ json::array({ 1, 2 }), json::array({ 4, 6 }) }) } }));
        REQUIRE(result.accepted);
        const apply_patch_effect& effect { effect_of<apply_patch_effect>(result) };
        CHECK_FALSE(effect.whole_hunk);
        CHECK(effect.request.patch == *build_line_patch(modified_diff().files.front(), 0, { 1, 2, 6 }, patch_direction::stage));
        CHECK(effect.request.cached);
        CHECK_FALSE(effect.request.reverse);
    }

    SECTION("unstage lines uses --cached -R")
    {
        page ui {};
        static_cast<void>(ui.controller.show(modified_diff(diff_view::staged), u8"C:\\repo"));
        REQUIRE(ui.controller.poll());
        CHECK(ui.latest().at("can_unstage") == true);
        CHECK(ui.latest().at("can_stage") == false);
        const web_message_result result = ui.controller.handle_web_message(ui.action({ { "type", "unstage_lines" }, { "hunk", 0 }, { "ranges", json::array({ json::array({ 2, 2 }) }) } }));
        REQUIRE(result.accepted);
        const apply_patch_effect& effect { effect_of<apply_patch_effect>(result) };
        CHECK(effect.request.cached);
        CHECK(effect.request.reverse);
        CHECK(effect.request.patch == *build_line_patch(modified_diff().files.front(), 0, { 2 }, patch_direction::unstage));
    }

    SECTION("discard lines uses -R on the worktree")
    {
        page ui {};
        static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
        REQUIRE(ui.controller.poll());
        const web_message_result result = ui.controller.handle_web_message(ui.action({ { "type", "discard_hunk" }, { "hunk", 0 } }));
        REQUIRE(result.accepted);
        const apply_patch_effect& effect { effect_of<apply_patch_effect>(result) };
        CHECK_FALSE(effect.request.cached);
        CHECK(effect.request.reverse);
    }
}

TEST_CASE("actions the current view does not allow are refused", "[diff][controller]")
{
    const auto accepted = [](diff_view_controller& controller, const std::string& type) {
        json message { { "type", type }, { "seq", controller.content_seq() }, { "hunk", 0 } };
        return controller.handle_web_message(message.dump()).accepted;
    };

    SECTION("unstaged: stage and discard only")
    {
        page ui {};
        static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
        REQUIRE(ui.controller.poll());
        CHECK_FALSE(accepted(ui.controller, "unstage_hunk"));
        CHECK(accepted(ui.controller, "discard_hunk"));
    }

    SECTION("staged: unstage only")
    {
        page ui {};
        static_cast<void>(ui.controller.show(modified_diff(diff_view::staged), u8"C:\\repo"));
        REQUIRE(ui.controller.poll());
        CHECK_FALSE(accepted(ui.controller, "stage_hunk"));
        CHECK_FALSE(accepted(ui.controller, "discard_hunk"));
        CHECK(accepted(ui.controller, "unstage_hunk"));
    }

    SECTION("commit: nothing")
    {
        page ui {};
        static_cast<void>(ui.controller.show(modified_diff(diff_view::commit), u8"C:\\repo"));
        REQUIRE(ui.controller.poll());
        CHECK(ui.latest().at("can_stage") == false);
        CHECK(ui.latest().at("can_unstage") == false);
        CHECK(ui.latest().at("can_discard") == false);
        for (const char* type : { "stage_hunk", "unstage_hunk", "discard_hunk" })
            CHECK_FALSE(accepted(ui.controller, type));
    }

    SECTION("untracked file: stage but never discard")
    {
        page ui {};
        const std::string text = join_lines({
            "diff --git a/n.txt b/n.txt",
            "new file mode 100644",
            "--- /dev/null",
            "+++ b/n.txt",
            "@@ -0,0 +1,2 @@",
            "+one",
            "+two",
        });
        static_cast<void>(ui.controller.show(make_diff(text, "n.txt", diff_view::unstaged, true), u8"C:\\repo"));
        REQUIRE(ui.controller.poll());
        CHECK(ui.latest().at("can_stage") == true);
        CHECK(ui.latest().at("can_discard") == false);
        CHECK_FALSE(accepted(ui.controller, "discard_hunk"));
        CHECK(accepted(ui.controller, "stage_hunk"));
    }
}

TEST_CASE("stale or invalid actions never produce work", "[diff][controller]")
{
    page ui {};
    static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
    REQUIRE(ui.controller.poll());
    const std::uint64_t seq { ui.controller.content_seq() };

    const auto hunk_message = [](const std::uint64_t value, const std::size_t hunk) { return json({ { "type", "stage_hunk" }, { "seq", value }, { "hunk", hunk } }).dump(); };
    CHECK_FALSE(ui.controller.handle_web_message(hunk_message(seq + 1, 0)).accepted);
    CHECK_FALSE(ui.controller.handle_web_message(hunk_message(seq - 1, 0)).accepted);
    // hunk가 하나뿐이다.
    CHECK_FALSE(ui.controller.handle_web_message(hunk_message(seq, 1)).accepted);
    CHECK_FALSE(ui.controller.handle_web_message(hunk_message(seq, 999999)).accepted);

    const auto lines_message = [seq](const json& ranges) { return json({ { "type", "stage_lines" }, { "seq", seq }, { "hunk", 0 }, { "ranges", ranges } }).dump(); };
    // 범위가 hunk 밖이다.
    CHECK_FALSE(ui.controller.handle_web_message(lines_message(json::array({ json::array({ 0, 100 }) }))).accepted);
    // context만 덮는다.
    const web_message_result only_context = ui.controller.handle_web_message(lines_message(json::array({ json::array({ 4, 5 }) })));
    CHECK_FALSE(only_context.accepted);
    CHECK(only_context.effects.empty());
    // 거절된 동작은 다음 동작을 막지 않는다.
    CHECK(ui.controller.handle_web_message(hunk_message(seq, 0)).accepted);

    CHECK_FALSE(ui.controller.handle_web_message("not json").accepted);
    CHECK_FALSE(ui.controller.handle_web_message("{}").accepted);
}

TEST_CASE("a diff over the line limit is cut and offers the full view", "[diff][controller]")
{
    diff_view_options options {};
    options.line_limit = 5;
    page ui { options };
    static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
    REQUIRE(ui.controller.poll());

    json cut { ui.latest() };
    CHECK(cut.at("truncated") == true);
    CHECK(cut.at("total_lines") == 10);
    CHECK(cut.at("shown_lines") == 5);
    // 잘린 diff에서는 patch가 보이지 않는 줄을 건드릴 수 있어 막는다.
    CHECK(cut.at("can_stage") == false);
    CHECK(ui.controller.truncated());
    CHECK_FALSE(ui.controller.handle_web_message(ui.action({ { "type", "stage_hunk" }, { "hunk", 0 } })).accepted);

    REQUIRE(ui.controller.handle_web_message(ui.action({ { "type", "show_full" } })).accepted);
    REQUIRE(ui.controller.poll());
    const json full { ui.latest() };
    CHECK(full.at("truncated") == false);
    CHECK(full.at("shown_lines") == 10);
    CHECK(full.at("can_stage") == true);

    // 같은 파일을 다시 읽어도 전체 보기가 유지되고, 다른 파일로 가면 풀린다.
    static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
    CHECK_FALSE(ui.controller.truncated());
    static_cast<void>(ui.controller.show(make_diff(modified_text("other.txt"), "other.txt"), u8"C:\\repo"));
    CHECK(ui.controller.truncated());
}

TEST_CASE("show_full is refused when nothing was cut", "[diff][controller]")
{
    page ui {};
    static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
    REQUIRE(ui.controller.poll());
    CHECK_FALSE(ui.controller.handle_web_message(ui.action({ { "type", "show_full" } })).accepted);
}

TEST_CASE("the page's own layout choice is remembered without a reply", "[diff][controller]")
{
    page ui {};
    static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
    REQUIRE(ui.controller.poll());
    REQUIRE(ui.controller.handle_web_message(R"({"type":"layout_changed","layout":"split"})").accepted);
    CHECK(ui.controller.state().layout == diff_layout::split);
    CHECK_FALSE(ui.controller.poll());
    // 같은 값을 C++이 정해도 보낼 것이 없다.
    ui.controller.set_layout(diff_layout::split);
    CHECK_FALSE(ui.controller.poll());
    ui.controller.set_layout(diff_layout::inline_view);
    CHECK(ui.controller.poll());
}

TEST_CASE("open external and copy become effects for the caller", "[diff][controller]")
{
    page ui {};
    static_cast<void>(ui.controller.show(modified_diff(diff_view::staged), u8"C:\\repo"));
    REQUIRE(ui.controller.poll());

    const web_message_result opened = ui.controller.handle_web_message(ui.action({ { "type", "open_external" } }));
    REQUIRE(opened.accepted);
    const open_external_effect& open { effect_of<open_external_effect>(opened) };
    CHECK(open.plan.repository_path == u8"C:\\repo");
    CHECK(open.plan.view == diff_view::staged);
    CHECK(open.plan.old_path == "f.txt");
    CHECK(open.plan.new_path == "f.txt");

    const web_message_result copied = ui.controller.handle_web_message(ui.action({ { "type", "copy" }, { "text", "a\nb" } }));
    REQUIRE(copied.accepted);
    CHECK(effect_of<copy_text_effect>(copied).text == "a\nb");

    // 오래된 seq의 copy는 받지 않는다.
    json stale { { "type", "copy" }, { "seq", ui.controller.content_seq() - 1 }, { "text", "x" } };
    CHECK_FALSE(ui.controller.handle_web_message(stale.dump()).accepted);
}

TEST_CASE("a diff with no changes shows an empty file", "[diff][controller]")
{
    page ui {};
    file_diff diff {};
    diff.request.path = "same.txt";
    static_cast<void>(ui.controller.show(diff, u8"C:\\repo"));
    REQUIRE(ui.controller.poll());
    CHECK(ui.latest().at("type") == "show_diff");
    CHECK(ui.latest().at("file").at("new_path") == "same.txt");
    CHECK(ui.latest().at("file").at("hunks").empty());
}

TEST_CASE("clear empties the viewer", "[diff][controller]")
{
    page ui {};
    static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
    REQUIRE(ui.controller.poll());
    const std::uint64_t seq { ui.controller.content_seq() };
    ui.controller.clear();
    REQUIRE(ui.controller.poll());
    CHECK(ui.latest().at("type") == "clear");
    // 비운 뒤에는 옛 diff에 대한 동작을 받지 않는다.
    json stale { { "type", "stage_hunk" }, { "seq", seq }, { "hunk", 0 } };
    CHECK_FALSE(ui.controller.handle_web_message(stale.dump()).accepted);
}

namespace {
    file_diff binary_diff(const std::string& path)
    {
        file_diff diff {};
        diff.request.path = path;
        diff.files = parse_git_diff(join_lines({
            "diff --git a/" + path + " b/" + path,
            "index 1111111..2222222 100644",
            "Binary files a/" + path + " and b/" + path + " differ",
        }));
        return diff;
    }
} // namespace

TEST_CASE("a binary file asks for the sizes of its two sides and then shows a card", "[diff][controller]")
{
    page ui {};
    const std::vector<diff_view_effect> effects { ui.controller.show(binary_diff("data/model.bin"), u8"C:\\repo") };
    REQUIRE(effects.size() == 1);
    REQUIRE(std::holds_alternative<prepare_sides_effect>(effects.front()));
    const prepare_sides_effect& prepare { std::get<prepare_sides_effect>(effects.front()) };
    CHECK_FALSE(prepare.write_files);
    CHECK(prepare.plan.new_path == "data/model.bin");
    CHECK(prepare.plan.repository_path == u8"C:\\repo");

    // 기다리는 동안 뷰어는 비워 둔다.
    REQUIRE(ui.controller.poll());
    CHECK(ui.latest().at("type") == "clear");

    extracted_sides sides {};
    sides.ok = true;
    sides.left = { true, {}, 100 };
    sides.right = { true, {}, 250 };
    ui.controller.sides_prepared(prepare.ticket, sides);
    REQUIRE(ui.controller.poll());
    const json card { ui.latest() };
    CHECK(card.at("type") == "show_binary");
    CHECK(card.at("old_size") == 100);
    CHECK(card.at("new_size") == 250);
    CHECK(card.at("path") == "data/model.bin");

    // 열기는 받아들인다.
    CHECK(ui.controller.handle_web_message(ui.action({ { "type", "open_external" } })).accepted);
}

TEST_CASE("an image shows both sides from temporary files", "[diff][controller]")
{
    page ui {};
    const std::vector<diff_view_effect> effects { ui.controller.show(binary_diff("img/logo.PNG"), u8"C:\\repo") };
    REQUIRE(effects.size() == 1);
    const prepare_sides_effect& prepare { std::get<prepare_sides_effect>(effects.front()) };
    CHECK(prepare.write_files);

    extracted_sides sides {};
    sides.ok = true;
    sides.left = { true, u8"C:\\Temp\\cyella-diff\\op1\\left\\logo.PNG", 10 };
    sides.right = { false, u8"C:\\Temp\\cyella-diff\\op1\\right\\logo.PNG", std::nullopt };
    ui.controller.sides_prepared(prepare.ticket, sides);
    REQUIRE(ui.controller.poll());
    const json image { ui.latest() };
    CHECK(image.at("type") == "show_image");
    CHECK(image.at("left_url") == "file:///C:/Temp/cyella-diff/op1/left/logo.PNG");
    // 없는 쪽(삭제된 파일의 오른쪽)은 주소가 없다.
    CHECK(image.at("right_url").is_null());
    CHECK(image.at("left_size") == 10);
}

TEST_CASE("results for a file the user already left are dropped", "[diff][controller]")
{
    page ui {};
    const std::vector<diff_view_effect> effects { ui.controller.show(binary_diff("img/logo.png"), u8"C:\\repo") };
    const std::uint64_t old_ticket { std::get<prepare_sides_effect>(effects.front()).ticket };
    static_cast<void>(ui.controller.show(modified_diff(), u8"C:\\repo"));
    REQUIRE(ui.controller.poll());

    extracted_sides sides {};
    sides.ok = true;
    ui.controller.sides_prepared(old_ticket, sides);
    CHECK_FALSE(ui.controller.poll());
    CHECK(ui.latest().at("type") == "show_diff");
}

TEST_CASE("an image whose sides could not be extracted falls back to the card", "[diff][controller]")
{
    page ui {};
    const std::vector<diff_view_effect> effects { ui.controller.show(binary_diff("img/logo.png"), u8"C:\\repo") };
    extracted_sides failed {};
    failed.ok = false;
    failed.error = "git failed";
    ui.controller.sides_prepared(std::get<prepare_sides_effect>(effects.front()).ticket, failed);
    REQUIRE(ui.controller.poll());
    CHECK(ui.latest().at("type") == "show_binary");
}
TEST_CASE("a textconv file shows no stage or discard buttons because its hunks do not match the file", "[diff][controller]")
{
    page ui {};
    file_diff plain { modified_diff() };
    static_cast<void>(ui.controller.show(plain, u8"C:\\repo"));
    REQUIRE(ui.controller.poll());
    CHECK(ui.controller.can_stage());
    CHECK(ui.controller.textconv_driver().empty());
    CHECK(ui.latest().at("can_stage") == true);

    file_diff converted { modified_diff() };
    converted.textconv_driver = "word";
    static_cast<void>(ui.controller.show(converted, u8"C:\\repo"));
    REQUIRE(ui.controller.poll());
    CHECK(ui.controller.textconv_driver() == "word");
    CHECK_FALSE(ui.controller.can_stage());
    CHECK_FALSE(ui.controller.can_discard());
    CHECK(ui.latest().at("can_stage") == false);
    // 웹뷰가 단추 없이도 보낸 동작은 거절한다.
    const web_message_result refused { ui.controller.handle_web_message(ui.action({ { "type", "stage_hunk" }, { "hunk", 0 } })) };
    CHECK_FALSE(refused.accepted);

    // 다음 파일(textconv 없음)로 넘어가면 다시 켜진다.
    static_cast<void>(ui.controller.show(plain, u8"C:\\repo"));
    CHECK(ui.controller.can_stage());
    CHECK(ui.controller.textconv_driver().empty());
}

TEST_CASE("a diff read without context needs --unidiff-zero to be applied", "[diff][controller]")
{
    page ui {};
    file_diff diff { modified_diff() };
    diff.request.context_lines = 0;
    static_cast<void>(ui.controller.show(diff, u8"C:\\repo"));
    REQUIRE(ui.controller.poll());
    const web_message_result zero { ui.controller.handle_web_message(ui.action({ { "type", "stage_hunk" }, { "hunk", 0 } })) };
    REQUIRE(zero.accepted);
    CHECK(effect_of<apply_patch_effect>(zero).request.unidiff_zero);

    page normal {};
    static_cast<void>(normal.controller.show(modified_diff(), u8"C:\\repo"));
    REQUIRE(normal.controller.poll());
    const web_message_result with_context { normal.controller.handle_web_message(normal.action({ { "type", "stage_hunk" }, { "hunk", 0 } })) };
    REQUIRE(with_context.accepted);
    CHECK_FALSE(effect_of<apply_patch_effect>(with_context).request.unidiff_zero);
}
