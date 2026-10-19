#include "domain/patch_builder.h"
#include "infrastructure/git_diff_parser.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using cyella::build_hunk_patch;
    using cyella::build_line_patch;
    using cyella::diff_file;
    using cyella::parse_git_diff;
    using cyella::patch_direction;

    // 줄마다 '\n'을 붙여 텍스트를 만든다.
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

    diff_file parse_one(const std::string& text)
    {
        std::vector<diff_file> files { parse_git_diff(text) };
        REQUIRE(files.size() == 1);
        return files.front();
    }

    diff_file parse_lines(const std::initializer_list<std::string_view> lines)
    {
        return parse_one(join_lines(lines));
    }

    // 줄 위치: 0 a, 1 -b, 2 +B, 3 +B2, 4 c, 5 d, 6 -e, 7 +E, 8 f, 9 g
    diff_file modified_file()
    {
        return parse_lines({
            "diff --git a/f.txt b/f.txt",
            "index 1111111..2222222 100644",
            "--- a/f.txt",
            "+++ b/f.txt",
            "@@ -1,7 +1,8 @@ int main()",
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

    // 줄 위치: 0 +one, 1 +two, 2 +three
    diff_file created_file()
    {
        return parse_lines({
            "diff --git a/n.txt b/n.txt",
            "new file mode 100644",
            "index 0000000..2222222",
            "--- /dev/null",
            "+++ b/n.txt",
            "@@ -0,0 +1,3 @@",
            "+one",
            "+two",
            "+three",
        });
    }

    // 줄 위치: 0 -one, 1 -two, 2 -three
    diff_file deleted_file()
    {
        return parse_lines({
            "diff --git a/d.txt b/d.txt",
            "deleted file mode 100644",
            "index 2222222..0000000",
            "--- a/d.txt",
            "+++ /dev/null",
            "@@ -1,3 +0,0 @@",
            "-one",
            "-two",
            "-three",
        });
    }
} // namespace

TEST_CASE("patch apply mode follows the direction", "[diff][patch]")
{
    CHECK(cyella::patch_apply_mode_for(patch_direction::stage).cached);
    CHECK_FALSE(cyella::patch_apply_mode_for(patch_direction::stage).reverse);
    CHECK(cyella::patch_apply_mode_for(patch_direction::unstage).cached);
    CHECK(cyella::patch_apply_mode_for(patch_direction::unstage).reverse);
    CHECK_FALSE(cyella::patch_apply_mode_for(patch_direction::discard).cached);
    CHECK(cyella::patch_apply_mode_for(patch_direction::discard).reverse);
}

TEST_CASE("staging a hunk keeps every change and recounts the header", "[diff][patch]")
{
    const std::optional<std::string> patch { build_hunk_patch(modified_file(), 0, patch_direction::stage) };
    REQUIRE(patch.has_value());
    CHECK(*patch
        == join_lines({
            "diff --git a/f.txt b/f.txt",
            "--- a/f.txt",
            "+++ b/f.txt",
            "@@ -1,7 +1,8 @@ int main()",
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
        }));
}

TEST_CASE("staging selected lines drops unselected adds and keeps unselected dels as context", "[diff][patch]")
{
    // -b와 +B만 고른다. +B2는 버려지고 -e는 context가 되며 +E는 버려진다.
    const std::optional<std::string> patch { build_line_patch(modified_file(), 0, { 1, 2 }, patch_direction::stage) };
    REQUIRE(patch.has_value());
    CHECK(*patch
        == join_lines({
            "diff --git a/f.txt b/f.txt",
            "--- a/f.txt",
            "+++ b/f.txt",
            "@@ -1,7 +1,7 @@ int main()",
            " a",
            "-b",
            "+B",
            " c",
            " d",
            " e",
            " f",
            " g",
        }));
}

TEST_CASE("unstaging and discarding invert which unselected lines survive", "[diff][patch]")
{
    // 같은 선택이라도 -R은 부호를 뒤집는다. 고르지 않은 add(+B2, +E)는 context가 되고 고르지 않은
    // del(-e)은 버려진다.
    const std::string expected = join_lines({
        "diff --git a/f.txt b/f.txt",
        "--- a/f.txt",
        "+++ b/f.txt",
        "@@ -1,8 +1,8 @@ int main()",
        " a",
        "-b",
        "+B",
        " B2",
        " c",
        " d",
        " E",
        " f",
        " g",
    });
    const std::optional<std::string> unstage { build_line_patch(modified_file(), 0, { 1, 2 }, patch_direction::unstage) };
    REQUIRE(unstage.has_value());
    CHECK(*unstage == expected);
    // 방향이 달라도 patch 텍스트는 같다. 다른 것은 git apply의 옵션뿐이다.
    CHECK(build_line_patch(modified_file(), 0, { 1, 2 }, patch_direction::discard) == expected);
}

TEST_CASE("a selection of one line in a pure insertion hunk still anchors at the hunk start", "[diff][patch]")
{
    const diff_file file = parse_lines({
        "diff --git a/f.txt b/f.txt",
        "--- a/f.txt",
        "+++ b/f.txt",
        "@@ -10,3 +10,5 @@",
        " x",
        "+y1",
        "+y2",
        " z",
        " w",
    });

    const std::optional<std::string> stage { build_line_patch(file, 0, { 2 }, patch_direction::stage) };
    REQUIRE(stage.has_value());
    CHECK(*stage
        == join_lines({
            "diff --git a/f.txt b/f.txt",
            "--- a/f.txt",
            "+++ b/f.txt",
            "@@ -10,3 +10,4 @@",
            " x",
            "+y2",
            " z",
            " w",
        }));

    const std::optional<std::string> unstage { build_line_patch(file, 0, { 2 }, patch_direction::unstage) };
    REQUIRE(unstage.has_value());
    CHECK(*unstage
        == join_lines({
            "diff --git a/f.txt b/f.txt",
            "--- a/f.txt",
            "+++ b/f.txt",
            "@@ -10,4 +10,5 @@",
            " x",
            " y1",
            "+y2",
            " z",
            " w",
        }));
}

TEST_CASE("only the chosen hunk is emitted from a multi-hunk file", "[diff][patch]")
{
    const diff_file file = parse_lines({
        "diff --git a/m.txt b/m.txt",
        "--- a/m.txt",
        "+++ b/m.txt",
        "@@ -1,3 +1,4 @@",
        " a",
        "+A",
        " b",
        " c",
        "@@ -20,3 +21,3 @@ function",
        " t",
        "-u",
        "+U",
        " v",
    });

    const std::optional<std::string> second { build_hunk_patch(file, 1, patch_direction::stage) };
    REQUIRE(second.has_value());
    // 첫 hunk가 줄 수를 바꿨어도 hunk를 따로 보내면 새쪽 시작도 옛쪽과 같다.
    CHECK(*second
        == join_lines({
            "diff --git a/m.txt b/m.txt",
            "--- a/m.txt",
            "+++ b/m.txt",
            "@@ -20,3 +20,3 @@ function",
            " t",
            "-u",
            "+U",
            " v",
        }));
    const std::optional<std::string> second_reversed { build_hunk_patch(file, 1, patch_direction::discard) };
    REQUIRE(second_reversed.has_value());
    CHECK(second_reversed->find("@@ -21,3 +21,3 @@ function\n") != std::string::npos);
}

TEST_CASE("a new file keeps its creation header only while no context lines remain", "[diff][patch]")
{
    SECTION("staging selected lines creates the file with just those lines")
    {
        const std::optional<std::string> patch { build_line_patch(created_file(), 0, { 1 }, patch_direction::stage) };
        REQUIRE(patch.has_value());
        CHECK(*patch
            == join_lines({
                "diff --git a/n.txt b/n.txt",
                "new file mode 100644",
                "--- /dev/null",
                "+++ b/n.txt",
                "@@ -0,0 +1,1 @@",
                "+two",
            }));
    }

    SECTION("unstaging every line removes the file from the index")
    {
        const std::optional<std::string> patch { build_hunk_patch(created_file(), 0, patch_direction::unstage) };
        REQUIRE(patch.has_value());
        CHECK(*patch
            == join_lines({
                "diff --git a/n.txt b/n.txt",
                "new file mode 100644",
                "--- /dev/null",
                "+++ b/n.txt",
                "@@ -0,0 +1,3 @@",
                "+one",
                "+two",
                "+three",
            }));
    }

    SECTION("unstaging some lines turns it into a modification of the remaining file")
    {
        const std::optional<std::string> patch { build_line_patch(created_file(), 0, { 1 }, patch_direction::unstage) };
        REQUIRE(patch.has_value());
        CHECK(*patch
            == join_lines({
                "diff --git a/n.txt b/n.txt",
                "--- a/n.txt",
                "+++ b/n.txt",
                "@@ -1,2 +1,3 @@",
                " one",
                "+two",
                " three",
            }));
    }
}

TEST_CASE("a deleted file keeps its deletion header only while no context lines remain", "[diff][patch]")
{
    SECTION("staging every deletion keeps the deletion header")
    {
        const std::optional<std::string> patch { build_hunk_patch(deleted_file(), 0, patch_direction::stage) };
        REQUIRE(patch.has_value());
        CHECK(*patch
            == join_lines({
                "diff --git a/d.txt b/d.txt",
                "deleted file mode 100644",
                "--- a/d.txt",
                "+++ /dev/null",
                "@@ -1,3 +0,0 @@",
                "-one",
                "-two",
                "-three",
            }));
    }

    SECTION("staging some deletions edits the file instead of removing it")
    {
        const std::optional<std::string> patch { build_line_patch(deleted_file(), 0, { 1 }, patch_direction::stage) };
        REQUIRE(patch.has_value());
        CHECK(*patch
            == join_lines({
                "diff --git a/d.txt b/d.txt",
                "--- a/d.txt",
                "+++ b/d.txt",
                "@@ -1,3 +1,2 @@",
                " one",
                "-two",
                " three",
            }));
    }

    SECTION("restoring some deleted lines creates the file from just those lines")
    {
        const std::optional<std::string> patch { build_line_patch(deleted_file(), 0, { 0, 2 }, patch_direction::discard) };
        REQUIRE(patch.has_value());
        CHECK(*patch
            == join_lines({
                "diff --git a/d.txt b/d.txt",
                "deleted file mode 100644",
                "--- a/d.txt",
                "+++ /dev/null",
                "@@ -1,2 +0,0 @@",
                "-one",
                "-three",
            }));
    }
}

TEST_CASE("no-newline markers follow the line they decorate", "[diff][patch]")
{
    SECTION("a marker on a dropped add is dropped with it")
    {
        const diff_file file = parse_lines({
            "diff --git a/e.txt b/e.txt",
            "--- a/e.txt",
            "+++ b/e.txt",
            "@@ -1,2 +1,2 @@",
            " a",
            "-b",
            "+b2",
            "\\ No newline at end of file",
        });
        const std::optional<std::string> only_deletion { build_line_patch(file, 0, { 1 }, patch_direction::stage) };
        REQUIRE(only_deletion.has_value());
        CHECK(*only_deletion
            == join_lines({
                "diff --git a/e.txt b/e.txt",
                "--- a/e.txt",
                "+++ b/e.txt",
                "@@ -1,2 +1,1 @@",
                " a",
                "-b",
            }));

        // add만 고르면 del이 context로 남고 add의 표지는 그대로 따라온다.
        const std::optional<std::string> only_addition { build_line_patch(file, 0, { 2 }, patch_direction::stage) };
        REQUIRE(only_addition.has_value());
        CHECK(*only_addition
            == join_lines({
                "diff --git a/e.txt b/e.txt",
                "--- a/e.txt",
                "+++ b/e.txt",
                "@@ -1,2 +1,3 @@",
                " a",
                " b",
                "+b2",
                "\\ No newline at end of file",
            }));
    }

    SECTION("a del whose old last line lacks a newline is tied to the adds after it")
    {
        const diff_file file = parse_lines({
            "diff --git a/e.txt b/e.txt",
            "--- a/e.txt",
            "+++ b/e.txt",
            "@@ -1,2 +1,2 @@",
            " a",
            "-b",
            "\\ No newline at end of file",
            "+b2",
        });
        const std::string expected = join_lines({
            "diff --git a/e.txt b/e.txt",
            "--- a/e.txt",
            "+++ b/e.txt",
            "@@ -1,2 +1,2 @@",
            " a",
            "-b",
            "\\ No newline at end of file",
            "+b2",
        });
        // 어느 쪽을 골라도 둘 다 고른 것이 된다.
        CHECK(build_line_patch(file, 0, { 3 }, patch_direction::stage) == expected);
        CHECK(build_line_patch(file, 0, { 1 }, patch_direction::stage) == expected);
        CHECK(build_line_patch(file, 0, { 3 }, patch_direction::unstage) == expected);
    }

    SECTION("a context line without a newline keeps its marker")
    {
        const diff_file file = parse_lines({
            "diff --git a/e.txt b/e.txt",
            "--- a/e.txt",
            "+++ b/e.txt",
            "@@ -1,2 +1,3 @@",
            " a",
            "+x",
            " b",
            "\\ No newline at end of file",
        });
        const std::optional<std::string> patch { build_line_patch(file, 0, { 1 }, patch_direction::stage) };
        REQUIRE(patch.has_value());
        CHECK(*patch
            == join_lines({
                "diff --git a/e.txt b/e.txt",
                "--- a/e.txt",
                "+++ b/e.txt",
                "@@ -1,2 +1,3 @@",
                " a",
                "+x",
                " b",
                "\\ No newline at end of file",
            }));
    }
}

TEST_CASE("CRLF stays inside the line text", "[diff][patch]")
{
    const diff_file file { parse_one("diff --git a/w.txt b/w.txt\n--- a/w.txt\n+++ b/w.txt\n@@ -1,2 +1,2 @@\n a\r\n-b\r\n+c\r\n") };
    const std::optional<std::string> patch { build_hunk_patch(file, 0, patch_direction::stage) };
    REQUIRE(patch.has_value());
    CHECK(patch->ends_with(" a\r\n-b\r\n+c\r\n"));
}

TEST_CASE("paths that need quoting are quoted the way git reads them", "[diff][patch]")
{
    diff_file file { modified_file() };
    SECTION("spaces get a trailing tab on the marker lines")
    {
        file.old_path = file.new_path = "dir/with space.txt";
        const std::optional<std::string> patch { build_hunk_patch(file, 0, patch_direction::stage) };
        REQUIRE(patch.has_value());
        CHECK(patch->starts_with("diff --git a/dir/with space.txt b/dir/with space.txt\n--- a/dir/with space.txt\t\n+++ b/dir/with space.txt\t\n"));
    }

    SECTION("control characters and quotes are escaped")
    {
        file.old_path = file.new_path = "we\"ird\tname.txt";
        const std::optional<std::string> patch { build_hunk_patch(file, 0, patch_direction::stage) };
        REQUIRE(patch.has_value());
        CHECK(patch->starts_with("diff --git \"a/we\\\"ird\\tname.txt\" \"b/we\\\"ird\\tname.txt\"\n--- \"a/we\\\"ird\\tname.txt\"\n+++ \"b/we\\\"ird\\tname.txt\"\n"));
    }

    SECTION("non-ASCII names are left as UTF-8")
    {
        file.old_path = file.new_path = "\xED\x95\x9C\xEA\xB8\x80.txt";
        const std::optional<std::string> patch { build_hunk_patch(file, 0, patch_direction::stage) };
        REQUIRE(patch.has_value());
        CHECK(patch->starts_with("diff --git a/\xED\x95\x9C\xEA\xB8\x80.txt b/\xED\x95\x9C\xEA\xB8\x80.txt\n"));
    }
}

TEST_CASE("a renamed file is patched under the name the target has", "[diff][patch]")
{
    diff_file file { modified_file() };
    file.status = 'R';
    file.old_path = "old.txt";
    file.new_path = "new.txt";
    const std::optional<std::string> unstage { build_hunk_patch(file, 0, patch_direction::unstage) };
    REQUIRE(unstage.has_value());
    CHECK(unstage->starts_with("diff --git a/new.txt b/new.txt\n--- a/new.txt\n+++ b/new.txt\n"));
    const std::optional<std::string> stage { build_hunk_patch(file, 0, patch_direction::stage) };
    REQUIRE(stage.has_value());
    CHECK(stage->starts_with("diff --git a/old.txt b/old.txt\n"));
}

TEST_CASE("invalid selections are rejected", "[diff][patch]")
{
    const diff_file file { modified_file() };
    CHECK_FALSE(build_line_patch(file, 0, {}, patch_direction::stage).has_value());
    // 0은 context, 10은 범위 밖이다.
    CHECK_FALSE(build_line_patch(file, 0, { 0 }, patch_direction::stage).has_value());
    CHECK_FALSE(build_line_patch(file, 0, { 1, 10 }, patch_direction::stage).has_value());
    CHECK_FALSE(build_line_patch(file, 1, { 1 }, patch_direction::stage).has_value());
    CHECK_FALSE(build_hunk_patch(file, 1, patch_direction::stage).has_value());

    SECTION("a marker line cannot be selected")
    {
        const diff_file marked = parse_lines({
            "diff --git a/e.txt b/e.txt",
            "--- a/e.txt",
            "+++ b/e.txt",
            "@@ -1,1 +1,1 @@",
            "-b",
            "+c",
            "\\ No newline at end of file",
        });
        CHECK_FALSE(build_line_patch(marked, 0, { 2 }, patch_direction::stage).has_value());
    }

    SECTION("binary files have no patch")
    {
        diff_file binary { file };
        binary.is_binary = true;
        CHECK_FALSE(build_hunk_patch(binary, 0, patch_direction::stage).has_value());
    }

    SECTION("duplicate indices count once")
    {
        CHECK(build_line_patch(file, 0, { 1, 1, 2 }, patch_direction::stage) == build_line_patch(file, 0, { 2, 1 }, patch_direction::stage));
    }
}

TEST_CASE("changed_line_indices lists only add and del lines", "[diff][patch]")
{
    const diff_file file { modified_file() };
    CHECK(cyella::changed_line_indices(file.hunks[0]) == std::vector<std::size_t> { 1, 2, 3, 6, 7 });
}
