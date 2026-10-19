#include "infrastructure/git_command_builder.h"

#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stdexcept>
#include <string>
#include <vector>

namespace {
    using cyella::test::to_text;
    using text_list = std::vector<std::string>;

    // 인자 목록 어디에도 NUL이 없고 첫 인자가 비어 있지 않다는 모든 조립기의 공통 약속이다.
    void check_wellformed(const cyella::git_arguments& arguments)
    {
        REQUIRE_FALSE(arguments.empty());
        for (const std::u8string& argument : arguments)
            CHECK(argument.find(u8'\0') == std::u8string::npos);
    }
} // namespace

using namespace cyella;

TEST_CASE("query builders produce the commands the parsers expect", "[git][builder]")
{
    CHECK(to_text(make_git_rev_parse_args()) == text_list { "rev-parse", "--absolute-git-dir", "--show-toplevel" });
    CHECK(to_text(make_git_status_args()) == text_list { "status", "--porcelain=v2", "--branch", "-z", "--untracked-files=all" });
    CHECK(to_text(make_git_status_args(true)) == text_list { "status", "--porcelain=v2", "--branch", "-z", "--untracked-files=all", "--ignored" });
    CHECK(to_text(make_git_stash_list_args()) == text_list { "stash", "list", "-z", "--format=%gd%x1f%H%x1f%P%x1f%ct%x1f%gs" });

    const text_list refs { to_text(make_git_for_each_ref_args()) };
    REQUIRE(refs.size() == 6);
    CHECK(refs[0] == "for-each-ref");
    CHECK(refs[1] == "--format=%(refname)%1f%(objectname)%1f%(*objectname)%1f%(HEAD)%1f%(upstream)%1f%(upstream:short)%1f%(upstream:track)%1f%(symref)");
    CHECK(refs[2] == "--");
    CHECK(refs[3] == "refs/heads");
    CHECK(refs[4] == "refs/remotes");
    CHECK(refs[5] == "refs/tags");
}

TEST_CASE("make_git_log_args pages with max-count and skip", "[git][builder]")
{
    const text_list full { to_text(make_git_log_args()) };
    CHECK(full == text_list { "log", "--topo-order", "-z", "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%ct%x1f%s", "--branches", "--remotes", "--tags", "HEAD", "--" });

    git_log_options paged {};
    paged.max_count = 500;
    paged.skip = 1000;
    paged.include_head = false;
    paged.include_body = true;
    CHECK(to_text(make_git_log_args(paged))
        == text_list { "log", "--topo-order", "-z", "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%ct%x1f%s%x1f%b", "--branches", "--remotes", "--tags", "--max-count=500", "--skip=1000", "--" });
}

TEST_CASE("make_git_show_commit_args shows the first-parent diff for merges only", "[git][builder]")
{
    const std::string sha { "c1bf80d6a7260e533f88e475f0d4c611194766c2" };
    CHECK(to_text(make_git_show_commit_args(sha, false)) == text_list { "show", "--format=", "--no-color", "--no-ext-diff", "--src-prefix=a/", "--dst-prefix=b/", "-U3", "--find-renames", sha, "--" });
    CHECK(to_text(make_git_show_commit_args(sha, true, 5))
        == text_list { "show", "--format=", "-m", "--first-parent", "--no-color", "--no-ext-diff", "--src-prefix=a/", "--dst-prefix=b/", "-U5", "--find-renames", sha, "--" });
    CHECK(to_text(make_git_show_numstat_args(sha, true)) == text_list { "show", "--format=", "--numstat", "-z", "--find-renames", "-m", "--first-parent", sha, "--" });
}

