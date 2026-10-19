#include "infrastructure/git_query_parser.h"

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace cyella;

TEST_CASE("parse_git_remotes merges fetch and push lines per remote", "[git][query]")
{
    const std::string bytes = "origin\tgit@github.com:me/repo.git (fetch)\n"
                              "origin\tgit@github.com:me/repo.git (push)\n"
                              "upstream\thttps://example.com/path with space/repo.git (fetch)\n"
                              "upstream\thttps://example.com/push.git (push)\r\n";
    const std::vector<remote_info> remotes { parse_git_remotes(bytes) };

    REQUIRE(remotes.size() == 2);
    CHECK(remotes[0].name == "origin");
    CHECK(remotes[0].fetch_url == "git@github.com:me/repo.git");
    CHECK(remotes[0].push_url == "git@github.com:me/repo.git");
    CHECK(remotes[1].name == "upstream");
    CHECK(remotes[1].fetch_url == "https://example.com/path with space/repo.git");
    CHECK(remotes[1].push_url == "https://example.com/push.git");
    CHECK(parse_git_remotes("").empty());
}

TEST_CASE("parse_overwritten_files reads the tab-indented list after the git error", "[git][query]")
{
    // LC_ALL=C에서 git 2.52가 낸 문구 그대로다.
    const std::string tracked = "error: Your local changes to the following files would be overwritten by checkout:\n"
                                "\ta.txt\n"
                                "\tdir/한글 파일.txt\n"
                                "Please commit your changes or stash them before you switch branches.\n"
                                "Aborting\n";
    const overwritten_files files { parse_overwritten_files(tracked) };
    CHECK(files.files == std::vector<std::string> { "a.txt", "dir/한글 파일.txt" });
    CHECK_FALSE(files.untracked);

    const std::string untracked = "error: The following untracked working tree files would be overwritten by checkout:\r\n"
                                  "\tf.txt\r\n"
                                  "Please move or remove them before you switch branches.\r\n";
    const overwritten_files blocked { parse_overwritten_files(untracked) };
    CHECK(blocked.files == std::vector<std::string> { "f.txt" });
    CHECK(blocked.untracked);
    CHECK(parse_overwritten_files("fatal: not a git repository").files.empty());
}

TEST_CASE("parse_git_name_status keeps renames and the leading newline git may print", "[git][query]")
{
    using namespace std::string_view_literals;
    const std::string_view bytes { "\nM\0a.txt\0R100\0old name.txt\0new name.txt\0A\0한글 파일.txt\0D\0gone.txt\0"sv };
    const std::vector<file_change> changes { parse_git_name_status(bytes) };

    REQUIRE(changes.size() == 4);
    CHECK(changes[0].status == 'M');
    CHECK(changes[0].path == "a.txt");
    CHECK(changes[1].status == 'R');
    CHECK(changes[1].original_path == "old name.txt");
    CHECK(changes[1].path == "new name.txt");
    CHECK(changes[2].status == 'A');
    CHECK(changes[2].path == "한글 파일.txt");
    CHECK(changes[3].status == 'D');
    CHECK(changes[3].path == "gone.txt");
}

TEST_CASE("read_in_progress_operation reads the rebase step files", "[git][query]")
{
    // 실제 충돌한 rebase(git 2.52)의 `.git/rebase-merge` 내용이다.
    const std::map<std::string, std::string> files {
        { "rebase-merge/msgnum", "2\n" },
        { "rebase-merge/end", "3\n" },
        { "rebase-merge/onto", "85bed87523af002c3c36633ebeb6295417ca6006\n" },
        { "rebase-merge/head-name", "refs/heads/feature\n" },
        { "rebase-merge/message", "feat two\n\nbody line\n" },
    };
    const auto read = [&files](const std::string_view relative) -> std::optional<std::string> {
        const auto found { files.find(std::string { relative }) };
        if (found == files.end())
            return std::nullopt;
        return found->second;
    };

    const in_progress_operation rebase { read_in_progress_operation(repository_operation_state::rebase, read) };
    CHECK(rebase.kind == repository_operation_state::rebase);
    CHECK(rebase.step == 2);
    CHECK(rebase.total == 3);
    CHECK(rebase.onto == "85bed87523af002c3c36633ebeb6295417ca6006");
    CHECK(rebase.head_name == "refs/heads/feature");
    CHECK(rebase.current_summary == "feat two");

    const in_progress_operation none { read_in_progress_operation(repository_operation_state::none, read) };
    CHECK(none.kind == repository_operation_state::none);
    CHECK(none.step == 0);
}

TEST_CASE("read_in_progress_operation reads the merge message for merge and cherry-pick", "[git][query]")
{
    const auto read = [](const std::string_view relative) -> std::optional<std::string> {
        if (relative == "MERGE_MSG")
            return std::string { "Merge branch 'feature'\n\n# Conflicts:\n#\ta.txt\n" };
        if (relative == "rebase-apply/next")
            return std::string { "1\n" };
        return std::nullopt;
    };

    const in_progress_operation merge { read_in_progress_operation(repository_operation_state::merge, read) };
    CHECK(merge.kind == repository_operation_state::merge);
    CHECK(merge.current_summary == "Merge branch 'feature'");
    CHECK(merge.step == 0);

    // apply 방식 rebase는 다른 파일 이름을 쓴다. detached에서 시작했으면 head-name이 없다.
    const in_progress_operation applied { read_in_progress_operation(repository_operation_state::rebase, read) };
    CHECK(applied.step == 1);
    CHECK(applied.total == 0);
    CHECK(applied.head_name.empty());
}
