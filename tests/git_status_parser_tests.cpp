#include "infrastructure/git_status_parser.h"

#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using cyella::change_side;
    using cyella::file_change;
    using cyella::git_status_result;
    using cyella::parse_git_status;

    constexpr std::string_view head_sha { "c1bf80d6a7260e533f88e475f0d4c611194766c2" };

    // NUL로 레코드를 끝내 -z 출력을 흉내 낸다.
    std::string nul_terminated(const std::initializer_list<std::string_view> records)
    {
        std::string bytes {};
        for (const std::string_view record : records)
        {
            bytes.append(record);
            bytes.push_back('\0');
        }
        return bytes;
    }

    const file_change* find_change(const std::vector<file_change>& changes, const std::string_view path)
    {
        for (const file_change& change : changes)
            if (change.path == path)
                return &change;
        return nullptr;
    }
} // namespace

TEST_CASE("parse_git_status reads branch headers", "[git][status]")
{
    const git_status_result result { parse_git_status(cyella::test::read_fixture("status_basic.bin")) };

    CHECK(result.branch.oid == head_sha);
    CHECK_FALSE(result.branch.is_initial);
    CHECK(result.branch.head == "main");
    CHECK_FALSE(result.branch.is_detached);
    CHECK(result.branch.upstream == "origin/main");
    CHECK(result.branch.has_ahead_behind);
    CHECK(result.branch.ahead == 2);
    CHECK(result.branch.behind == 1);
    CHECK(result.skipped_entry_count == 0);
}

TEST_CASE("parse_git_status splits tracked changes into staged and unstaged", "[git][status]")
{
    const git_status_result result { parse_git_status(cyella::test::read_fixture("status_basic.bin")) };

    REQUIRE(result.staged.size() == 4);
    CHECK(result.staged[0].path == "a.txt");
    CHECK(result.staged[0].status == 'M');
    CHECK(result.staged[0].side == change_side::staged);
    CHECK(result.staged[1].path == "added.txt");
    CHECK(result.staged[1].status == 'A');
    CHECK(result.staged[2].path == "gone.txt");
    CHECK(result.staged[2].status == 'D');

    // 같은 파일이 양쪽에 변경을 가지면 한쪽씩 나온다.
    const file_change* unstaged_added { find_change(result.unstaged, "added.txt") };
    REQUIRE(unstaged_added != nullptr);
    CHECK(unstaged_added->status == 'M');
    CHECK(unstaged_added->side == change_side::unstaged);

    const file_change* unstaged_b { find_change(result.unstaged, "dir/b.txt") };
    REQUIRE(unstaged_b != nullptr);
    CHECK(unstaged_b->status == 'M');
    CHECK(find_change(result.staged, "dir/b.txt") == nullptr);
}

TEST_CASE("parse_git_status keeps the original path of a staged rename with spaces", "[git][status]")
{
    const git_status_result result { parse_git_status(cyella::test::read_fixture("status_basic.bin")) };

    const file_change* renamed { find_change(result.staged, "new name.txt") };
    REQUIRE(renamed != nullptr);
    CHECK(renamed->status == 'R');
    CHECK(renamed->original_path == "old_name.txt");

    // 작업 트리 쪽 수정에는 원래 경로가 없다.
    const file_change* modified { find_change(result.unstaged, "new name.txt") };
    REQUIRE(modified != nullptr);
    CHECK(modified->status == 'M');
    CHECK(modified->original_path.empty());

    // 원래 경로 레코드가 별도 항목으로 새어 나가면 안 된다.
    CHECK(find_change(result.staged, "old_name.txt") == nullptr);
    CHECK(find_change(result.unstaged, "old_name.txt") == nullptr);
}

TEST_CASE("parse_git_status reports untracked and ignored files", "[git][status]")
{
    const git_status_result result { parse_git_status(cyella::test::read_fixture("status_basic.bin")) };

    const file_change* untracked { find_change(result.unstaged, "untracked.txt") };
    REQUIRE(untracked != nullptr);
    CHECK(untracked->status == '?');
    CHECK(untracked->side == change_side::unstaged);
    // --untracked-files=all이므로 폴더가 아니라 안의 파일이 나온다.
    CHECK(find_change(result.unstaged, "untracked_dir/inner.txt") != nullptr);

    REQUIRE(result.ignored.size() == 1);
    CHECK(result.ignored[0].path == "ignore_me.log");
    CHECK(result.ignored[0].status == '!');
    CHECK(result.unstaged.size() == 5);
}

