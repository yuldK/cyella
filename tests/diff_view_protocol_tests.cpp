#include "application/diff_view_protocol.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {
    using namespace cyella;
    using json = nlohmann::json;

    std::string base64_decode(const std::string_view text)
    {
        std::string decoded {};
        std::uint32_t buffer { 0 };
        int bits { 0 };
        for (const char letter : text)
        {
            int value { -1 };
            if (letter >= 'A' && letter <= 'Z')
                value = letter - 'A';
            else if (letter >= 'a' && letter <= 'z')
                value = letter - 'a' + 26;
            else if (letter >= '0' && letter <= '9')
                value = letter - '0' + 52;
            else if (letter == '+')
                value = 62;
            else if (letter == '/')
                value = 63;
            else
                continue;
            buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                decoded.push_back(static_cast<char>((buffer >> bits) & 0xFF));
            }
        }
        return decoded;
    }

    diff_line make_line(const diff_line_kind kind, const std::int32_t old_line, const std::int32_t new_line, std::string content)
    {
        diff_line line {};
        line.kind = kind;
        line.old_line = old_line;
        line.new_line = new_line;
        line.content = std::move(content);
        return line;
    }

    diff_hunk make_hunk(std::vector<diff_line> lines)
    {
        diff_hunk hunk {};
        hunk.old_start = 1;
        hunk.old_count = 3;
        hunk.new_start = 1;
        hunk.new_count = 3;
        hunk.header = "@@ -1,3 +1,3 @@ fn()";
        hunk.section = "fn()";
        hunk.lines = std::move(lines);
        return hunk;
    }

    diff_file sample_file()
    {
        diff_file file {};
        file.old_path = "src/a.cpp";
        file.new_path = "src/a.cpp";
        file.status = 'M';
        file.old_mode = "100644";
        file.new_mode = "100644";
        file.lines_added = 1;
        file.lines_removed = 1;
        file.hunks.push_back(make_hunk({
            make_line(diff_line_kind::context, 1, 1, "keep"),
            make_line(diff_line_kind::del, 2, 0, "old\r"),
            make_line(diff_line_kind::add, 0, 2, "new \xED\x95\x9C\xEA\xB8\x80 \xF0\x9F\x98\x80"),
            make_line(diff_line_kind::no_newline, 0, 0, "No newline at end of file"),
        }));
        return file;
    }

    // JSON으로 나간 파일을 도로 diff_file로 읽는다. 뷰어가 하는 일과 같다.
    diff_file decode_file(const json& value)
    {
        diff_file file {};
        file.old_path = value.at("old_path").get<std::string>();
        file.new_path = value.at("new_path").get<std::string>();
        file.status = value.at("status").get<std::string>().front();
        for (const json& hunk_value : value.at("hunks"))
        {
            diff_hunk hunk {};
            hunk.header = hunk_value.at("header").get<std::string>();
            hunk.section = hunk_value.at("section").get<std::string>();
            hunk.old_start = hunk_value.at("old_start").get<std::int32_t>();
            hunk.old_count = hunk_value.at("old_count").get<std::int32_t>();
            hunk.new_start = hunk_value.at("new_start").get<std::int32_t>();
            hunk.new_count = hunk_value.at("new_count").get<std::int32_t>();
            for (const json& entry : hunk_value.at("lines"))
            {
                diff_line line {};
                const std::string kind { entry.at(0).get<std::string>() };
                line.kind = kind == "c" ? diff_line_kind::context : kind == "a" ? diff_line_kind::add : kind == "d" ? diff_line_kind::del : diff_line_kind::no_newline;
                line.old_line = entry.at(1).get<std::int32_t>();
                line.new_line = entry.at(2).get<std::int32_t>();
                const std::string text { entry.at(3).get<std::string>() };
                line.content = entry.size() > 4 ? base64_decode(text) : text;
                hunk.lines.push_back(std::move(line));
            }
            file.hunks.push_back(std::move(hunk));
        }
        return file;
    }

    json serialize_diff(const diff_file& file, const std::size_t limit = 0)
    {
        show_diff_message message {};
        message.seq = 7;
        message.file = &file;
        message.line_limit = limit;
        return json::parse(serialize_show_diff(message));
    }

    bool same_lines(const diff_file& left, const diff_file& right)
    {
        if (left.hunks.size() != right.hunks.size())
            return false;
        for (std::size_t hunk { 0 }; hunk < left.hunks.size(); ++hunk)
        {
            const std::vector<diff_line>& a { left.hunks[hunk].lines };
            const std::vector<diff_line>& b { right.hunks[hunk].lines };
            if (a.size() != b.size())
                return false;
            for (std::size_t index { 0 }; index < a.size(); ++index)
                if (a[index].kind != b[index].kind || a[index].old_line != b[index].old_line || a[index].new_line != b[index].new_line || a[index].content != b[index].content)
                    return false;
        }
        return true;
    }

    // stage_<suffix>, unstage_<suffix>, discard_<suffix>와 그 명령이다.
    std::vector<std::pair<std::string, diff_hunk_command>> commands_with(const std::string& suffix)
    {
        std::vector<std::pair<std::string, diff_hunk_command>> commands {};
        commands.emplace_back("stage_" + suffix, diff_hunk_command::stage);
        commands.emplace_back("unstage_" + suffix, diff_hunk_command::unstage);
        commands.emplace_back("discard_" + suffix, diff_hunk_command::discard);
        return commands;
    }

    diff_action_parse_result parse(const json& message)
    {
        return parse_diff_view_action(message.dump());
    }
} // namespace

