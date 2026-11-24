#include "infrastructure/git_command_builder.h"

#include "git_test_support.h"
#include "native_path_support.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <stdexcept>
#include <string>
#include <vector>

using cyella::test::native_path;

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
    paged.graph.include_head = false;
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

TEST_CASE("the staged patch builder covers the whole index and excludes paths literally", "[git][builder]")
{
    CHECK(to_text(make_git_staged_patch_args())
        == text_list { "diff", "--no-color", "--no-ext-diff", "--src-prefix=a/", "--dst-prefix=b/", "-U3", "--cached", "-M", "--", ":(top)" });
    // glob 글자와 `:`로 시작하는 이름도 `literal`이라 글자 그대로 빠진다.
    const std::vector<std::string> excluded { ".env", "web/*.lock", ":odd" };
    CHECK(to_text(make_git_staged_patch_args(excluded))
        == text_list {
            "diff", "--no-color", "--no-ext-diff", "--src-prefix=a/", "--dst-prefix=b/", "-U3", "--cached", "-M", "--", ":(top)",
            ":(top,exclude,literal).env", ":(top,exclude,literal)web/*.lock", ":(top,exclude,literal):odd",
        });
    const std::vector<std::string> empty_path { "" };
    CHECK_THROWS_AS(make_git_staged_patch_args(empty_path), std::invalid_argument);
}