TEST_CASE("diff builders pin the output format and treat paths literally", "[git][builder]")
{
    CHECK(to_text(make_git_diff_args("dir/*.txt", diff_source::unstaged))
        == text_list { "--literal-pathspecs", "diff", "--no-color", "--no-ext-diff", "--src-prefix=a/", "--dst-prefix=b/", "-U3", "--", "dir/*.txt" });
    CHECK(to_text(make_git_diff_args("-weird", diff_source::staged, 0))
        == text_list { "--literal-pathspecs", "diff", "--no-color", "--no-ext-diff", "--src-prefix=a/", "--dst-prefix=b/", "-U0", "--cached", "--", "-weird" });
    CHECK(to_text(make_git_diff_untracked_args("new.txt"))
        == text_list { "diff", "--no-index", "--no-color", "--no-ext-diff", "--src-prefix=a/", "--dst-prefix=b/", "-U3", "--", "/dev/null", "new.txt" });
    CHECK(to_text(make_git_diff_numstat_args(diff_source::unstaged)) == text_list { "diff", "--numstat", "-z" });
    CHECK(to_text(make_git_diff_numstat_args(diff_source::staged)) == text_list { "diff", "--numstat", "-z", "--cached", "-M" });
    CHECK(to_text(make_git_ls_remote_tags_args("origin")) == text_list { "ls-remote", "--tags", "--", "origin" });
}

TEST_CASE("branch builders put the names after double dash", "[git][builder]")
{
    CHECK(to_text(make_git_switch_args("main")) == text_list { "switch", "--", "main" });
    CHECK(to_text(make_git_switch_detach_args("c1bf80d")) == text_list { "switch", "--detach", "--", "c1bf80d" });
    CHECK(to_text(make_git_switch_create_args("topic")) == text_list { "switch", "-c", "topic" });
    CHECK(to_text(make_git_switch_create_args("topic", "origin/topic", true)) == text_list { "switch", "-c", "topic", "--track", "--", "origin/topic" });
    CHECK(to_text(make_git_branch_create_args("topic", "HEAD~2")) == text_list { "branch", "--", "topic", "HEAD~2" });
    CHECK(to_text(make_git_branch_rename_args("old", "new")) == text_list { "branch", "-m", "--", "old", "new" });
    CHECK(to_text(make_git_branch_delete_args("old", false)) == text_list { "branch", "-d", "--", "old" });
    CHECK(to_text(make_git_branch_delete_args("old", true)) == text_list { "branch", "-D", "--", "old" });
}

TEST_CASE("commit builders read the message from stdin", "[git][builder]")
{
    CHECK(to_text(make_git_commit_args()) == text_list { "commit", "-F", "-" });

    git_commit_options amend {};
    amend.amend = true;
    CHECK(to_text(make_git_commit_args(amend)) == text_list { "commit", "--amend", "-F", "-" });

    amend.keep_message = true;
    CHECK(to_text(make_git_commit_args(amend)) == text_list { "commit", "--amend", "--no-edit" });

    git_commit_options invalid {};
    invalid.keep_message = true;
    CHECK_THROWS_AS(make_git_commit_args(invalid), std::invalid_argument);
}

TEST_CASE("index builders end options before the paths", "[git][builder]")
{
    const std::vector<std::string> paths { "a.txt", "dir/b c.txt", "-dash.txt" };
    CHECK(to_text(make_git_add_args(paths)) == text_list { "--literal-pathspecs", "add", "--", "a.txt", "dir/b c.txt", "-dash.txt" });
    CHECK(to_text(make_git_restore_staged_args(paths)) == text_list { "--literal-pathspecs", "restore", "--staged", "--", "a.txt", "dir/b c.txt", "-dash.txt" });
    CHECK(to_text(make_git_restore_worktree_args(paths)) == text_list { "--literal-pathspecs", "restore", "--", "a.txt", "dir/b c.txt", "-dash.txt" });
    CHECK(to_text(make_git_add_from_stdin_args()) == text_list { "--literal-pathspecs", "add", "--pathspec-from-file=-", "--pathspec-file-nul" });
    CHECK(to_text(make_git_restore_staged_from_stdin_args()) == text_list { "--literal-pathspecs", "restore", "--staged", "--pathspec-from-file=-", "--pathspec-file-nul" });
    CHECK(to_text(make_git_restore_worktree_from_stdin_args()) == text_list { "--literal-pathspecs", "restore", "--pathspec-from-file=-", "--pathspec-file-nul" });

    // 경로 없이 호출하면 모든 변경을 건드리게 되므로 거절한다.
    const std::vector<std::string> none {};
    CHECK_THROWS_AS(make_git_add_args(none), std::invalid_argument);
    CHECK_THROWS_AS(make_git_restore_worktree_args(none), std::invalid_argument);
    const std::vector<std::string> empty_path { "" };
    CHECK_THROWS_AS(make_git_add_args(empty_path), std::invalid_argument);
}

