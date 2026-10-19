#include "infrastructure/git_stash_parser.h"

#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

namespace {
    using cyella::git_stash_list;
    using cyella::parse_git_stash_list;
} // namespace

TEST_CASE("parse_git_stash_list reads a stash made with a message and untracked files", "[git][stash]")
{
    const git_stash_list list { parse_git_stash_list(cyella::test::read_fixture("stash_list.bin")) };

    REQUIRE(list.entries.size() == 1);
    CHECK(list.skipped_count == 0);
    const cyella::stash_entry& entry { list.entries[0] };
    CHECK(entry.index == 0);
    CHECK(entry.name == "stash@{0}");
    CHECK(entry.sha == "8a65a65f49ceaca9fd0591364c21260e14bb3829");
    CHECK(entry.base_sha == "c1bf80d6a7260e533f88e475f0d4c611194766c2");
    CHECK(entry.branch == "main");
    CHECK(entry.message == "work in progress \xE2\x9C\x93");
    CHECK_FALSE(entry.is_automatic_message);
    CHECK(entry.has_untracked);
    CHECK(entry.commit_time == 1767747600);
}

TEST_CASE("parse_git_stash_list keeps git's order and tells automatic messages apart", "[git][stash]")
{
    const git_stash_list list { parse_git_stash_list(cyella::test::read_fixture("stash_list_two.bin")) };

    REQUIRE(list.entries.size() == 3);
    CHECK(list.entries[0].index == 0);
    CHECK(list.entries[1].index == 1);
    CHECK(list.entries[2].index == 2);

    // 메시지 없이 push하면 git이 `WIP on <branch>: <abbrev> <subject>`를 붙인다.
    CHECK(list.entries[0].is_automatic_message);
    CHECK(list.entries[0].branch == "main");
    CHECK(list.entries[0].message == "c1bf80d Merge branch 'feature'");
    CHECK_FALSE(list.entries[0].has_untracked);

    CHECK_FALSE(list.entries[1].is_automatic_message);
    CHECK(list.entries[1].message == "second stash");
    CHECK(list.entries[2].has_untracked);
}

TEST_CASE("parse_git_stash_list keeps unknown subjects whole", "[git][stash]")
{
    std::string bytes { "stash@{4}" };
    bytes.append("\x1F"
                 "aaaa\x1F"
                 "bbbb\x1F"
                 "10\x1F"
                 "custom text without prefix");
    bytes.push_back('\0');
    const git_stash_list list { parse_git_stash_list(bytes) };

    REQUIRE(list.entries.size() == 1);
    CHECK(list.entries[0].index == 4);
    CHECK(list.entries[0].branch.empty());
    CHECK(list.entries[0].message == "custom text without prefix");
}

TEST_CASE("parse_git_stash_list skips malformed records and accepts an empty list", "[git][stash]")
{
    CHECK(parse_git_stash_list({}).entries.empty());

    std::string bytes { "garbage" };
    bytes.push_back('\0');
    bytes.append("stash@{x}\x1F"
                 "aaaa\x1F"
                 "bbbb\x1F"
                 "10\x1F"
                 "On main: m");
    bytes.push_back('\0');
    const git_stash_list list { parse_git_stash_list(bytes) };

    CHECK(list.entries.empty());
    CHECK(list.skipped_count == 2);
}
