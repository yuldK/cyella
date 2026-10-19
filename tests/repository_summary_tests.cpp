#include "domain/repository_summary.h"
#include "infrastructure/git_command_builder.h"
#include "infrastructure/git_status_parser.h"

#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {
    using cyella::detect_repository_operation_state;
    using cyella::repository_operation_state;

    repository_operation_state detect(const std::vector<std::string>& markers)
    {
        return detect_repository_operation_state(markers);
    }
} // namespace

TEST_CASE("detect_repository_operation_state returns none without markers", "[repository_summary]")
{
    CHECK(detect({}) == repository_operation_state::none);
    CHECK(detect({ "HEAD", "index" }) == repository_operation_state::none);
}

TEST_CASE("detect_repository_operation_state recognizes each marker", "[repository_summary]")
{
    CHECK(detect({ "rebase-merge" }) == repository_operation_state::rebase);
    CHECK(detect({ "rebase-apply" }) == repository_operation_state::rebase);
    CHECK(detect({ "MERGE_HEAD" }) == repository_operation_state::merge);
    CHECK(detect({ "CHERRY_PICK_HEAD" }) == repository_operation_state::cherry_pick);
    CHECK(detect({ "REVERT_HEAD" }) == repository_operation_state::revert);
}

TEST_CASE("detect_repository_operation_state accepts full paths with either separator", "[repository_summary]")
{
    CHECK(detect({ "C:/repo/.git/MERGE_HEAD" }) == repository_operation_state::merge);
    CHECK(detect({ "C:\\repo\\.git\\CHERRY_PICK_HEAD" }) == repository_operation_state::cherry_pick);
    CHECK(detect({ ".git/rebase-merge/" }) == repository_operation_state::rebase);
    // 이름이 비슷할 뿐인 파일은 표지가 아니다.
    CHECK(detect({ ".git/ORIG_MERGE_HEAD" }) == repository_operation_state::none);
    CHECK(detect({ ".git/MERGE_HEAD.bak" }) == repository_operation_state::none);
}

TEST_CASE("detect_repository_operation_state prefers rebase over the picks it makes", "[repository_summary]")
{
    CHECK(detect({ "MERGE_HEAD", "rebase-merge" }) == repository_operation_state::rebase);
    CHECK(detect({ "REVERT_HEAD", "CHERRY_PICK_HEAD" }) == repository_operation_state::cherry_pick);
    CHECK(detect({ "MERGE_HEAD", "REVERT_HEAD" }) == repository_operation_state::revert);
}

TEST_CASE("make_in_progress_marker_paths lists every marker under the git directory", "[repository_summary]")
{
    const std::vector<std::string> paths { cyella::make_in_progress_marker_paths("C:/repo/.git") };
    REQUIRE(paths.size() == cyella::repository_operation_marker_names.size());
    CHECK(paths.front() == "C:/repo/.git/rebase-merge");
    CHECK(paths.back() == "C:/repo/.git/REVERT_HEAD");

    // 끝 구분자가 있든 없든 같은 경로가 나온다.
    CHECK(cyella::make_in_progress_marker_paths("C:/repo/.git/") == paths);
}

TEST_CASE("apply_git_status fills the repository summary", "[repository_summary][git]")
{
    const cyella::git_status_result status { cyella::parse_git_status(cyella::test::read_fixture("status_basic.bin")) };

    cyella::repository_summary summary {};
    summary.path = "C:/repo";
    summary.display_name = "repo";
    cyella::apply_git_status(summary, status);

    CHECK(summary.path == "C:/repo");
    CHECK(summary.current_branch == "main");
    CHECK_FALSE(summary.is_detached);
    CHECK(summary.head_sha == "c1bf80d6a7260e533f88e475f0d4c611194766c2");
    CHECK(summary.has_upstream);
    CHECK(summary.upstream == "origin/main");
    CHECK(summary.ahead == 2);
    CHECK(summary.behind == 1);
    // a.txt added.txt dir/b.txt gone.txt new name.txt untracked.txt untracked_dir/inner.txt
    CHECK(summary.changed_file_count == 7);
    CHECK(summary.conflict_count == 0);
}

TEST_CASE("apply_git_status reports a detached HEAD without a branch", "[repository_summary][git]")
{
    cyella::repository_summary summary {};
    cyella::apply_git_status(summary, cyella::parse_git_status(cyella::test::read_fixture("status_detached.bin")));

    CHECK(summary.is_detached);
    CHECK(summary.current_branch.empty());
    CHECK_FALSE(summary.has_upstream);
    CHECK(summary.head_sha == "c1bf80d6a7260e533f88e475f0d4c611194766c2");
    CHECK(summary.changed_file_count == 0);
}