TEST_CASE("make_git_apply_args reads the patch from stdin", "[git][builder]")
{
    CHECK(to_text(make_git_apply_args()) == text_list { "apply", "--cached", "-" });

    git_apply_options options {};
    options.reverse = true;
    options.check_only = true;
    options.recount = true;
    options.unidiff_zero = true;
    CHECK(to_text(make_git_apply_args(options)) == text_list { "apply", "--cached", "-R", "--check", "--recount", "--unidiff-zero", "-" });

    options.cached = false;
    options.reverse = false;
    options.check_only = false;
    options.recount = false;
    options.unidiff_zero = false;
    CHECK(to_text(make_git_apply_args(options)) == text_list { "apply", "-" });
}

TEST_CASE("remote builders add progress and keep remotes after double dash", "[git][builder]")
{
    CHECK(to_text(make_git_fetch_args()) == text_list { "fetch", "--prune", "--progress" });

    git_fetch_options fetch {};
    fetch.remote = "origin";
    fetch.tags = true;
    CHECK(to_text(make_git_fetch_args(fetch)) == text_list { "fetch", "--prune", "--progress", "--tags", "--", "origin" });
    fetch.all = true;
    CHECK(to_text(make_git_fetch_args(fetch)) == text_list { "fetch", "--prune", "--progress", "--tags", "--all" });

    CHECK(to_text(make_git_pull_args(git_pull_mode::fast_forward_only)) == text_list { "pull", "--ff-only" });
    CHECK(to_text(make_git_pull_args(git_pull_mode::rebase, "origin", "main")) == text_list { "pull", "--rebase", "--", "origin", "main" });
    CHECK_THROWS_AS(make_git_pull_args(git_pull_mode::rebase, "", "main"), std::invalid_argument);

    CHECK(to_text(make_git_push_args()) == text_list { "push", "--progress" });
    git_push_options push {};
    push.remote = "origin";
    push.refspec = "main";
    push.set_upstream = true;
    push.follow_tags = true;
    push.force_with_lease = true;
    CHECK(to_text(make_git_push_args(push)) == text_list { "push", "--progress", "--set-upstream", "--follow-tags", "--force-with-lease", "--", "origin", "main" });
    git_push_options refspec_only {};
    refspec_only.refspec = "main";
    CHECK_THROWS_AS(make_git_push_args(refspec_only), std::invalid_argument);
}

TEST_CASE("tag builders qualify tag names for the remote", "[git][builder]")
{
    CHECK(to_text(make_git_tag_args("v1.0")) == text_list { "tag", "--", "v1.0" });
    CHECK(to_text(make_git_tag_args("v1.0", "c1bf80d")) == text_list { "tag", "--", "v1.0", "c1bf80d" });
    CHECK(to_text(make_git_annotated_tag_args("v1.0", "HEAD~1")) == text_list { "tag", "-a", "-F", "-", "--", "v1.0", "HEAD~1" });
    CHECK(to_text(make_git_delete_tag_args("v1.0")) == text_list { "tag", "-d", "--", "v1.0" });
    CHECK(to_text(make_git_push_tag_args("origin", "v1.0")) == text_list { "push", "--progress", "--", "origin", "refs/tags/v1.0" });
    CHECK(to_text(make_git_push_all_tags_args("origin")) == text_list { "push", "--progress", "--tags", "--", "origin" });
    CHECK(to_text(make_git_delete_remote_tag_args("origin", "v1.0")) == text_list { "push", "--progress", "--delete", "--", "origin", "refs/tags/v1.0" });
    CHECK(to_text(make_git_delete_remote_branch_args("origin", "topic")) == text_list { "push", "--progress", "--delete", "--", "origin", "refs/heads/topic" });
}