TEST_CASE("show_diff carries the state and the file in one message", "[diff][protocol]")
{
    const diff_file file { sample_file() };
    show_diff_message message {};
    message.seq = 12;
    message.view = diff_view::staged;
    message.state.layout = diff_layout::split;
    message.state.theme.mode = diff_theme_mode::light;
    message.can_unstage = true;
    message.file = &file;

    const json parsed { json::parse(serialize_show_diff(message)) };
    CHECK(parsed.at("type") == "show_diff");
    CHECK(parsed.at("seq") == 12);
    CHECK(parsed.at("view") == "staged");
    CHECK(parsed.at("layout") == "split");
    CHECK(parsed.at("image_mode") == "side_by_side");
    CHECK(parsed.at("theme").at("mode") == "light");
    CHECK(parsed.at("toolbar") == true);
    CHECK(parsed.at("can_stage") == false);
    CHECK(parsed.at("can_unstage") == true);
    CHECK(parsed.at("can_discard") == false);
    CHECK(parsed.at("truncated") == false);
    CHECK(parsed.at("total_lines") == 4);
    CHECK(parsed.at("shown_lines") == 4);
    CHECK(parsed.at("file").at("lines_added") == 1);

    const json& line { parsed.at("file").at("hunks").at(0).at("lines").at(1) };
    CHECK(line == json::array({ "d", 2, 0, "old\r" }));
}

TEST_CASE("a diff round-trips through the message byte for byte", "[diff][protocol]")
{
    const diff_file file { sample_file() };
    const diff_file decoded { decode_file(serialize_diff(file).at("file")) };
    CHECK(same_lines(file, decoded));
    CHECK(decoded.hunks[0].header == file.hunks[0].header);
    CHECK(decoded.old_path == file.old_path);
}

TEST_CASE("lines that are not valid UTF-8 travel as base64 without losing a byte", "[diff][protocol]")
{
    const std::vector<std::string> contents {
        std::string { "\xFF\xFE", 2 },
        "caf\xC3",
        "\xE2\x82",
        "\xC0\x80",
        "\xED\xA0\x80",
        "\xF4\x90\x80\x80",
        std::string { "nul\0byte", 8 },
        "mixed \xED\x95\x9C then \x80 raw",
    };
    diff_file file {};
    file.old_path = file.new_path = "bin.dat";
    diff_hunk hunk { make_hunk({}) };
    std::int32_t number { 1 };
    for (const std::string& content : contents)
        hunk.lines.push_back(make_line(diff_line_kind::add, 0, number++, content));
    file.hunks.push_back(std::move(hunk));

    const json parsed { serialize_diff(file) };
    const json& lines { parsed.at("file").at("hunks").at(0).at("lines") };
    for (std::size_t index { 0 }; index < contents.size(); ++index)
    {
        CAPTURE(index);
        // 모두 올바르지 않으므로 다섯 번째 값(표지)이 붙고, NUL만 올바른 UTF-8이라 그냥 문자열이다.
        const bool valid { is_valid_utf8(contents[index]) };
        CHECK(lines[index].size() == (valid ? 4U : 5U));
    }
    CHECK(same_lines(file, decode_file(parsed.at("file"))));
}

