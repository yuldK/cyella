#include "domain/external_diff_tool.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace cyella;

namespace {
    using arguments = std::vector<std::u8string>;

    external_diff_values sample_values()
    {
        external_diff_values values {};
        values.left = u8"C:\\Temp\\old file.png";
        values.right = u8"C:\\Temp\\new file.png";
        values.base = u8"C:\\Temp\\base.png";
        values.path = u8"assets/한글 이미지.png";
        return values;
    }

    external_diff_tool make_tool(std::vector<std::u8string> extensions, std::u8string executable)
    {
        external_diff_tool tool {};
        tool.extensions = std::move(extensions);
        tool.executable = std::move(executable);
        return tool;
    }
} // namespace

TEST_CASE("template tokens are replaced inside single arguments", "[external_diff]")
{
    const auto result { expand_external_diff_arguments(u8"{left} {right}", sample_values()) };
    REQUIRE(result.has_value());
    // 공백이 든 경로도 한 인자로 남는다.
    CHECK(*result == arguments { u8"C:\\Temp\\old file.png", u8"C:\\Temp\\new file.png" });
}

TEST_CASE("quotes group template words and are removed", "[external_diff]")
{
    const auto result { expand_external_diff_arguments(u8"--title \"compare images\" --left=\"{left}\" /r:{right}", sample_values()) };
    REQUIRE(result.has_value());
    CHECK(*result == arguments { u8"--title", u8"compare images", u8"--left=C:\\Temp\\old file.png", u8"/r:C:\\Temp\\new file.png" });
}

TEST_CASE("all four tokens are supported including Korean values", "[external_diff]")
{
    const auto result { expand_external_diff_arguments(u8"-b {base} -p {path}", sample_values()) };
    REQUIRE(result.has_value());
    CHECK(*result == arguments { u8"-b", u8"C:\\Temp\\base.png", u8"-p", u8"assets/한글 이미지.png" });
}

TEST_CASE("values are never reinterpreted as quotes spaces or tokens", "[external_diff]")
{
    external_diff_values values {};
    values.left = u8"a \"quoted\" & b | c; {right} $(x) %PATH%";
    values.right = u8"R";

    const auto result { expand_external_diff_arguments(u8"{left} {right}", values) };
    REQUIRE(result.has_value());
    // 셸 메타 문자와 값 안의 토큰은 글자 그대로 한 인자다.
    CHECK(*result == arguments { u8"a \"quoted\" & b | c; {right} $(x) %PATH%", u8"R" });
}

TEST_CASE("an argument that becomes empty is dropped but a literal empty one stays", "[external_diff]")
{
    external_diff_values values {};
    values.left = u8"L";
    values.right = u8"R";

    const auto dropped { expand_external_diff_arguments(u8"{left} {base} \"{base}\" {right}", values) };
    REQUIRE(dropped.has_value());
    CHECK(*dropped == arguments { u8"L", u8"R" });

    const auto kept { expand_external_diff_arguments(u8"a \"\" b", values) };
    REQUIRE(kept.has_value());
    CHECK(*kept == arguments { u8"a", u8"", u8"b" });

    // 토큰이 다른 글자와 붙어 있으면 비어도 인자는 남는다.
    const auto prefixed { expand_external_diff_arguments(u8"--base={base}", values) };
    REQUIRE(prefixed.has_value());
    CHECK(*prefixed == arguments { u8"--base=" });
}

TEST_CASE("unknown braces and backslashes stay literal", "[external_diff]")
{
    const auto result { expand_external_diff_arguments(u8"{unknown} {LEFT} C:\\tools\\x.exe {left", sample_values()) };
    REQUIRE(result.has_value());
    CHECK(*result == arguments { u8"{unknown}", u8"{LEFT}", u8"C:\\tools\\x.exe", u8"{left" });
}