TEST_CASE("merge and rebase builders", "[git][builder]")
{
    CHECK(to_text(make_git_merge_args("topic")) == text_list { "merge", "--no-edit", "--", "topic" });
    CHECK(to_text(make_git_merge_args("topic", git_merge_mode::fast_forward_only)) == text_list { "merge", "--no-edit", "--ff-only", "--", "topic" });
    CHECK(to_text(make_git_merge_args("topic", git_merge_mode::no_fast_forward)) == text_list { "merge", "--no-edit", "--no-ff", "--", "topic" });
    CHECK(to_text(make_git_merge_abort_args()) == text_list { "merge", "--abort" });

    CHECK(to_text(make_git_rebase_args("main")) == text_list { "rebase", "--", "main" });
    CHECK(to_text(make_git_rebase_args("main", "topic", true)) == text_list { "rebase", "--autostash", "--", "main", "topic" });
    CHECK(to_text(make_git_rebase_continue_args()) == text_list { "-c", "core.editor=true", "rebase", "--continue" });
    CHECK(to_text(make_git_rebase_skip_args()) == text_list { "rebase", "--skip" });
    CHECK(to_text(make_git_rebase_abort_args()) == text_list { "rebase", "--abort" });
}

TEST_CASE("cherry-pick revert and reset builders", "[git][builder]")
{
    const std::vector<std::string> revisions { "aaaa111", "bbbb222" };
    CHECK(to_text(make_git_cherry_pick_args(revisions)) == text_list { "cherry-pick", "--", "aaaa111", "bbbb222" });
    CHECK(to_text(make_git_cherry_pick_args(revisions, 1)) == text_list { "cherry-pick", "-m", "1", "--", "aaaa111", "bbbb222" });
    CHECK(to_text(make_git_cherry_pick_continue_args()) == text_list { "-c", "core.editor=true", "cherry-pick", "--continue" });
    CHECK(to_text(make_git_cherry_pick_abort_args()) == text_list { "cherry-pick", "--abort" });
    CHECK(to_text(make_git_revert_args(revisions)) == text_list { "revert", "--no-edit", "--", "aaaa111", "bbbb222" });
    CHECK(to_text(make_git_revert_args(revisions, 2)) == text_list { "revert", "--no-edit", "-m", "2", "--", "aaaa111", "bbbb222" });
    CHECK(to_text(make_git_revert_abort_args()) == text_list { "revert", "--abort" });

    // reset의 `--`는 식을 경로로 만들지 않도록 식 뒤에 둔다.
    CHECK(to_text(make_git_reset_args(git_reset_mode::soft, "HEAD~1")) == text_list { "reset", "--soft", "HEAD~1", "--" });
    CHECK(to_text(make_git_reset_args(git_reset_mode::mixed, "HEAD")) == text_list { "reset", "--mixed", "HEAD", "--" });
    CHECK(to_text(make_git_reset_args(git_reset_mode::hard, "origin/main")) == text_list { "reset", "--hard", "origin/main", "--" });

    const std::vector<std::string> none {};
    CHECK_THROWS_AS(make_git_cherry_pick_args(none), std::invalid_argument);
}