TEST_CASE("valid UTF-8 stays a plain string", "[diff][protocol]")
{
    diff_file file {};
    file.hunks.push_back(make_hunk({ make_line(diff_line_kind::add, 0, 1, "\xED\x95\x9C\xEA\xB8\x80\xF0\x9F\x98\x80 text") }));
    const json parsed { serialize_diff(file) };
    const json& entry { parsed.at("file").at("hunks").at(0).at("lines").at(0) };
    CHECK(entry.size() == 4);
    CHECK(entry.at(3) == "\xED\x95\x9C\xEA\xB8\x80\xF0\x9F\x98\x80 text");
}

TEST_CASE("is_valid_utf8 follows RFC 3629", "[diff][protocol]")
{
    CHECK(is_valid_utf8(""));
    CHECK(is_valid_utf8("ascii"));
    CHECK(is_valid_utf8("\xC2\x80"));
    CHECK(is_valid_utf8("\xED\x9F\xBF"));
    CHECK(is_valid_utf8("\xEE\x80\x80"));
    CHECK(is_valid_utf8("\xF0\x90\x80\x80"));
    CHECK(is_valid_utf8("\xF4\x8F\xBF\xBF"));

    CHECK_FALSE(is_valid_utf8("\x80"));
    CHECK_FALSE(is_valid_utf8("\xC1\xBF"));
    CHECK_FALSE(is_valid_utf8("\xC2"));
    CHECK_FALSE(is_valid_utf8("\xE0\x80\x80"));
    CHECK_FALSE(is_valid_utf8("\xED\xA0\x80"));
    CHECK_FALSE(is_valid_utf8("\xF0\x80\x80\x80"));
    CHECK_FALSE(is_valid_utf8("\xF4\x90\x80\x80"));
    CHECK_FALSE(is_valid_utf8("\xF5\x80\x80\x80"));
    CHECK_FALSE(is_valid_utf8("\xFF"));
    CHECK_FALSE(is_valid_utf8("\xE2\x82\x28"));
}

TEST_CASE("to_valid_utf8 replaces each bad byte", "[diff][protocol]")
{
    CHECK(to_valid_utf8("plain") == "plain");
    CHECK(to_valid_utf8("a\xFF"
                        "b")
        == "a\xEF\xBF\xBD"
           "b");
    CHECK(to_valid_utf8("\xED\x95\x9C\xC3") == "\xED\x95\x9C\xEF\xBF\xBD");
    CHECK(is_valid_utf8(to_valid_utf8("\xE2\x82\xF0\x90\xFF")));
}

TEST_CASE("base64_encode matches the RFC 4648 vectors", "[diff][protocol]")
{
    CHECK(base64_encode("") == "");
    CHECK(base64_encode("f") == "Zg==");
    CHECK(base64_encode("fo") == "Zm8=");
    CHECK(base64_encode("foo") == "Zm9v");
    CHECK(base64_encode("foob") == "Zm9vYg==");
    CHECK(base64_encode("fooba") == "Zm9vYmE=");
    CHECK(base64_encode("foobar") == "Zm9vYmFy");
    CHECK(base64_encode(std::string { "\xFF\xFE\x00", 3 }) == "//4A");
}

TEST_CASE("a long diff is cut at the line limit and says so", "[diff][protocol]")
{
    diff_file file {};
    for (int hunk { 0 }; hunk < 3; ++hunk)
    {
        std::vector<diff_line> lines {};
        for (int line { 0 }; line < 10; ++line)
            lines.push_back(make_line(diff_line_kind::add, 0, line + 1, "x"));
        file.hunks.push_back(make_hunk(std::move(lines)));
    }

    const json limited { serialize_diff(file, 15) };
    CHECK(limited.at("truncated") == true);
    CHECK(limited.at("total_lines") == 30);
    CHECK(limited.at("shown_lines") == 15);
    REQUIRE(limited.at("file").at("hunks").size() == 2);
    CHECK(limited.at("file").at("hunks").at(0).at("lines").size() == 10);
    CHECK(limited.at("file").at("hunks").at(1).at("lines").size() == 5);

    const json unlimited { serialize_diff(file, 0) };
    CHECK(unlimited.at("truncated") == false);
    CHECK(unlimited.at("shown_lines") == 30);

    // 상한과 같으면 잘리지 않았다.
    CHECK(serialize_diff(file, 30).at("truncated") == false);
}

