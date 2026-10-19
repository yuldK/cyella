#include "infrastructure/git_diff_parser.h"
#include "infrastructure/git_status_parser.h"

#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using cyella::diff_file;
    using cyella::diff_hunk;
    using cyella::diff_line;
    using cyella::diff_line_kind;
    using cyella::parse_git_diff;

    // 줄마다 '\n'을 붙여 diff 본문을 만든다.
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

    const diff_file* find_file(const std::vector<diff_file>& files, const std::string_view path)
    {
        for (const diff_file& file : files)
            if (file.display_path() == path)
                return &file;
        return nullptr;
    }

    std::vector<diff_file> shapes()
    {
        return parse_git_diff(cyella::test::read_fixture("diff_shapes.patch"));
    }
} // namespace

TEST_CASE("parse_git_diff returns nothing for an empty diff", "[git][diff]")
{
    CHECK(parse_git_diff({}).empty());
    CHECK(parse_git_diff(cyella::test::read_fixture("diff_empty.patch")).empty());
}

TEST_CASE("parse_git_diff reads every file section of a commit", "[git][diff]")
{
    const std::vector<diff_file> files { shapes() };

    REQUIRE(files.size() == 13);
    CHECK(files[0].display_path() == "after_rename.txt");
    CHECK(files[1].display_path() == "blob.bin");
    CHECK(files[12].display_path() == "\xED\x95\x9C\xEA\xB8\x80.txt");
}

TEST_CASE("parse_git_diff reads hunk headers with function context", "[git][diff]")
{
    const std::vector<diff_file> files { shapes() };
    const diff_file* code { find_file(files, "code.c") };

    REQUIRE(code != nullptr);
    CHECK(code->status == 'M');
    CHECK(code->old_path == "code.c");
    CHECK(code->new_path == "code.c");
    CHECK(code->old_blob == "18acd49");
    CHECK(code->new_blob == "1e392bd");
    CHECK(code->old_mode == "100644");
    REQUIRE(code->hunks.size() == 2);
    CHECK(code->lines_added == 2);
    CHECK(code->lines_removed == 2);

    const diff_hunk& first { code->hunks[0] };
    CHECK(first.old_start == 14);
    CHECK(first.old_count == 7);
    CHECK(first.new_start == 14);
    CHECK(first.new_count == 7);
    CHECK(first.header == "@@ -14,7 +14,7 @@ int add(int a, int b)");
    CHECK(first.section == "int add(int a, int b)");
    CHECK(code->hunks[1].section == "int sub(int a, int b)");
}

TEST_CASE("parse_git_diff numbers lines on both sides", "[git][diff]")
{
    const std::vector<diff_file> files { shapes() };
    const diff_file* code { find_file(files, "code.c") };
    REQUIRE(code != nullptr);

    const std::vector<diff_line>& lines { code->hunks[0].lines };
    REQUIRE(lines.size() == 8);
    CHECK(lines[0].kind == diff_line_kind::context);
    CHECK(lines[0].old_line == 14);
    CHECK(lines[0].new_line == 14);
    CHECK(lines[0].content == "    /* pad 10 */");
    CHECK(lines[3].kind == diff_line_kind::del);
    CHECK(lines[3].old_line == 17);
    CHECK(lines[3].new_line == 0);
    CHECK(lines[3].content == "    return a + b;");
    CHECK(lines[4].kind == diff_line_kind::add);
    CHECK(lines[4].old_line == 0);
    CHECK(lines[4].new_line == 17);
    CHECK(lines[4].content == "    return a + b + 0;");
    CHECK(lines[5].old_line == 18);
    CHECK(lines[5].new_line == 18);
    // 빈 context 줄도 표지(공백)만 떼고 남는다.
    CHECK(lines[6].content.empty());
}

TEST_CASE("parse_git_diff fills in omitted hunk counts", "[git][diff]")
{
    const std::vector<diff_file> files { shapes() };
    const diff_file* single { find_file(files, "one_line.txt") };

    REQUIRE(single != nullptr);
    REQUIRE(single->hunks.size() == 1);
    const diff_hunk& hunk { single->hunks[0] };
    CHECK(hunk.header == "@@ -1 +1 @@");
    CHECK(hunk.old_start == 1);
    CHECK(hunk.old_count == 1);
    CHECK(hunk.new_start == 1);
    CHECK(hunk.new_count == 1);
    CHECK(hunk.section.empty());
    REQUIRE(hunk.lines.size() == 2);
    CHECK(hunk.lines[0].kind == diff_line_kind::del);
    CHECK(hunk.lines[1].kind == diff_line_kind::add);
}

