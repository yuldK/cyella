#include "domain/path_syntax.h"

#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace cyella;

TEST_CASE("a gitignore literal escapes pattern letters and trailing spaces", "[domain][path]")
{
    CHECK(gitignore_literal("a.txt") == "a.txt");
    CHECK(gitignore_literal("src/a[1].txt") == "src/a\\[1].txt");
    CHECK(gitignore_literal("*?\\") == "\\*\\?\\\\");
    // 끝의 공백은 git이 지우므로 하나씩 지킨다. 가운데 공백은 그대로다.
    CHECK(gitignore_literal("a b  ") == "a b\\ \\ ");
    CHECK(gitignore_literal("#x") == "#x");
}

TEST_CASE("integration: a gitignore literal ignores only that one file", "[domain][path][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not available");
    const test::temp_directory directory {};
    REQUIRE(test::run_git(directory.path(), std::vector<std::string> { "init", "-q", "-b", "main" }).exit_code == 0);
    // 이스케이프하지 않으면 `a[1].txt`는 문자 묶음 패턴이라 `a1.txt`를 무시하고 정작 그 파일은 계속 보인다.
    test::write_file(directory.path() / "a[1].txt", "x");
    test::write_file(directory.path() / "a1.txt", "x");
    test::write_file(directory.path() / ".gitignore", "/" + gitignore_literal("a[1].txt") + "\n");
    const test::git_run_result status { test::run_git(directory.path(), std::vector<std::string> { "status", "--porcelain", "--untracked-files=all" }) };
    REQUIRE(status.exit_code == 0);
    CHECK(status.standard_output.find("a[1].txt") == std::string::npos);
    CHECK(status.standard_output.find("?? a1.txt") != std::string::npos);
}

TEST_CASE("workspace children join the root with backslashes", "[domain][path]")
{
    CHECK(resolve_workspace_child(u8"C:\\work\\repo", "a.txt") == std::u8string { u8"C:\\work\\repo\\a.txt" });
    CHECK(resolve_workspace_child(u8"C:\\work\\repo\\", "src/deep/new file.txt") == std::u8string { u8"C:\\work\\repo\\src\\deep\\new file.txt" });
    CHECK(resolve_workspace_child(u8"C:/work/repo", "한글/파일.txt") == std::u8string { u8"C:/work/repo\\한글\\파일.txt" });
    // 이름이 `.git`으로 시작해도 `.git` 자체가 아니면 허용한다.
    CHECK(resolve_workspace_child(u8"C:\\work\\repo", ".gitignore").has_value());
}

TEST_CASE("workspace children refuse paths that leave the repository or touch .git", "[domain][path]")
{
    const std::u8string root { u8"C:\\work\\repo" };
    CHECK_FALSE(resolve_workspace_child(root, "").has_value());
    CHECK_FALSE(resolve_workspace_child(root, "../outside.txt").has_value());
    CHECK_FALSE(resolve_workspace_child(root, "a/../../outside.txt").has_value());
    CHECK_FALSE(resolve_workspace_child(root, "a\\..\\b").has_value());
    CHECK_FALSE(resolve_workspace_child(root, "./a.txt").has_value());
    CHECK_FALSE(resolve_workspace_child(root, "/a.txt").has_value());
    CHECK_FALSE(resolve_workspace_child(root, "a//b.txt").has_value());
    CHECK_FALSE(resolve_workspace_child(root, "dir/").has_value());
    CHECK_FALSE(resolve_workspace_child(root, "C:/Windows/system.ini").has_value());
    CHECK_FALSE(resolve_workspace_child(root, "C:evil.txt").has_value());
    CHECK_FALSE(resolve_workspace_child(root, ".git").has_value());
    CHECK_FALSE(resolve_workspace_child(root, ".GIT/config").has_value());
    CHECK_FALSE(resolve_workspace_child(root, "sub/.git/x").has_value());
    CHECK_FALSE(resolve_workspace_child(root, "file.txt:stream").has_value());
    CHECK_FALSE(resolve_workspace_child(root, std::string { "a\0b", 3 }).has_value());
    // NTFS는 끝의 점과 공백을 떼고 8.3 짧은 이름으로도 `.git`을 가리킨다.
    CHECK_FALSE(resolve_workspace_child(root, ".git.").has_value());
    CHECK_FALSE(resolve_workspace_child(root, ".git ").has_value());
    CHECK_FALSE(resolve_workspace_child(root, ".GIT../config").has_value());
    CHECK_FALSE(resolve_workspace_child(root, "GIT~1/config").has_value());
    CHECK_FALSE(resolve_workspace_child(root, "sub/git~12").has_value());
    CHECK_FALSE(resolve_workspace_child(root, "...").has_value());
    CHECK_FALSE(resolve_workspace_child(root, "a/.../b").has_value());
    CHECK_FALSE(resolve_workspace_child(root, "name.").has_value());
    // 비슷해 보여도 다른 이름은 막지 않는다.
    CHECK(resolve_workspace_child(root, "a.txt.bak").has_value());
    CHECK(resolve_workspace_child(root, "git~backup").has_value());
    CHECK(resolve_workspace_child(root, "git~").has_value());
    CHECK(resolve_workspace_child(root, "my git~1.txt").has_value());
    // 루트 자체가 절대 경로가 아니면 합치지 않는다.
    CHECK_FALSE(resolve_workspace_child(u8"relative", "a.txt").has_value());
    CHECK_FALSE(resolve_workspace_child(u8"", "a.txt").has_value());
}

TEST_CASE("absolute windows paths need a drive, a share or an extended drive prefix", "[domain][path]")
{
    CHECK(is_absolute_windows_path(u8"C:\\work"));
    CHECK(is_absolute_windows_path(u8"c:/work"));
    CHECK(is_absolute_windows_path(u8"\\\\server\\share\\x"));
    CHECK(is_absolute_windows_path(u8"\\\\server\\share"));
    CHECK(is_absolute_windows_path(u8"//server/share/x"));
    CHECK(is_absolute_windows_path(u8"\\\\?\\C:\\x"));
    CHECK(is_absolute_windows_path(u8"//?/C:/x"));
    CHECK(is_absolute_windows_path(u8"\\\\?\\UNC\\server\\share\\x"));
    CHECK(is_absolute_windows_path(u8"\\\\?\\unc\\server\\share"));
    // drive 기준 상대 경로, 현재 drive에 의존하는 경로, 빈 값은 절대 경로가 아니다.
    CHECK_FALSE(is_absolute_windows_path(u8"C:relative"));
    CHECK_FALSE(is_absolute_windows_path(u8"\\rooted"));
    CHECK_FALSE(is_absolute_windows_path(u8""));
    // 공유 이름이 없는 UNC와 장치·특수 이름 공간은 CreateProcess에 넘길 경로가 아니다.
    CHECK_FALSE(is_absolute_windows_path(u8"\\\\server"));
    CHECK_FALSE(is_absolute_windows_path(u8"\\\\server\\"));
    CHECK_FALSE(is_absolute_windows_path(u8"\\\\\\server\\share"));
    CHECK_FALSE(is_absolute_windows_path(u8"\\\\.\\PhysicalDrive0"));
    CHECK_FALSE(is_absolute_windows_path(u8"//./pipe/x"));
    CHECK_FALSE(is_absolute_windows_path(u8"\\\\?\\GLOBALROOT\\x"));
    CHECK_FALSE(is_absolute_windows_path(u8"\\\\?\\C:"));
    CHECK_FALSE(is_absolute_windows_path(u8"\\\\?\\UNC\\server"));
}