TEST_CASE("a missing file serializes as an empty diff", "[diff][protocol]")
{
    show_diff_message message {};
    message.seq = 1;
    const json parsed { json::parse(serialize_show_diff(message)) };
    CHECK(parsed.at("file").at("hunks").empty());
    CHECK(parsed.at("total_lines") == 0);
}

TEST_CASE("theme tokens are filtered before they reach CSS", "[diff][protocol]")
{
    diff_theme theme {};
    theme.mode = diff_theme_mode::light;
    theme.tokens["--window-background"] = "#f8f8f8";
    theme.tokens["--diff-add-background"] = "rgba(46, 160, 67, 0.15)";
    theme.tokens["--bad name"] = "#fff";
    theme.tokens["color"] = "red";
    theme.tokens["--evil"] = "red; } body { display: none";
    theme.tokens["--empty"] = "";

    const json parsed { json::parse(serialize_set_theme(3, theme)) };
    CHECK(parsed.at("type") == "set_theme");
    CHECK(parsed.at("seq") == 3);
    CHECK(parsed.at("theme").at("mode") == "light");
    const json& tokens { parsed.at("theme").at("tokens") };
    CHECK(tokens.size() == 2);
    CHECK(tokens.at("--window-background") == "#f8f8f8");
    CHECK(tokens.at("--diff-add-background") == "rgba(46, 160, 67, 0.15)");
}

TEST_CASE("small state messages carry only their own field", "[diff][protocol]")
{
    CHECK(json::parse(serialize_set_layout(4, diff_layout::split)) == json::parse(R"({"type":"set_layout","seq":4,"layout":"split"})"));
    CHECK(json::parse(serialize_set_layout(4, diff_layout::inline_view)).at("layout") == "inline");
    CHECK(json::parse(serialize_set_image_mode(5, image_compare_mode::onion_skin)) == json::parse(R"({"type":"set_image_mode","seq":5,"mode":"onion"})"));
    CHECK(json::parse(serialize_set_image_mode(5, image_compare_mode::swipe)).at("mode") == "swipe");

    diff_view_state state {};
    state.layout = diff_layout::split;
    state.toolbar = false;
    const json cleared { json::parse(serialize_clear(6, state)) };
    CHECK(cleared.at("type") == "clear");
    CHECK(cleared.at("seq") == 6);
    CHECK(cleared.at("layout") == "split");
    CHECK(cleared.at("toolbar") == false);
    CHECK(cleared.at("theme").at("mode") == "dark");
}

TEST_CASE("image and binary messages carry urls and sizes", "[diff][protocol]")
{
    show_image_message image {};
    image.seq = 8;
    image.path = "assets/logo.png";
    image.status = 'M';
    image.left_url = "file:///C:/tmp/left/logo.png";
    image.left_size = 1024;
    image.right_size = 2048;
    const json parsed_image { json::parse(serialize_show_image(image)) };
    CHECK(parsed_image.at("type") == "show_image");
    CHECK(parsed_image.at("left_url") == "file:///C:/tmp/left/logo.png");
    CHECK(parsed_image.at("right_url").is_null());
    CHECK(parsed_image.at("left_size") == 1024);
    CHECK(parsed_image.at("right_size") == 2048);
    CHECK(parsed_image.at("can_open_external") == true);

    show_binary_message binary {};
    binary.seq = 9;
    binary.path = "tool.exe";
    binary.status = 'A';
    binary.new_size = 4096;
    binary.can_open_external = false;
    const json parsed_binary { json::parse(serialize_show_binary(binary)) };
    CHECK(parsed_binary.at("type") == "show_binary");
    CHECK(parsed_binary.at("old_size").is_null());
    CHECK(parsed_binary.at("new_size") == 4096);
    CHECK(parsed_binary.at("can_open_external") == false);
}

TEST_CASE("make_file_url percent-encodes the path the way a browser reads it", "[diff][protocol]")
{
    CHECK(make_file_url(u8"") == "");
    CHECK(make_file_url(u8"C:\\Temp\\cyella-diff\\left\\a.png") == "file:///C:/Temp/cyella-diff/left/a.png");
    CHECK(make_file_url(u8"C:\\Temp\\a b#c%.png") == "file:///C:/Temp/a%20b%23c%25.png");
    CHECK(make_file_url(u8"C:\\\uD55C\uAE00\\\uADF8\uB9BC.png") == "file:///C:/%ED%95%9C%EA%B8%80/%EA%B7%B8%EB%A6%BC.png");
    CHECK(make_file_url(u8"\\\\server\\share\\x.png") == "file://server/share/x.png");
}