TEST_CASE("the recent subjects builder reads from HEAD only", "[git][builder]")
{
    CHECK(to_text(make_git_recent_subjects_args(10)) == text_list { "log", "--max-count=10", "--format=%s", "HEAD", "--" });
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
    CHECK(to_text(make_git_branch_set_upstream_args("feature/x", "origin", "release/1.0")) == text_list { "branch", "--set-upstream-to=origin/release/1.0", "--", "feature/x" });
    CHECK(to_text(make_git_branch_unset_upstream_args("feature/x")) == text_list { "branch", "--unset-upstream", "--", "feature/x" });
    CHECK(to_text(make_git_remote_add_args("origin", "git@github.com:me/repo.git")) == text_list { "remote", "add", "--", "origin", "git@github.com:me/repo.git" });
    CHECK(to_text(make_git_remote_add_args("upstream", native_path("C:\\My Repos\\shared.git"))) == text_list { "remote", "add", "--", "upstream", native_path("C:\\My Repos\\shared.git") });
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
    // unstage는 첫 커밋 전에도 되도록 `restore --staged`가 아니라 `reset`이다.
    CHECK(to_text(make_git_restore_staged_args(paths)) == text_list { "--literal-pathspecs", "reset", "-q", "--", "a.txt", "dir/b c.txt", "-dash.txt" });
    CHECK(to_text(make_git_restore_worktree_args(paths)) == text_list { "--literal-pathspecs", "restore", "--", "a.txt", "dir/b c.txt", "-dash.txt" });
    CHECK(to_text(make_git_add_from_stdin_args()) == text_list { "--literal-pathspecs", "add", "--pathspec-from-file=-", "--pathspec-file-nul" });
    CHECK(to_text(make_git_restore_staged_from_stdin_args()) == text_list { "--literal-pathspecs", "reset", "-q", "--pathspec-from-file=-", "--pathspec-file-nul" });
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
    // 사용자의 `apply.whitespace`가 hunk를 고치거나 거절하지 않게 공백 처리를 늘 고정한다.
    CHECK(to_text(make_git_apply_args()) == text_list { "apply", "--cached", "--whitespace=nowarn", "-" });

    git_apply_options options {};
    options.reverse = true;
    options.check_only = true;
    options.recount = true;
    options.unidiff_zero = true;
    CHECK(to_text(make_git_apply_args(options)) == text_list { "apply", "--cached", "-R", "--check", "--recount", "--unidiff-zero", "--whitespace=nowarn", "-" });

    options.cached = false;
    options.reverse = false;
    options.check_only = false;
    options.recount = false;
    options.unidiff_zero = false;
    CHECK(to_text(make_git_apply_args(options)) == text_list { "apply", "--whitespace=nowarn", "-" });
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
    clone.directory = native_path("C:/work/repo");
    CHECK(to_text(make_git_clone_args(clone)) == text_list { "clone", "--progress", "--", "git@example.com:team/repo.git", native_path("C:/work/repo") });

    clone.branch = "dev";
    clone.depth = 1;
    CHECK(to_text(make_git_clone_args(clone)) == text_list { "clone", "--progress", "--branch", "dev", "--depth", "1", "--", "git@example.com:team/repo.git", native_path("C:/work/repo") });

    clone.url = "--upload-pack=calc";
    CHECK_THROWS_AS(make_git_clone_args(clone), std::invalid_argument);

    CHECK(to_text(make_git_init_args(native_path("C:/work/new"))) == text_list { "init", "--", native_path("C:/work/new") });
    CHECK(to_text(make_git_init_args(native_path("C:/work/new"), "trunk")) == text_list { "init", "--initial-branch=trunk", "--", native_path("C:/work/new") });
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
    CHECK_THROWS_AS(make_git_branch_set_upstream_args("-x", "origin", "main"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_branch_set_upstream_args("x", "--origin", "main"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_branch_set_upstream_args("x", "origin", "--main"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_branch_set_upstream_args("x", "", "main"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_branch_unset_upstream_args("--all"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_remote_add_args("-x", "https://example.com/repo.git"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_remote_add_args("", "https://example.com/repo.git"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_remote_add_args("my remote", "https://example.com/repo.git"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_remote_add_args("origin", ""), std::invalid_argument);
    CHECK_THROWS_AS(make_git_remote_add_args("origin", "--upload-pack=evil"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_remote_add_args("origin", "https://example.com/a\nb"), std::invalid_argument);
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
    CHECK_THROWS_AS(make_git_init_args(native_path("C:/x"), "-b"), std::invalid_argument);

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

TEST_CASE("session builders cover checkout policies, local fast-forward and drag-and-drop queries", "[git][builder]")
{
    CHECK(to_text(make_git_switch_args("main", git_switch_mode::normal)) == text_list { "switch", "--", "main" });
    CHECK(to_text(make_git_switch_args("main", git_switch_mode::discard_changes)) == text_list { "switch", "--discard-changes", "--", "main" });
    CHECK(to_text(make_git_switch_args("main", git_switch_mode::merge)) == text_list { "switch", "--merge", "--", "main" });
    CHECK(to_text(make_git_switch_detach_args("abc123", git_switch_mode::merge)) == text_list { "switch", "--detach", "--merge", "--", "abc123" });
    CHECK(to_text(make_git_switch_create_args("release/1.0", "origin/release/1.0", true, git_switch_mode::discard_changes))
        == text_list { "switch", "-c", "release/1.0", "--track", "--discard-changes", "--", "origin/release/1.0" });

    // branch -f와 달리 git이 non-fast-forward를 거절하는 길이다.
    CHECK(to_text(make_git_fetch_fast_forward_args("refs/heads/main", "refs/heads/feature")) == text_list { "fetch", "--", ".", "refs/heads/main:refs/heads/feature" });
    CHECK(to_text(make_git_is_ancestor_args("main", "feature")) == text_list { "merge-base", "--is-ancestor", "main", "feature" });
    CHECK(to_text(make_git_count_commits_args("main", "feature")) == text_list { "rev-list", "--count", "main..feature", "--" });
    CHECK_THROWS_AS(make_git_count_commits_args("--all", "feature"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_fetch_fast_forward_args("refs/heads/main", "-x"), std::invalid_argument);
}

TEST_CASE("session builders for remotes, conflicts and commit details", "[git][builder]")
{
    CHECK(to_text(make_git_remote_list_args()) == text_list { "remote", "-v" });
    CHECK(to_text(make_git_add_all_args()) == text_list { "add", "--all" });
    CHECK(to_text(make_git_checkout_conflict_side_args("a b.txt", true)) == text_list { "--literal-pathspecs", "checkout", "--ours", "--", "a b.txt" });
    CHECK(to_text(make_git_checkout_conflict_side_args("a.txt", false)) == text_list { "--literal-pathspecs", "checkout", "--theirs", "--", "a.txt" });
    CHECK(to_text(make_git_merge_continue_args()) == text_list { "-c", "core.editor=true", "merge", "--continue" });
    CHECK(to_text(make_git_cherry_pick_skip_args()) == text_list { "cherry-pick", "--skip" });
    CHECK(to_text(make_git_revert_skip_args()) == text_list { "revert", "--skip" });
    CHECK(to_text(make_git_current_branch_args()) == text_list { "symbolic-ref", "--quiet", "--short", "HEAD" });
    CHECK(to_text(make_git_ref_exists_args("refs/heads/main")) == text_list { "show-ref", "--verify", "--quiet", "refs/heads/main" });
    CHECK(to_text(make_git_symbolic_full_name_args("main")) == text_list { "rev-parse", "--symbolic-full-name", "main" });

    const std::string sha { "c1bf80d6a7260e533f88e475f0d4c611194766c2" };
    CHECK(to_text(make_git_commit_info_args(sha)) == text_list { "log", "-1", "-z", "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%ct%x1f%s%x1f%b", sha, "--" });
    // `--root`가 없으면 사용자의 `log.showRoot=false`가 첫 커밋의 파일 목록을 비운다.
    CHECK(to_text(make_git_commit_name_status_args(sha, false)) == text_list { "log", "-1", "-z", "--format=", "--name-status", "--find-renames", "--root", sha, "--" });
    CHECK(to_text(make_git_commit_name_status_args(sha, true)) == text_list { "log", "-1", "-z", "--format=", "--name-status", "--find-renames", "--root", "-m", "--first-parent", sha, "--" });
    CHECK(to_text(make_git_show_file_args(sha, "src/a.cpp", false))
        == text_list { "--literal-pathspecs", "show", "--format=", "--no-color", "--no-ext-diff", "--src-prefix=a/", "--dst-prefix=b/", "-U3", sha, "--", "src/a.cpp" });
}

TEST_CASE("comparing two revisions diffs their trees with the older one on the left", "[git][builder]")
{
    const std::string base { "1111111111111111111111111111111111111111" };
    const std::string tip { "2222222222222222222222222222222222222222" };
    CHECK(to_text(make_git_diff_revisions_name_status_args(base, tip)) == text_list { "diff", "-z", "--name-status", "--find-renames", base, tip, "--" });
    CHECK(to_text(make_git_diff_revisions_numstat_args(base, tip)) == text_list { "diff", "--numstat", "-z", "--find-renames", base, tip, "--" });
    CHECK(to_text(make_git_diff_revisions_file_args(base, tip, "dir/*.txt", 5))
        == text_list { "--literal-pathspecs", "diff", "--no-color", "--no-ext-diff", "--src-prefix=a/", "--dst-prefix=b/", "-U5", base, tip, "--", "dir/*.txt" });
    CHECK(to_text(make_git_count_reachable_args(tip)) == text_list { "rev-list", "--count", tip, "--" });
    // 빈 트리의 해시는 저장소의 객체 형식(sha1·sha256)을 따르므로 상수로 두지 않고 git에게 묻는다.
    CHECK(to_text(make_git_empty_tree_args()) == text_list { "hash-object", "-t", "tree", "--stdin" });
    CHECK_THROWS_AS(make_git_diff_revisions_name_status_args("--output=x", tip), std::invalid_argument);
    CHECK_THROWS_AS(make_git_diff_revisions_file_args(base, "-R", "a.txt"), std::invalid_argument);
}

TEST_CASE("blame reads the working tree unless a revision is given", "[git][builder]")
{
    const std::string sha { "1111111111111111111111111111111111111111" };
    CHECK(to_text(make_git_blame_args("src/a.cpp")) == text_list { "blame", "--porcelain", "--", "src/a.cpp" });
    CHECK(to_text(make_git_blame_args("src/a.cpp", sha)) == text_list { "blame", "--porcelain", sha, "--", "src/a.cpp" });
    // blame의 경로는 pathspec이 아니라 파일 이름 그대로다. glob 글자가 든 이름도 그대로 넘긴다.
    CHECK(to_text(make_git_blame_args("dir/b[12]*.txt", "main~2")) == text_list { "blame", "--porcelain", "main~2", "--", "dir/b[12]*.txt" });
    check_wellformed(make_git_blame_args("a.txt", sha));

    // 리비전 자리의 옵션과 빈 경로는 git에 닿기 전에 거절한다. 경로는 `--` 뒤라 `-`로 시작해도 된다.
    CHECK_THROWS_AS(make_git_blame_args("a.txt", "--contents=x"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_blame_args("a.txt", "-L1,2"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_blame_args(""), std::invalid_argument);
    CHECK_THROWS_AS(make_git_blame_args(std::string { "a\0b", 3 }), std::invalid_argument);
    CHECK(to_text(make_git_blame_args("-L1,2")) == text_list { "blame", "--porcelain", "--", "-L1,2" });
}

TEST_CASE("file history follows one literal path from HEAD or a revision", "[git][builder]")
{
    const std::string sha { "1111111111111111111111111111111111111111" };
    const std::string format { "--format=%x1e%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%ct%x1f%s" };
    // `--literal-pathspecs`가 없으면 `b[12].txt`가 glob으로 읽혀 `b1.txt`의 커밋이 섞이고 이름 바꾸기를 따라가지 못한다.
    // `--root`가 없으면 `log.showRoot=false`인 사용자에게서 파일을 만든 첫 커밋이 빠진다.
    CHECK(to_text(make_git_file_log_args("dir/b[12].txt"))
        == text_list { "--literal-pathspecs", "log", "--follow", "-z", "--name-status", "--root", format, "HEAD", "--", "dir/b[12].txt" });
    CHECK(to_text(make_git_file_log_args("a.txt", sha, 2001))
        == text_list { "--literal-pathspecs", "log", "--follow", "-z", "--name-status", "--root", format, "--max-count=2001", sha, "--", "a.txt" });
    // 레코드의 필드는 그래프 log와 같아야 한다 (같은 해석기로 읽는다). 앞에 레코드 시작 글자만 더 있다.
    const text_list graph_log { to_text(make_git_log_args()) };
    REQUIRE(graph_log.size() > 3);
    CHECK(format == "--format=%x1e" + graph_log[3].substr(std::string { "--format=" }.size()));
    check_wellformed(make_git_file_log_args("a.txt", sha, 10));

    CHECK_THROWS_AS(make_git_file_log_args("a.txt", "--all"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_file_log_args("", sha), std::invalid_argument);
}

TEST_CASE("commit search matches one field literally over the refs the graph reads", "[git][builder][commit_search]")
{
    const std::string format { "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%ct%x1f%s" };
    // `-i --fixed-strings`가 없으면 질의가 대소문자를 가리는 정규식으로 읽힌다. `--no-mailmap`이 없으면 `--author`가 화면에 보이는
    // 이름(`%an`)이 아니라 `.mailmap`으로 바꾼 이름과 견준다.
    CHECK(to_text(make_git_search_log_args("fix parser", commit_search_field::message))
        == text_list { "log", "--topo-order", "-z", format, "--no-mailmap", "-i", "--fixed-strings", "--grep=fix parser", "--branches", "--remotes", "--tags", "HEAD", "--" });
    CHECK(to_text(make_git_search_log_args("morgan@example.com", commit_search_field::author, git_graph_refs { .include_head = false }, 1001))
        == text_list { "log", "--topo-order", "-z", format, "--no-mailmap", "-i", "--fixed-strings", "--author=morgan@example.com", "--max-count=1001", "--branches", "--remotes", "--tags", "--" });
    // 그래프 log와 같은 해석기로 읽는다. 본문은 받지 않는다.
    CHECK(format == to_text(make_git_log_args())[3]);

    // 옵션처럼 보이는 질의는 `--grep=`·`--author=`의 값으로만 들어간다. 다른 인자는 그대로다.
    for (const commit_search_field field : { commit_search_field::message, commit_search_field::author })
    {
        const std::string option { field == commit_search_field::message ? "--grep=" : "--author=" };
        const text_list plain { to_text(make_git_search_log_args("x", field, {}, 5)) };
        for (const std::string query : { "--all", "-n1", "--grep=other", ".*", "a b", "인사", "--" })
        {
            INFO("query " << query);
            text_list expected { plain };
            expected[7] = option + query;
            CHECK(to_text(make_git_search_log_args(query, field, {}, 5)) == expected);
        }
        check_wellformed(make_git_search_log_args("x", field));
        // 빈 질의는 모든 커밋에 맞고, 줄바꿈은 git이 패턴 여럿으로 나눠 읽는다.
        CHECK_THROWS_AS(make_git_search_log_args("", field), std::invalid_argument);
        CHECK_THROWS_AS(make_git_search_log_args("a\nb", field), std::invalid_argument);
        CHECK_THROWS_AS(make_git_search_log_args(std::string { "a\0b", 3 }, field), std::invalid_argument);
    }
}

TEST_CASE("commit search and the graph log read the same set of refs", "[git][builder][commit_search]")
{
    const std::string sha { "1111111111111111111111111111111111111111" };
    for (const bool include_head : { true, false })
    {
        INFO("include_head " << include_head);
        git_log_options options {};
        options.graph.include_head = include_head;
        // `log --topo-order -z <format> <refs> --`에서 ref 집합만 떼어 낸다.
        const text_list graph { to_text(make_git_log_args(options)) };
        REQUIRE(graph.size() > 5);
        const text_list refs { graph.begin() + 4, graph.end() - 1 };
        CHECK(refs == (include_head ? text_list { "--branches", "--remotes", "--tags", "HEAD" } : text_list { "--branches", "--remotes", "--tags" }));

        for (const commit_search_field field : { commit_search_field::message, commit_search_field::author })
        {
            const text_list search { to_text(make_git_search_log_args("x", field, options.graph)) };
            REQUIRE(search.size() > 9);
            CHECK(text_list { search.begin() + 8, search.end() - 1 } == refs);
            CHECK(search.back() == "--");
        }

        // 닿는지 묻는 명령도 같은 집합을 `--not` 뒤에 둔다. 출력이 비면 그래프의 ref에서 닿는 커밋이다.
        text_list reachability { "rev-list", "--max-count=1", sha, "--not" };
        reachability.insert(reachability.end(), refs.begin(), refs.end());
        reachability.push_back("--");
        CHECK(to_text(make_git_graph_reachability_args(sha, options.graph)) == reachability);
    }
    CHECK_THROWS_AS(make_git_graph_reachability_args("--all"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_graph_reachability_args("main"), std::invalid_argument);
}

namespace {
    // 그래프의 ref 집합만 떼어 낸다. 그래프 log, 두 검색, 닿는지 묻는 명령이 모두 같은 집합을 내는지도 함께 본다.
    text_list graph_refs_of(const git_graph_refs& graph)
    {
        const std::string sha { "1111111111111111111111111111111111111111" };
        git_log_options options {};
        options.graph = graph;
        // `log --topo-order -z <format> <refs> --`
        const text_list log { to_text(make_git_log_args(options)) };
        REQUIRE(log.size() >= 5);
        const text_list refs { log.begin() + 4, log.end() - 1 };
        for (const commit_search_field field : { commit_search_field::message, commit_search_field::author })
        {
            // `log --topo-order -z <format> --no-mailmap -i --fixed-strings <--grep|--author> <refs> --`
            const text_list search { to_text(make_git_search_log_args("x", field, graph)) };
            REQUIRE(search.size() >= 9);
            CHECK(text_list { search.begin() + 8, search.end() - 1 } == refs);
        }
        text_list reachability { "rev-list", "--max-count=1", sha, "--not" };
        reachability.insert(reachability.end(), refs.begin(), refs.end());
        reachability.push_back("--");
        CHECK(to_text(make_git_graph_reachability_args(sha, graph)) == reachability);
        return refs;
    }
} // namespace

TEST_CASE("without a graph ref filter the graph reads every branch, remote branch and tag as before", "[git][builder][graph_filter]")
{
    CHECK(graph_refs_of({}) == text_list { "--branches", "--remotes", "--tags", "HEAD" });
    CHECK(graph_refs_of({ .include_head = false }) == text_list { "--branches", "--remotes", "--tags" });
    // 현재 브랜치만 알려 주어도 인자는 그대로다.
    CHECK(graph_refs_of({ .current_branch = "main" }) == text_list { "--branches", "--remotes", "--tags", "HEAD" });
    CHECK(to_text(make_git_log_args()) == text_list { "log", "--topo-order", "-z", "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%ct%x1f%s", "--branches", "--remotes", "--tags", "HEAD", "--" });
}

TEST_CASE("showing only some refs starts the graph from exactly those refs, without HEAD", "[git][builder][graph_filter]")
{
    git_graph_refs graph {};
    graph.current_branch = "main";
    graph.filter.only = { "refs/heads/feature", "refs/tags/v1.0", "refs/remotes/origin/topic" };
    // 전체 이름 그대로다. 같은 이름의 브랜치와 태그가 있어도 헷갈리지 않는다.
    CHECK(graph_refs_of(graph) == text_list { "refs/heads/feature", "refs/tags/v1.0", "refs/remotes/origin/topic" });

    // 그동안 숨긴 ref는 듣지 않는다.
    graph.filter.hidden = { "refs/heads/old", "refs/heads/feature" };
    CHECK(graph_refs_of(graph) == text_list { "refs/heads/feature", "refs/tags/v1.0", "refs/remotes/origin/topic" });
    graph.include_head = false;
    CHECK(graph_refs_of(graph) == text_list { "refs/heads/feature", "refs/tags/v1.0", "refs/remotes/origin/topic" });
}

TEST_CASE("hidden refs are excluded right before the group they belong to", "[git][builder][graph_filter]")
{
    git_graph_refs graph {};
    // 넣은 순서와 무관하게 묶음마다 모은다. `--exclude`는 바로 다음의 `--branches`·`--remotes`·`--tags` 하나에만 걸린다.
    graph.filter.hidden = { "refs/tags/v0.1", "refs/heads/old", "refs/remotes/origin/old", "refs/heads/wip/x", "refs/remotes/up/stream/y" };
    CHECK(graph_refs_of(graph)
        == text_list { "--exclude=old", "--exclude=wip/x", "--branches", "--exclude=origin/old", "--exclude=up/stream/y", "--exclude=*/HEAD", "--remotes", "--exclude=v0.1", "--tags", "HEAD" });
    graph.include_head = false;
    CHECK(graph_refs_of(graph)
        == text_list { "--exclude=old", "--exclude=wip/x", "--branches", "--exclude=origin/old", "--exclude=up/stream/y", "--exclude=*/HEAD", "--remotes", "--exclude=v0.1", "--tags" });

    // 원격 브랜치를 숨기지 않으면 원격의 기호 ref(`origin/HEAD`)는 건드리지 않는다.
    git_graph_refs tag_only {};
    tag_only.filter.hidden = { "refs/tags/v0.1" };
    CHECK(graph_refs_of(tag_only) == text_list { "--branches", "--remotes", "--exclude=v0.1", "--tags", "HEAD" });
    git_graph_refs branch_only {};
    branch_only.filter.hidden = { "refs/heads/old" };
    CHECK(graph_refs_of(branch_only) == text_list { "--exclude=old", "--branches", "--remotes", "--tags", "HEAD" });
    // 숨긴 원격 브랜치가 하나라도 있으면 그 커밋이 `<remote>/HEAD`로 닿지 않게 함께 뺀다.
    git_graph_refs remote_only {};
    remote_only.filter.hidden = { "refs/remotes/origin/main" };
    CHECK(graph_refs_of(remote_only) == text_list { "--branches", "--exclude=origin/main", "--exclude=*/HEAD", "--remotes", "--tags", "HEAD" });
}

TEST_CASE("the current branch is never excluded, whatever the filter says", "[git][builder][graph_filter]")
{
    git_graph_refs graph {};
    graph.current_branch = "main";
    graph.filter.hidden = { "refs/heads/main", "refs/heads/old" };
    CHECK(graph_refs_of(graph) == text_list { "--exclude=old", "--branches", "--remotes", "--tags", "HEAD" });

    // 이름만 같은 원격 브랜치와 태그는 현재 브랜치가 아니다.
    graph.filter.hidden = { "refs/heads/main", "refs/remotes/main", "refs/tags/main" };
    CHECK(graph_refs_of(graph) == text_list { "--branches", "--exclude=main", "--exclude=*/HEAD", "--remotes", "--exclude=main", "--tags", "HEAD" });

    // detached HEAD에는 현재 브랜치가 없다.
    graph.current_branch.clear();
    graph.filter.hidden = { "refs/heads/main" };
    CHECK(graph_refs_of(graph) == text_list { "--exclude=main", "--branches", "--remotes", "--tags", "HEAD" });
}

TEST_CASE("a graph ref filter takes full ref names of branches, remote branches and tags only", "[git][builder][graph_filter]")
{
    // 짧은 이름, 세 묶음 밖의 ref, 옵션처럼 보이는 글, 그리고 `--exclude`의 glob으로 다른 ref까지 걸러 낼 글자다.
    const text_list bad_names { "main", "origin/main", "refs/heads/", "refs/stash", "refs/notes/commits", "--all", "HEAD", "", "refs/heads/a*", "refs/tags/v[1]", "refs/remotes/origin/?", "refs/heads/a\\b", "refs/heads/a\nb" };
    for (const std::string& bad : bad_names)
    {
        INFO("name " << bad);
        git_log_options only {};
        only.graph.filter.only = { bad };
        CHECK_THROWS_AS(make_git_log_args(only), std::invalid_argument);
        git_log_options hidden {};
        hidden.graph.filter.hidden = { bad };
        CHECK_THROWS_AS(make_git_log_args(hidden), std::invalid_argument);
        CHECK_THROWS_AS(make_git_search_log_args("x", commit_search_field::message, hidden.graph), std::invalid_argument);
        CHECK_THROWS_AS(make_git_graph_reachability_args("1111111111111111111111111111111111111111", only.graph), std::invalid_argument);
    }
}

TEST_CASE("a commit hash prefix is 4 to 64 hexadecimal digits", "[git][builder][commit_search]")
{
    CHECK(is_commit_hash_prefix("abcd"));
    CHECK(is_commit_hash_prefix("ABCDEF01"));
    CHECK(is_commit_hash_prefix(std::string(40, '1')));
    CHECK(is_commit_hash_prefix(std::string(64, 'f')));
    // git은 4자보다 짧은 앞부분을 풀지 않는다.
    CHECK_FALSE(is_commit_hash_prefix("abc"));
    CHECK_FALSE(is_commit_hash_prefix(""));
    CHECK_FALSE(is_commit_hash_prefix(std::string(65, 'f')));
    CHECK_FALSE(is_commit_hash_prefix("abcg"));
    CHECK_FALSE(is_commit_hash_prefix("ab cd"));
    CHECK_FALSE(is_commit_hash_prefix("--all"));
    CHECK_FALSE(is_commit_hash_prefix("인사말"));
}

TEST_CASE("fetch can keep stale tracking branches and pull can merge", "[git][builder]")
{
    git_fetch_options fetch {};
    fetch.prune = false;
    fetch.remote = "origin";
    CHECK(to_text(make_git_fetch_args(fetch)) == text_list { "fetch", "--progress", "--", "origin" });
    CHECK(to_text(make_git_pull_args(git_pull_mode::merge)) == text_list { "pull", "--no-rebase" });
}
TEST_CASE("textconv detection builders", "[git][builder]")
{
    CHECK(to_text(make_git_check_attr_diff_args("dir/a.docx")) == text_list { "check-attr", "-z", "diff", "--", "dir/a.docx" });
    CHECK(to_text(make_git_config_get_args("diff.word.textconv")) == text_list { "config", "--get", "diff.word.textconv" });
    CHECK_THROWS_AS(make_git_check_attr_diff_args(""), std::invalid_argument);
    CHECK_THROWS_AS(make_git_config_get_args(""), std::invalid_argument);
    CHECK_THROWS_AS(make_git_config_get_args("--global"), std::invalid_argument);
    CHECK_THROWS_AS(make_git_config_get_args("diff.a\nb.textconv"), std::invalid_argument);
}

TEST_CASE("a fetch refspec fetches only that ref and never prunes", "[git][builder]")
{
    git_fetch_options fetch {};
    fetch.remote = "origin";
    fetch.refspec = "+pull/7/head:pr/7";
    fetch.all = true;
    CHECK(to_text(make_git_fetch_args(fetch)) == text_list { "fetch", "--progress", "--", "origin", "+pull/7/head:pr/7" });
    fetch.remote.clear();
    CHECK_THROWS_AS(make_git_fetch_args(fetch), std::invalid_argument);
    fetch.remote = "origin";
    for (const char* bad : { "--upload-pack=evil", "a b:c", "x\n:y", "a\b:c" })
    {
        fetch.refspec = bad;
        CHECK_THROWS_AS(make_git_fetch_args(fetch), std::invalid_argument);
    }
}
