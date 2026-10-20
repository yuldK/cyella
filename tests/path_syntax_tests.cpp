#include "domain/path_syntax.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace cyella;

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
    // 루트 자체가 절대 경로가 아니면 합치지 않는다.
    CHECK_FALSE(resolve_workspace_child(u8"relative", "a.txt").has_value());
    CHECK_FALSE(resolve_workspace_child(u8"", "a.txt").has_value());
}