TEST_CASE("every action the page can send parses", "[diff][protocol]")
{
    SECTION("ready")
    {
        const diff_action_parse_result result { parse({ { "type", "ready" }, { "protocol", 1 } }) };
        REQUIRE(result.action.has_value());
        CHECK(std::get<ready_action>(*result.action).protocol == 1);
    }

    SECTION("hunk actions")
    {
        for (const auto& [name, command] : commands_with("hunk"))
        {
            const diff_action_parse_result result { parse({ { "type", name }, { "seq", 9 }, { "hunk", 2 } }) };
            REQUIRE(result.action.has_value());
            const hunk_action& action { std::get<hunk_action>(*result.action) };
            CHECK(action.command == command);
            CHECK(action.seq == 9);
            CHECK(action.hunk == 2);
        }
    }

    SECTION("line actions")
    {
        for (const auto& [name, command] : commands_with("lines"))
        {
            const diff_action_parse_result result { parse({ { "type", name }, { "seq", 3 }, { "hunk", 0 }, { "ranges", json::array({ json::array({ 1, 4 }), json::array({ 7, 7 }) }) } }) };
            REQUIRE(result.action.has_value());
            const lines_action& action { std::get<lines_action>(*result.action) };
            CHECK(action.command == command);
            CHECK(action.seq == 3);
            CHECK(action.ranges == std::vector<diff_line_range> { { 1, 4 }, { 7, 7 } });
        }
    }

    SECTION("open_external, show_full and copy")
    {
        const diff_action_parse_result external { parse({ { "type", "open_external" }, { "seq", 5 } }) };
        REQUIRE(external.action.has_value());
        CHECK(std::get<open_external_action>(*external.action).seq == 5);

        const diff_action_parse_result full { parse({ { "type", "show_full" }, { "seq", 6 } }) };
        REQUIRE(full.action.has_value());
        CHECK(std::get<show_full_action>(*full.action).seq == 6);

        const diff_action_parse_result copied { parse({ { "type", "copy" }, { "seq", 7 }, { "text", "line one\nline two\xED\x95\x9C" } }) };
        REQUIRE(copied.action.has_value());
        CHECK(std::get<copy_action>(*copied.action).text == "line one\nline two\xED\x95\x9C");
    }

    SECTION("view changes the page made itself")
    {
        const diff_action_parse_result layout { parse({ { "type", "layout_changed" }, { "layout", "split" } }) };
        REQUIRE(layout.action.has_value());
        CHECK(std::get<layout_changed_action>(*layout.action).layout == diff_layout::split);

        const diff_action_parse_result mode { parse({ { "type", "image_mode_changed" }, { "mode", "onion" } }) };
        REQUIRE(mode.action.has_value());
        CHECK(std::get<image_mode_changed_action>(*mode.action).mode == image_compare_mode::onion_skin);
    }
}