TEST_CASE("parse_git_diff handles new and deleted files", "[git][diff]")
{
    const std::vector<diff_file> files { shapes() };

    const diff_file* created { find_file(files, "created.txt") };
    REQUIRE(created != nullptr);
    CHECK(created->status == 'A');
    CHECK(created->old_path.empty());
    CHECK(created->new_path == "created.txt");
    CHECK(created->new_mode == "100644");
    REQUIRE(created->hunks.size() == 1);
    CHECK(created->hunks[0].old_start == 0);
    CHECK(created->hunks[0].old_count == 0);
    CHECK(created->hunks[0].new_count == 2);
    CHECK(created->lines_added == 2);
    CHECK(created->lines_removed == 0);

    const diff_file* deleted { find_file(files, "deleted.txt") };
    REQUIRE(deleted != nullptr);
    CHECK(deleted->status == 'D');
    CHECK(deleted->new_path.empty());
    CHECK(deleted->old_path == "deleted.txt");
    CHECK(deleted->old_mode == "100644");
    REQUIRE(deleted->hunks.size() == 1);
    CHECK(deleted->hunks[0].new_start == 0);
    CHECK(deleted->hunks[0].new_count == 0);
    CHECK(deleted->lines_removed == 1);
}

TEST_CASE("parse_git_diff handles renames with and without edits", "[git][diff]")
{
    const std::vector<diff_file> files { shapes() };

    const diff_file* edited { find_file(files, "after_rename.txt") };
    REQUIRE(edited != nullptr);
    CHECK(edited->status == 'R');
    CHECK(edited->old_path == "before_rename.txt");
    CHECK(edited->new_path == "after_rename.txt");
    CHECK(edited->similarity == 84);
    REQUIRE(edited->hunks.size() == 1);
    CHECK(edited->hunks[0].section == "content");
    CHECK(edited->lines_added == 1);

    const diff_file* pure { find_file(files, "pure_after.txt") };
    REQUIRE(pure != nullptr);
    CHECK(pure->status == 'R');
    CHECK(pure->old_path == "pure_before.txt");
    CHECK(pure->similarity == 100);
    CHECK(pure->hunks.empty());
    CHECK_FALSE(pure->is_binary);
}

TEST_CASE("parse_git_diff handles a mode change without content change", "[git][diff]")
{
    const std::vector<diff_file> files { shapes() };
    const diff_file* script { find_file(files, "script.sh") };

    REQUIRE(script != nullptr);
    CHECK(script->status == 'M');
    CHECK(script->old_mode == "100644");
    CHECK(script->new_mode == "100755");
    CHECK(script->old_path == "script.sh");
    CHECK(script->new_path == "script.sh");
    CHECK(script->hunks.empty());
}

TEST_CASE("parse_git_diff flags binary files", "[git][diff]")
{
    const std::vector<diff_file> files { shapes() };
    const diff_file* blob { find_file(files, "blob.bin") };

    REQUIRE(blob != nullptr);
    CHECK(blob->is_binary);
    CHECK(blob->hunks.empty());
    CHECK(blob->old_path == "blob.bin");
    CHECK(blob->old_blob == "c94be36");
}

TEST_CASE("parse_git_diff ignores the body of a binary patch", "[git][diff]")
{
    const std::vector<diff_file> files { parse_git_diff(cyella::test::read_fixture("diff_binary_patch.patch")) };

    REQUIRE(files.size() == 1);
    CHECK(files[0].is_binary);
    CHECK(files[0].hunks.empty());
}