TEST_CASE("blank and unterminated templates", "[external_diff]")
{
    const auto empty { expand_external_diff_arguments(u8"", sample_values()) };
    REQUIRE(empty.has_value());
    CHECK(empty->empty());

    const auto blank { expand_external_diff_arguments(u8" \t  ", sample_values()) };
    REQUIRE(blank.has_value());
    CHECK(blank->empty());

    CHECK_FALSE(expand_external_diff_arguments(u8"\"{left} {right}", sample_values()).has_value());
}

TEST_CASE("extensions are normalized for comparison", "[external_diff]")
{
    CHECK(normalize_diff_extension(u8".PNG") == u8".png");
    CHECK(normalize_diff_extension(u8"png") == u8".png");
    CHECK(normalize_diff_extension(u8"*.Tar.GZ") == u8".tar.gz");
    CHECK(normalize_diff_extension(u8"  .psd ") == u8".psd");
    CHECK(normalize_diff_extension(u8"") == u8"");
    CHECK(normalize_diff_extension(u8".") == u8"");
    CHECK(normalize_diff_extension(u8"*") == u8"");
}

TEST_CASE("a tool is picked by case insensitive extension", "[external_diff]")
{
    const std::vector<external_diff_tool> tools {
        make_tool({ u8".png", u8".jpg" }, u8"C:\\tools\\imgdiff.exe"),
        make_tool({ u8"psd" }, u8"C:\\tools\\psddiff.exe"),
    };

    const external_diff_tool* png { select_external_diff_tool(tools, u8"assets/Logo.PNG") };
    REQUIRE(png != nullptr);
    CHECK(png->executable == u8"C:\\tools\\imgdiff.exe");

    const external_diff_tool* psd { select_external_diff_tool(tools, u8"C:\\art\\한글.Psd") };
    REQUIRE(psd != nullptr);
    CHECK(psd->executable == u8"C:\\tools\\psddiff.exe");

    CHECK(select_external_diff_tool(tools, u8"src/main.cpp") == nullptr);
    CHECK(select_external_diff_tool(tools, u8"png") == nullptr);
    CHECK(select_external_diff_tool({}, u8"a.png") == nullptr);
}

TEST_CASE("only the file name extension counts and a bare extension name is not a match", "[external_diff]")
{
    const std::vector<external_diff_tool> tools { make_tool({ u8".png" }, u8"x.exe") };

    // 폴더 이름의 점은 확장자가 아니다.
    CHECK(select_external_diff_tool(tools, u8"dir.png/readme") == nullptr);
    CHECK(select_external_diff_tool(tools, u8"dir.png\\readme") == nullptr);
    // 이름이 확장자뿐인 파일은 숨김 파일이다.
    CHECK(select_external_diff_tool(tools, u8"images/.png") == nullptr);
    CHECK(select_external_diff_tool(tools, u8"a.png.bak") == nullptr);
}

TEST_CASE("multi dot extensions win over shorter ones regardless of order", "[external_diff]")
{
    const std::vector<external_diff_tool> tools {
        make_tool({ u8".gz" }, u8"gz.exe"),
        make_tool({ u8".tar.gz" }, u8"targz.exe"),
    };

    const external_diff_tool* archive { select_external_diff_tool(tools, u8"dist/pkg.TAR.GZ") };
    REQUIRE(archive != nullptr);
    CHECK(archive->executable == u8"targz.exe");

    const external_diff_tool* single { select_external_diff_tool(tools, u8"dist/log.gz") };
    REQUIRE(single != nullptr);
    CHECK(single->executable == u8"gz.exe");

    // 확장자 길이가 같으면 목록에서 먼저 나온 도구다.
    const std::vector<external_diff_tool> tied {
        make_tool({ u8".dat" }, u8"first.exe"),
        make_tool({ u8".DAT" }, u8"second.exe"),
    };
    CHECK(select_external_diff_tool(tied, u8"a.dat")->executable == u8"first.exe");
}