TEST_CASE("malformed messages from the page are rejected", "[diff][protocol]")
{
    const json base { { "type", "stage_hunk" }, { "seq", 1 }, { "hunk", 0 } };
    REQUIRE(parse(base).action.has_value());

    const std::vector<std::string> rejected {
        "",
        "not json",
        "{",
        "[]",
        "42",
        "\"stage_hunk\"",
        "null",
        R"({})",
        R"({"type":5})",
        R"({"type":"unknown_action"})",
        R"({"type":"stage_hunk"})",
        R"({"type":"stage_hunk","seq":1})",
        R"({"type":"stage_hunk","seq":-1,"hunk":0})",
        R"({"type":"stage_hunk","seq":1.5,"hunk":0})",
        R"({"type":"stage_hunk","seq":"1","hunk":0})",
        R"({"type":"stage_hunk","seq":1,"hunk":-2})",
        R"({"type":"stage_hunk","seq":1,"hunk":2000000000})",
        R"({"type":"stage_hunk","seq":18446744073709551616,"hunk":0})",
        R"({"type":"stage_hunk","seq":true,"hunk":0})",
        R"({"type":"stage_hunk","seq":1,"hunk":0,"extra":1})",
        R"({"type":"stage_lines","seq":1,"hunk":0})",
        R"({"type":"stage_lines","seq":1,"hunk":0,"ranges":[]})",
        R"({"type":"stage_lines","seq":1,"hunk":0,"ranges":5})",
        R"({"type":"stage_lines","seq":1,"hunk":0,"ranges":[1,2]})",
        R"({"type":"stage_lines","seq":1,"hunk":0,"ranges":[[1]]})",
        R"({"type":"stage_lines","seq":1,"hunk":0,"ranges":[[1,2,3]]})",
        R"({"type":"stage_lines","seq":1,"hunk":0,"ranges":[[4,2]]})",
        R"({"type":"stage_lines","seq":1,"hunk":0,"ranges":[[-1,2]]})",
        R"({"type":"stage_lines","seq":1,"hunk":0,"ranges":[["a","b"]]})",
        R"({"type":"stage_lines","seq":1,"hunk":0,"ranges":[[0,1]],"extra":true})",
        R"({"type":"open_external"})",
        R"({"type":"open_external","seq":1,"hunk":0})",
        R"({"type":"copy","seq":1})",
        R"({"type":"copy","seq":1,"text":""})",
        R"({"type":"copy","seq":1,"text":5})",
        R"({"type":"layout_changed","layout":"diagonal"})",
        R"({"type":"layout_changed"})",
        R"({"type":"image_mode_changed","mode":"x"})",
        R"({"type":"ready"})",
        R"({"type":"ready","protocol":-1})",
        R"({"type":"ready","protocol":99999})",
    };
    for (const std::string& text : rejected)
    {
        CAPTURE(text);
        const diff_action_parse_result result { parse_diff_view_action(text) };
        CHECK_FALSE(result.action.has_value());
        CHECK_FALSE(result.error.empty());
    }
}

TEST_CASE("size limits of the page messages are enforced", "[diff][protocol]")
{
    json ranges { json::array() };
    for (std::size_t index { 0 }; index < diff_view_maximum_ranges; ++index)
        ranges.push_back(json::array({ index, index }));
    json lines { { "type", "stage_lines" }, { "seq", 1 }, { "hunk", 0 }, { "ranges", ranges } };
    CHECK(parse(lines).action.has_value());
    lines["ranges"].push_back(json::array({ 99999, 99999 }));
    CHECK_FALSE(parse(lines).action.has_value());

    json copy { { "type", "copy" }, { "seq", 1 }, { "text", std::string(diff_view_maximum_copy_bytes, 'x') } };
    CHECK(parse(copy).action.has_value());
    copy["text"] = std::string(diff_view_maximum_copy_bytes + 1, 'x');
    CHECK_FALSE(parse(copy).action.has_value());
}

TEST_CASE("line ranges expand to the add and del lines inside them", "[diff][protocol]")
{
    // 0 ctx, 1 del, 2 add, 3 ctx, 4 add, 5 no-newline
    const diff_hunk hunk = make_hunk({
        make_line(diff_line_kind::context, 1, 1, "a"),
        make_line(diff_line_kind::del, 2, 0, "b"),
        make_line(diff_line_kind::add, 0, 2, "B"),
        make_line(diff_line_kind::context, 3, 3, "c"),
        make_line(diff_line_kind::add, 0, 4, "d"),
        make_line(diff_line_kind::no_newline, 0, 0, "No newline at end of file"),
    });

    CHECK(expand_line_ranges(hunk, { { 0, 5 } }) == std::vector<std::size_t> { 1, 2, 4 });
    CHECK(expand_line_ranges(hunk, { { 2, 2 }, { 1, 1 } }) == std::vector<std::size_t> { 1, 2 });
    CHECK(expand_line_ranges(hunk, { { 1, 3 }, { 2, 4 } }) == std::vector<std::size_t> { 1, 2, 4 });
    // context만 덮는 구간은 빈 선택이다. 거절은 호출자의 몫이다.
    CHECK(expand_line_ranges(hunk, { { 0, 0 } }) == std::vector<std::size_t> {});
    CHECK_FALSE(expand_line_ranges(hunk, { { 0, 6 } }).has_value());
    CHECK_FALSE(expand_line_ranges(hunk, { { 4, 3 } }).has_value());
}