TEST_CASE("parse_git_diff marks lines without a trailing newline", "[git][diff]")
{
    const std::vector<diff_file> files { shapes() };

    const diff_file* changed { find_file(files, "nonl.txt") };
    REQUIRE(changed != nullptr);
    REQUIRE(changed->hunks.size() == 1);
    const std::vector<diff_line>& lines { changed->hunks[0].lines };
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].kind == diff_line_kind::del);
    CHECK(lines[1].kind == diff_line_kind::no_newline);
    CHECK(lines[1].content == "No newline at end of file");
    CHECK(lines[1].old_line == 0);
    CHECK(lines[1].new_line == 0);
    CHECK(lines[2].kind == diff_line_kind::add);
    CHECK(lines[3].kind == diff_line_kind::no_newline);
    CHECK(changed->lines_added == 1);
    CHECK(changed->lines_removed == 1);

    const diff_file* created { find_file(files, "created_nonl.txt") };
    REQUIRE(created != nullptr);
    REQUIRE(created->hunks.size() == 1);
    REQUIRE(created->hunks[0].lines.size() == 2);
    CHECK(created->hunks[0].lines[1].kind == diff_line_kind::no_newline);
}

TEST_CASE("parse_git_diff keeps carriage returns of CRLF content", "[git][diff]")
{
    const std::vector<diff_file> files { shapes() };
    const diff_file* crlf { find_file(files, "crlf.txt") };

    REQUIRE(crlf != nullptr);
    REQUIRE(crlf->hunks.size() == 1);
    const std::vector<diff_line>& lines { crlf->hunks[0].lines };
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].content == "keep\r");
    CHECK(lines[1].kind == diff_line_kind::del);
    CHECK(lines[1].content == "me\r");
    CHECK(lines[2].content == "me CHANGED\r");
    CHECK(lines[3].content == "CRLF\r");
}

TEST_CASE("parse_git_diff decodes quoted paths with octal escapes", "[git][diff]")
{
    const std::vector<diff_file> files { shapes() };
    const diff_file* korean { find_file(files, "\xED\x95\x9C\xEA\xB8\x80.txt") };

    REQUIRE(korean != nullptr);
    CHECK(korean->old_path == "\xED\x95\x9C\xEA\xB8\x80.txt");
    CHECK(korean->new_path == "\xED\x95\x9C\xEA\xB8\x80.txt");
    CHECK(korean->lines_added == 1);

    // quotepath를 끈 출력은 인용 없이 같은 결과여야 한다.
    const std::vector<diff_file> unquoted { parse_git_diff(cyella::test::read_fixture("diff_unquoted_path.patch")) };
    REQUIRE(unquoted.size() == 1);
    CHECK(unquoted[0].new_path == "\xED\x95\x9C\xEA\xB8\x80.txt");
}

TEST_CASE("parse_git_diff handles paths with spaces", "[git][diff]")
{
    const std::vector<diff_file> files { shapes() };
    const diff_file* spaced { find_file(files, "with space.txt") };

    REQUIRE(spaced != nullptr);
    CHECK(spaced->old_path == "with space.txt");
    CHECK(spaced->new_path == "with space.txt");
    REQUIRE(spaced->hunks.size() == 1);
}

TEST_CASE("parse_git_diff reads staged diffs with renames and untracked diffs", "[git][diff]")
{
    const std::vector<diff_file> staged { parse_git_diff(cyella::test::read_fixture("diff_staged_all.patch")) };
    REQUIRE(staged.size() == 4);
    const diff_file* renamed { find_file(staged, "new name.txt") };
    REQUIRE(renamed != nullptr);
    CHECK(renamed->status == 'R');
    CHECK(renamed->old_path == "old_name.txt");
    CHECK(renamed->similarity == 100);

    const std::vector<diff_file> untracked { parse_git_diff(cyella::test::read_fixture("diff_untracked.patch")) };
    REQUIRE(untracked.size() == 1);
    CHECK(untracked[0].status == 'A');
    CHECK(untracked[0].new_path == "untracked.txt");
    CHECK(untracked[0].lines_added == 1);

    const std::vector<diff_file> unstaged { parse_git_diff(cyella::test::read_fixture("diff_unstaged_modify.patch")) };
    REQUIRE(unstaged.size() == 1);
    CHECK(unstaged[0].new_path == "dir/b.txt");
    CHECK(unstaged[0].lines_added == 1);
    CHECK(unstaged[0].lines_removed == 1);
}