TEST_CASE("parse_git_status lists merge conflicts apart from the other changes", "[git][status]")
{
    const git_status_result result { parse_git_status(cyella::test::read_fixture("status_conflict.bin")) };

    REQUIRE(result.conflicts.size() == 4);
    CHECK(result.staged.empty());
    CHECK(result.unstaged.empty());
    CHECK(result.conflicts[0].path == "both.txt");
    CHECK(result.conflicts[0].status == 'U');
    CHECK(result.conflicts[0].conflict_code == "UU");
    CHECK(result.conflicts[1].conflict_code == "AA");
    CHECK(result.conflicts[2].conflict_code == "UD");
    CHECK(result.conflicts[3].conflict_code == "DU");
    CHECK(result.conflicts[3].path == "del_theirs.txt");
}

TEST_CASE("parse_git_status handles detached HEAD without upstream", "[git][status]")
{
    const git_status_result result { parse_git_status(cyella::test::read_fixture("status_detached.bin")) };

    CHECK(result.branch.is_detached);
    CHECK(result.branch.head.empty());
    CHECK(result.branch.oid == head_sha);
    CHECK(result.branch.upstream.empty());
    CHECK_FALSE(result.branch.has_ahead_behind);
    CHECK(result.staged.empty());
}

TEST_CASE("parse_git_status handles a repository without commits", "[git][status]")
{
    const git_status_result result { parse_git_status(cyella::test::read_fixture("status_initial.bin")) };

    CHECK(result.branch.is_initial);
    CHECK(result.branch.oid.empty());
    CHECK(result.branch.head == "main");
}

TEST_CASE("parse_git_status handles an empty and a clean status", "[git][status]")
{
    const git_status_result empty { parse_git_status({}) };
    CHECK(empty.staged.empty());
    CHECK(empty.skipped_entry_count == 0);

    const git_status_result clean { parse_git_status(cyella::test::read_fixture("status_clean.bin")) };
    CHECK(clean.staged.empty());
    CHECK(clean.unstaged.empty());
    CHECK(clean.conflicts.empty());
}

TEST_CASE("parse_git_status skips malformed records instead of misreading them", "[git][status]")
{
    const std::string bytes = nul_terminated({
        "# branch.head main",
        "1 M. N... 100644 100644 100644 aaaa bbbb ok.txt",
        "1 M. truncated",
        "x unknown record",
        "2 R. N... 100644 100644 100644 aaaa bbbb R100 renamed.txt",
    });
    const git_status_result result { parse_git_status(bytes) };

    REQUIRE(result.staged.size() == 1);
    CHECK(result.staged[0].path == "ok.txt");
    // 잘린 항목, 모르는 종류, 원래 경로가 빠진 rename
    CHECK(result.skipped_entry_count == 3);
}

TEST_CASE("parse_git_status keeps paths with spaces and non-ASCII bytes verbatim", "[git][status]")
{
    const std::string bytes = nul_terminated({
        "? dir with space/\xED\x95\x9C\xEA\xB8\x80 file.txt",
        "1 .M N... 100644 100644 100644 aaaa bbbb a  double space.txt",
    });
    const git_status_result result { parse_git_status(bytes) };

    REQUIRE(result.unstaged.size() == 2);
    CHECK(result.unstaged[0].path == "dir with space/\xED\x95\x9C\xEA\xB8\x80 file.txt");
    CHECK(result.unstaged[1].path == "a  double space.txt");
}

TEST_CASE("parse_git_status marks submodules", "[git][status]")
{
    const std::string bytes = nul_terminated({
        "1 .M S.M. 160000 160000 160000 aaaa bbbb libs/sub",
    });
    const git_status_result result { parse_git_status(bytes) };

    REQUIRE(result.unstaged.size() == 1);
    CHECK(result.unstaged[0].is_submodule);
}

TEST_CASE("parse_git_status leaves ahead and behind unset when the upstream is gone", "[git][status]")
{
    const std::string bytes = nul_terminated({
        "# branch.oid c1bf80d6a7260e533f88e475f0d4c611194766c2",
        "# branch.head topic",
        "# branch.upstream origin/topic",
    });
    const git_status_result result { parse_git_status(bytes) };

    CHECK(result.branch.upstream == "origin/topic");
    CHECK_FALSE(result.branch.has_ahead_behind);
}