TEST_CASE("stash builders select entries by index", "[git][builder]")
{
    CHECK(to_text(make_git_stash_push_args()) == text_list { "stash", "push" });

    git_stash_push_options options {};
    options.message = "-m starts with dash";
    options.include_untracked = true;
    options.keep_index = true;
    CHECK(to_text(make_git_stash_push_args(options)) == text_list { "stash", "push", "--include-untracked", "--keep-index", "-m", "-m starts with dash" });

    CHECK(to_text(make_git_stash_apply_args(2)) == text_list { "stash", "apply", "stash@{2}" });
    CHECK(to_text(make_git_stash_apply_args(0, true)) == text_list { "stash", "apply", "--index", "stash@{0}" });
    CHECK(to_text(make_git_stash_pop_args(0)) == text_list { "stash", "pop", "stash@{0}" });
    CHECK(to_text(make_git_stash_drop_args(11)) == text_list { "stash", "drop", "stash@{11}" });
}

TEST_CASE("clone and init builders", "[git][builder]")
{
    git_clone_options clone {};
    clone.url = "git@example.com:team/repo.git";
    clone.directory = "C:/work/repo";
    CHECK(to_text(make_git_clone_args(clone)) == text_list { "clone", "--progress", "--", "git@example.com:team/repo.git", "C:/work/repo" });

    clone.branch = "dev";
    clone.depth = 1;
    CHECK(to_text(make_git_clone_args(clone)) == text_list { "clone", "--progress", "--branch", "dev", "--depth", "1", "--", "git@example.com:team/repo.git", "C:/work/repo" });

    clone.url = "--upload-pack=calc";
    CHECK_THROWS_AS(make_git_clone_args(clone), std::invalid_argument);

    CHECK(to_text(make_git_init_args("C:/work/new")) == text_list { "init", "--", "C:/work/new" });
    CHECK(to_text(make_git_init_args("C:/work/new", "trunk")) == text_list { "init", "--initial-branch=trunk", "--", "C:/work/new" });
}

TEST_CASE("builders reject names that git would read as options", "[git][builder]")
{
    CHECK_THROWS_AS(make_git_switch_args("--orphan"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_switch_args(""), std::invalid_argument);
    CHECK_THROWS_AS(make_git_switch_detach_args("-f"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_switch_create_args("-bad"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_switch_create_args("topic", "--force"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_branch_rename_args("old", "--new"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_branch_delete_args("-D", false), std::invalid_argument);
    CHECK_THROWS_AS(make_git_merge_args("--abort"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_rebase_args("--root"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_reset_args(git_reset_mode::hard, "--help"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_show_commit_args("--all", false), std::invalid_argument);
    CHECK_THROWS_AS(make_git_push_tag_args("--mirror", "v1"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_ls_remote_tags_args("-h"), std::invalid_argument);
}

TEST_CASE("builders apply the check-ref-format rules to new names", "[git][builder]")
{
    CHECK_THROWS_AS(make_git_switch_create_args("a..b"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_switch_create_args("a b"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_switch_create_args("topic.lock"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_branch_create_args("HEAD"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_tag_args("v1:beta"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_annotated_tag_args("v1~", "HEAD"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_push_tag_args("origin", "bad name"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_delete_remote_tag_args("origin", "a//b"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_init_args("C:/x", "-b"), std::invalid_argument);

    // 이유를 담은 메시지를 던진다.
    CHECK_THROWS_WITH(make_git_switch_create_args("a..b"), Catch::Matchers::ContainsSubstring("branch name"));
    CHECK_THROWS_WITH(make_git_switch_create_args("a..b"), Catch::Matchers::ContainsSubstring(".."));
}

TEST_CASE("every builder output is free of NUL and not empty", "[git][builder]")
{
    const std::vector<std::string> paths { "a.txt" };
    check_wellformed(make_git_rev_parse_args());
    check_wellformed(make_git_status_args());
    check_wellformed(make_git_for_each_ref_args());
    check_wellformed(make_git_log_args());
    check_wellformed(make_git_add_args(paths));
    check_wellformed(make_git_apply_args());
    check_wellformed(make_git_stash_list_args());
}