TEST_CASE("parse_git_diff reads the first-parent diff of a merge and a root commit", "[git][diff]")
{
    const std::vector<diff_file> merge { parse_git_diff(cyella::test::read_fixture("diff_merge_first_parent.patch")) };
    REQUIRE(merge.size() == 1);
    CHECK(merge[0].new_path == "f.txt");
    CHECK(merge[0].status == 'A');

    const std::vector<diff_file> root { parse_git_diff(cyella::test::read_fixture("diff_root_commit.patch")) };
    REQUIRE(root.size() >= 5);
    for (const diff_file& file : root)
        CHECK(file.status == 'A');
}

TEST_CASE("parse_git_diff reads zero-context hunks", "[git][diff]")
{
    const std::vector<diff_file> files { parse_git_diff(cyella::test::read_fixture("diff_u0.patch")) };

    REQUIRE(files.size() == 1);
    REQUIRE(files[0].hunks.size() == 2);
    CHECK(files[0].hunks[0].header == "@@ -17 +17 @@ int add(int a, int b)");
    CHECK(files[0].hunks[0].lines.size() == 2);
    CHECK(files[0].hunks[1].old_start == 34);
}

TEST_CASE("parse_git_diff counts lines by the hunk header, not by their look", "[git][diff]")
{
    // 지워진 줄 "-- foo"가 diff에서는 "--- foo"로 보이고, 추가된 줄 "++ bar"는 "+++ bar"로 보인다.
    // 새 파일 머리말로 오인하면 안 된다. 내용이 "diff --git"로 시작하는 줄도 마찬가지다.
    const std::string text = join_lines({
        "diff --git a/x b/x",
        "index 1111111..2222222 100644",
        "--- a/x",
        "+++ b/x",
        "@@ -1,3 +1,3 @@",
        "--- foo",
        "+++ bar",
        "-diff --git a/fake b/fake",
        "+diff --git a/fake2 b/fake2",
        " keep",
        "diff --git a/y b/y",
        "new file mode 100644",
        "--- /dev/null",
        "+++ b/y",
        "@@ -0,0 +1 @@",
        "+y",
    });
    const std::vector<diff_file> files { parse_git_diff(text) };

    REQUIRE(files.size() == 2);
    REQUIRE(files[0].hunks.size() == 1);
    const std::vector<diff_line>& lines { files[0].hunks[0].lines };
    REQUIRE(lines.size() == 5);
    CHECK(lines[0].kind == diff_line_kind::del);
    CHECK(lines[0].content == "-- foo");
    CHECK(lines[1].kind == diff_line_kind::add);
    CHECK(lines[1].content == "++ bar");
    CHECK(lines[2].content == "diff --git a/fake b/fake");
    CHECK(lines[3].content == "diff --git a/fake2 b/fake2");
    CHECK(lines[4].kind == diff_line_kind::context);
    CHECK(files[1].new_path == "y");
}

TEST_CASE("parse_git_diff skips commit headers and tolerates truncated input", "[git][diff]")
{
    const std::string text = join_lines({
        "commit 1111111111111111111111111111111111111111",
        "Author: Ann <ann@example.com>",
        "",
        "    message",
        "",
        "diff --git a/x b/x",
        "--- a/x",
        "+++ b/x",
        "@@ -1,3 +1,3 @@",
        " one",
        "-two",
    });
    const std::vector<diff_file> files { parse_git_diff(text) };

    REQUIRE(files.size() == 1);
    REQUIRE(files[0].hunks.size() == 1);
    CHECK(files[0].hunks[0].lines.size() == 2);
    CHECK(files[0].lines_removed == 1);
}

TEST_CASE("parse_git_diff skips combined diffs it does not support", "[git][diff]")
{
    const std::string text = join_lines({
        "diff --cc merged.txt",
        "index 1111111,2222222..3333333",
        "--- a/merged.txt",
        "+++ b/merged.txt",
        "@@@ -1,2 -1,2 +1,2 @@@",
        "  one",
        "++two",
        "diff --git a/plain b/plain",
        "--- a/plain",
        "+++ b/plain",
        "@@ -1 +1 @@",
        "-a",
        "+b",
    });
    const std::vector<diff_file> files { parse_git_diff(text) };

    REQUIRE(files.size() == 1);
    CHECK(files[0].new_path == "plain");
}

TEST_CASE("parse_git_diff takes paths from the diff header when no marker lines exist", "[git][diff]")
{
    const std::string text = join_lines({
        "diff --git a/dir with space/file b.txt b/dir with space/file b.txt",
        "old mode 100644",
        "new mode 100755",
    });
    const std::vector<diff_file> files { parse_git_diff(text) };

    REQUIRE(files.size() == 1);
    CHECK(files[0].old_path == "dir with space/file b.txt");
    CHECK(files[0].new_path == "dir with space/file b.txt");
}

TEST_CASE("unquote_git_path decodes C style quoting", "[git][diff]")
{
    CHECK(cyella::unquote_git_path("plain.txt") == "plain.txt");
    CHECK(cyella::unquote_git_path("\"a\\tb\\nc\\\\d\\\"e\"") == "a\tb\nc\\d\"e");
    CHECK(cyella::unquote_git_path("\"\\355\\225\\234.txt\"") == "\xED\x95\x9C.txt");
    CHECK(cyella::unquote_git_path("\"\\1\"") == "\x01");
    // 닫히지 않은 따옴표는 그대로 둔다.
    CHECK(cyella::unquote_git_path("\"abc") == "\"abc");
}

TEST_CASE("parse_git_numstat reads plain, binary and renamed entries", "[git][diff]")
{
    const std::vector<cyella::numstat_entry> entries { cyella::parse_git_numstat(cyella::test::read_fixture("numstat_shapes.bin")) };

    REQUIRE(entries.size() == 13);
    CHECK(entries[0].path == "after_rename.txt");
    CHECK(entries[0].original_path == "before_rename.txt");
    CHECK(entries[0].lines_added == 1);
    CHECK(entries[0].lines_removed == 0);

    CHECK(entries[1].path == "blob.bin");
    CHECK(entries[1].is_binary);
    CHECK_FALSE(entries[1].lines_added.has_value());

    CHECK(entries[2].path == "code.c");
    CHECK(entries[2].lines_added == 2);
    CHECK(entries[2].lines_removed == 2);
    CHECK(entries[2].original_path.empty());

    CHECK(entries[9].path == "pure_after.txt");
    CHECK(entries[9].original_path == "pure_before.txt");
    CHECK(entries[12].path == "\xED\x95\x9C\xEA\xB8\x80.txt");
}

TEST_CASE("parse_git_numstat tolerates empty and damaged input", "[git][diff]")
{
    CHECK(cyella::parse_git_numstat({}).empty());

    std::string bytes { "garbage without tabs" };
    bytes.push_back('\0');
    bytes.append("x\ty\tbad.txt");
    bytes.push_back('\0');
    bytes.append("3\t4\tgood.txt");
    bytes.push_back('\0');
    const std::vector<cyella::numstat_entry> entries { cyella::parse_git_numstat(bytes) };

    REQUIRE(entries.size() == 1);
    CHECK(entries[0].path == "good.txt");
}

TEST_CASE("apply_numstat merges line counts into file changes", "[git][diff]")
{
    cyella::git_status_result status { cyella::parse_git_status(cyella::test::read_fixture("status_basic.bin")) };
    const std::vector<cyella::numstat_entry> staged { cyella::parse_git_numstat(cyella::test::read_fixture("numstat_staged.bin")) };
    const std::vector<cyella::numstat_entry> unstaged { cyella::parse_git_numstat(cyella::test::read_fixture("numstat_unstaged.bin")) };

    cyella::apply_numstat(status.staged, staged);
    cyella::apply_numstat(status.unstaged, unstaged);

    REQUIRE(status.staged.size() == 4);
    CHECK(status.staged[0].path == "a.txt");
    CHECK(status.staged[0].lines_added == 1);
    CHECK(status.staged[0].lines_removed == 0);
    CHECK(status.staged[2].path == "gone.txt");
    CHECK(status.staged[2].lines_removed == 1);
    // 이름 변경은 새 경로로 맞춘다.
    CHECK(status.staged[3].path == "new name.txt");
    CHECK(status.staged[3].lines_added == 0);

    // unstaged 쪽: dir/b.txt는 한 줄 바뀜. untracked는 numstat이 없어 값이 비어 있다.
    REQUIRE(status.unstaged.size() == 5);
    CHECK(status.unstaged[1].path == "dir/b.txt");
    CHECK(status.unstaged[1].lines_added == 1);
    CHECK(status.unstaged[1].lines_removed == 1);
    CHECK_FALSE(status.unstaged[3].lines_added.has_value());
}
