// 실제 git으로 임시 저장소를 만들고, 조립기가 만든 명령을 실행해 해석기에 되먹이는
// 통합 test다. git이 설치되어 있지 않으면 건너뛴다. 모두 `[integration]` 태그를 가진다.
#include "infrastructure/git_command_builder.h"
#include "infrastructure/git_diff_parser.h"
#include "infrastructure/git_error_classifier.h"
#include "infrastructure/git_log_parser.h"
#include "infrastructure/git_ref_parser.h"
#include "infrastructure/git_stash_parser.h"
#include "infrastructure/git_status_parser.h"

#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using namespace cyella;
    using cyella::test::git_run_result;
    using cyella::test::run_git;
    using strings = std::vector<std::string>;

    std::string trimmed(std::string text)
    {
        while (text.empty() == false && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        return text;
    }

    // 실행이 성공해야 하는 명령이다. 실패하면 stderr를 함께 보여 준다.
    git_run_result run_ok(const std::filesystem::path& repo, const git_arguments& arguments, const std::string_view standard_input = {})
    {
        git_run_result result { run_git(repo, arguments, standard_input) };
        INFO("git " << [&arguments] {
            std::string joined {};
            for (const std::string& argument : cyella::test::to_text(arguments))
                joined.append(argument).push_back(' ');
            return joined;
        }());
        INFO("stderr: " << result.standard_error);
        INFO("stdout: " << result.standard_output);
        REQUIRE(result.exit_code == 0);
        return result;
    }

    git_run_result run_ok(const std::filesystem::path& repo, const std::vector<std::string>& arguments, const std::string_view standard_input = {})
    {
        std::vector<std::u8string> converted {};
        for (const std::string& argument : arguments)
            converted.emplace_back(argument.begin(), argument.end());
        return run_ok(repo, converted, standard_input);
    }

    // 실패해야 하는 명령이다. 실행 결과를 돌려준다.
    git_run_result run_failing(const std::filesystem::path& repo, const git_arguments& arguments, const std::string_view standard_input = {})
    {
        git_run_result result { run_git(repo, arguments, standard_input) };
        INFO("stderr: " << result.standard_error);
        INFO("stdout: " << result.standard_output);
        REQUIRE(result.exit_code != 0);
        return result;
    }

    void write(const std::filesystem::path& repo, const std::string_view relative, const std::string_view content)
    {
        cyella::test::write_file(repo / std::filesystem::path { std::string { relative } }, content);
    }

    void stage_and_commit(const std::filesystem::path& repo, const std::vector<std::string>& paths, const std::string_view message)
    {
        run_ok(repo, make_git_add_args(paths));
        run_ok(repo, make_git_commit_args(), std::string { message } + "\n");
    }

    void commit_file(const std::filesystem::path& repo, const std::string& path, const std::string_view content, const std::string_view message)
    {
        write(repo, path, content);
        stage_and_commit(repo, { path }, message);
    }

    std::string head_sha(const std::filesystem::path& repo)
    {
        return trimmed(run_ok(repo, std::vector<std::string> { "rev-parse", "HEAD" }).standard_output);
    }

    git_status_result status_of(const std::filesystem::path& repo)
    {
        return parse_git_status(run_ok(repo, make_git_status_args()).standard_output);
    }

    git_ref_list refs_of(const std::filesystem::path& repo)
    {
        return parse_git_refs(run_ok(repo, make_git_for_each_ref_args()).standard_output);
    }

    std::vector<commit_info> log_of(const std::filesystem::path& repo, const git_log_options& options = {})
    {
        return parse_git_log(run_ok(repo, make_git_log_args(options)).standard_output, options.include_body);
    }

    repository_operation_state operation_state_of(const std::filesystem::path& repo)
    {
        const std::string output { run_ok(repo, make_git_rev_parse_args()).standard_output };
        const std::string git_dir { trimmed(output.substr(0, output.find('\n'))) };
        std::vector<std::string> existing {};
        for (const std::string& marker : make_in_progress_marker_paths(git_dir))
            if (std::filesystem::exists(std::filesystem::path { marker }))
                existing.push_back(marker);
        return detect_repository_operation_state(existing);
    }

    const ref* find_ref(const git_ref_list& list, const std::string_view full_name)
    {
        for (const ref& entry : list.refs)
            if (entry.full_name == full_name)
                return &entry;
        return nullptr;
    }

    // 첫 커밋까지 만든 저장소 폴더를 돌려준다.
    std::filesystem::path make_repository(const cyella::test::temp_directory& directory, const std::string& name)
    {
        const std::filesystem::path repo { directory.path() / name };
        run_ok(directory.path(), make_git_init_args(repo.string(), "main"));
        return repo;
    }
} // namespace

TEST_CASE("integration: real git output flows through every read parser", "[git][integration]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    const cyella::test::temp_directory directory {};
    const std::filesystem::path repo { make_repository(directory, "read") };

    // 첫 커밋: 이름을 바꾸고 지울 파일을 포함한다.
    write(repo, "a.txt", "hello\nworld\n");
    write(repo, "dir/b.txt", "one\ntwo\nthree\nfour\nfive\n");
    write(repo, "old_name.txt", "rename me\nline 2\nline 3\nline 4\nline 5\n");
    write(repo, "gone.txt", "bye\n");
    run_ok(repo, make_git_add_args(strings { "a.txt", "dir/b.txt", "old_name.txt", "gone.txt" }));
    run_ok(repo, make_git_commit_args(), "initial commit\n\nbody of the first commit\n");
    const std::string root_sha { head_sha(repo) };

    // 브랜치를 만들어 병합한다.
    run_ok(repo, make_git_switch_create_args("feature"));
    commit_file(repo, "f.txt", "feat\n", "feat: add f");
    run_ok(repo, make_git_switch_args("main"));
    commit_file(repo, "a.txt", "hello\nmain\n", "main change");
    run_ok(repo, make_git_merge_args("feature", git_merge_mode::no_fast_forward));
    const std::string merge_sha { head_sha(repo) };

    // 태그 둘: lightweight와 annotated
    run_ok(repo, make_git_tag_args("v1.0"));
    run_ok(repo, make_git_annotated_tag_args("v2.0"), "release 2\n");

    // stash 하나
    write(repo, "a.txt", "hello\nstash me\n");
    git_stash_push_options stash_options {};
    stash_options.message = "wip \xED\x95\x9C";
    run_ok(repo, make_git_stash_push_args(stash_options));

    // 작업 트리 변경: 이름 변경(staged), 삭제(staged), 수정(unstaged), untracked
    run_ok(repo, std::vector<std::string> { "mv", "old_name.txt", "new name.txt" });
    run_ok(repo, std::vector<std::string> { "rm", "-q", "gone.txt" });
    write(repo, "dir/b.txt", "one\ntwo\nTHREE\nfour\nfive\n");
    write(repo, "untracked.txt", "u\n");

    SECTION("status")
    {
        const git_status_result status { status_of(repo) };
        CHECK(status.branch.head == "main");
        CHECK(status.branch.oid == merge_sha);
        CHECK(status.skipped_entry_count == 0);

        REQUIRE(status.staged.size() == 2);
        CHECK(status.staged[0].path == "gone.txt");
        CHECK(status.staged[0].status == 'D');
        CHECK(status.staged[1].path == "new name.txt");
        CHECK(status.staged[1].status == 'R');
        CHECK(status.staged[1].original_path == "old_name.txt");

        REQUIRE(status.unstaged.size() == 2);
        CHECK(status.unstaged[0].path == "dir/b.txt");
        CHECK(status.unstaged[0].status == 'M');
        CHECK(status.unstaged[1].path == "untracked.txt");
        CHECK(status.unstaged[1].status == '?');

        repository_summary summary {};
        apply_git_status(summary, status);
        CHECK(summary.changed_file_count == 4);
        CHECK(operation_state_of(repo) == repository_operation_state::none);
    }

    SECTION("numstat joins the status entries")
    {
        git_status_result status { status_of(repo) };
        apply_numstat(status.staged, parse_git_numstat(run_ok(repo, make_git_diff_numstat_args(diff_source::staged)).standard_output));
        apply_numstat(status.unstaged, parse_git_numstat(run_ok(repo, make_git_diff_numstat_args(diff_source::unstaged)).standard_output));

        CHECK(status.staged[0].lines_removed == 1);
        CHECK(status.staged[1].lines_added == 0);
        CHECK(status.unstaged[0].lines_added == 1);
        CHECK(status.unstaged[0].lines_removed == 1);
    }

    SECTION("refs")
    {
        git_ref_list list { refs_of(repo) };
        const ref* main_branch { find_ref(list, "refs/heads/main") };
        REQUIRE(main_branch != nullptr);
        CHECK(main_branch->is_current);
        CHECK(main_branch->target_sha == merge_sha);
        CHECK(main_branch->upstream.empty());
        REQUIRE(find_ref(list, "refs/heads/feature") != nullptr);

        const ref* light { find_ref(list, "refs/tags/v1.0") };
        const ref* annotated { find_ref(list, "refs/tags/v2.0") };
        REQUIRE(light != nullptr);
        REQUIRE(annotated != nullptr);
        CHECK_FALSE(light->is_annotated_tag);
        CHECK(annotated->is_annotated_tag);
        // 두 태그 모두 병합 커밋을 가리킨다.
        CHECK(light->target_sha == merge_sha);
        CHECK(annotated->target_sha == merge_sha);
        CHECK(list.remote_defaults.empty());
    }

    SECTION("log")
    {
        const std::vector<commit_info> commits { log_of(repo) };
        REQUIRE(commits.size() == 4);
        CHECK(commits[0].sha == merge_sha);
        CHECK(commits[0].parents.size() == 2);
        CHECK(commits.back().sha == root_sha);
        CHECK(commits.back().parents.empty());
        CHECK(commits.back().author_name == "Ann Author");
        CHECK(commits.back().committer_time == commits.back().author_time);

        git_log_options with_body {};
        with_body.include_body = true;
        const std::vector<commit_info> detailed { log_of(repo, with_body) };
        REQUIRE(detailed.size() == 4);
        CHECK(detailed.back().subject == "initial commit");
        CHECK(detailed.back().body == "body of the first commit\n");

        git_log_options paged {};
        paged.max_count = 2;
        paged.skip = 1;
        const std::vector<commit_info> page { log_of(repo, paged) };
        REQUIRE(page.size() == 2);
        CHECK(page[0].sha == commits[1].sha);
        CHECK(page[1].sha == commits[2].sha);
    }

    SECTION("stash")
    {
        const git_stash_list list { parse_git_stash_list(run_ok(repo, make_git_stash_list_args()).standard_output) };
        REQUIRE(list.entries.size() == 1);
        CHECK(list.entries[0].name == "stash@{0}");
        CHECK(list.entries[0].branch == "main");
        CHECK(list.entries[0].message == "wip \xED\x95\x9C");
        CHECK(list.entries[0].base_sha == merge_sha);
    }

    SECTION("diffs")
    {
        const std::vector<diff_file> unstaged { parse_git_diff(run_ok(repo, make_git_diff_args("dir/b.txt", diff_source::unstaged)).standard_output) };
        REQUIRE(unstaged.size() == 1);
        CHECK(unstaged[0].new_path == "dir/b.txt");
        REQUIRE(unstaged[0].hunks.size() == 1);
        CHECK(unstaged[0].lines_added == 1);

        const std::vector<diff_file> staged { parse_git_diff(run_ok(repo, make_git_diff_args("gone.txt", diff_source::staged)).standard_output) };
        REQUIRE(staged.size() == 1);
        CHECK(staged[0].status == 'D');

        // 추적하지 않는 파일은 차이가 있어 종료 코드가 1이다.
        const git_run_result untracked { run_git(repo, make_git_diff_untracked_args("untracked.txt")) };
        CHECK(untracked.exit_code == 1);
        const std::vector<diff_file> added { parse_git_diff(untracked.standard_output) };
        REQUIRE(added.size() == 1);
        CHECK(added[0].status == 'A');
        CHECK(added[0].new_path == "untracked.txt");

        // 병합 커밋은 첫 부모와의 diff만 받는다.
        const std::vector<diff_file> merge_diff { parse_git_diff(run_ok(repo, make_git_show_commit_args(merge_sha, true)).standard_output) };
        REQUIRE(merge_diff.size() == 1);
        CHECK(merge_diff[0].new_path == "f.txt");
        CHECK(merge_diff[0].status == 'A');

        const std::vector<numstat_entry> merge_numstat { parse_git_numstat(run_ok(repo, make_git_show_numstat_args(merge_sha, true)).standard_output) };
        REQUIRE(merge_numstat.size() == 1);
        CHECK(merge_numstat[0].path == "f.txt");

        // 첫 커밋은 전체 내용이 추가로 보인다.
        const std::vector<diff_file> root_diff { parse_git_diff(run_ok(repo, make_git_show_commit_args(root_sha, false)).standard_output) };
        REQUIRE(root_diff.size() == 4);
        for (const diff_file& file : root_diff)
            CHECK(file.status == 'A');
    }
}

TEST_CASE("integration: write commands run and leave the repository in the expected state", "[git][integration]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    const cyella::test::temp_directory directory {};
    const std::filesystem::path repo { make_repository(directory, "write") };

    commit_file(repo, "a.txt", "one\n", "first");
    commit_file(repo, "b.txt", "b\n", "second");

    SECTION("amend and reset")
    {
        git_commit_options keep {};
        keep.amend = true;
        keep.keep_message = true;
        const std::string before { head_sha(repo) };
        write(repo, "b.txt", "b changed\n");
        run_ok(repo, make_git_add_args(strings { "b.txt" }));
        run_ok(repo, make_git_commit_args(keep));
        CHECK(head_sha(repo) != before);
        CHECK(log_of(repo).front().subject == "second");

        git_commit_options reword {};
        reword.amend = true;
        run_ok(repo, make_git_commit_args(reword), "reworded \xED\x95\x9C\n\nwith a body\n");
        git_log_options with_body {};
        with_body.include_body = true;
        const std::vector<commit_info> commits { log_of(repo, with_body) };
        CHECK(commits.front().subject == "reworded \xED\x95\x9C");
        CHECK(commits.front().body == "with a body\n");
        CHECK(commits.size() == 2);

        run_ok(repo, make_git_reset_args(git_reset_mode::soft, "HEAD~1"));
        const git_status_result status { status_of(repo) };
        REQUIRE(status.staged.size() == 1);
        CHECK(status.staged[0].path == "b.txt");
        CHECK(log_of(repo).size() == 1);

        run_ok(repo, make_git_reset_args(git_reset_mode::hard, "HEAD"));
        CHECK(status_of(repo).staged.empty());
    }

    SECTION("branches")
    {
        run_ok(repo, make_git_branch_create_args("tmp", "HEAD~1"));
        run_ok(repo, make_git_branch_rename_args("tmp", "tmp2"));
        const git_ref_list list { refs_of(repo) };
        REQUIRE(find_ref(list, "refs/heads/tmp2") != nullptr);
        CHECK(find_ref(list, "refs/heads/tmp") == nullptr);
        CHECK(find_ref(list, "refs/heads/tmp2")->target_sha != head_sha(repo));

        run_ok(repo, make_git_switch_args("tmp2"));
        CHECK(status_of(repo).branch.head == "tmp2");
        run_ok(repo, make_git_switch_args("main"));
        run_ok(repo, make_git_branch_delete_args("tmp2", false));
        CHECK(find_ref(refs_of(repo), "refs/heads/tmp2") == nullptr);

        run_ok(repo, make_git_switch_detach_args(head_sha(repo)));
        const git_status_result detached { status_of(repo) };
        CHECK(detached.branch.is_detached);
        run_ok(repo, make_git_switch_args("main"));
    }

    SECTION("stage unstage and discard")
    {
        write(repo, "a.txt", "changed\n");
        write(repo, "glob[1].txt", "literal\n");
        CHECK(status_of(repo).unstaged.size() == 2);

        // 글롭 문자가 든 이름도 그 파일 하나만 뜻한다.
        run_ok(repo, make_git_add_args(strings { "a.txt", "glob[1].txt" }));
        git_status_result status { status_of(repo) };
        CHECK(status.staged.size() == 2);
        CHECK(status.unstaged.empty());

        run_ok(repo, make_git_restore_staged_args(strings { "a.txt", "glob[1].txt" }));
        status = status_of(repo);
        CHECK(status.staged.empty());
        REQUIRE(status.unstaged.size() == 2);

        run_ok(repo, make_git_restore_worktree_args(strings { "a.txt" }));
        status = status_of(repo);
        REQUIRE(status.unstaged.size() == 1);
        CHECK(status.unstaged[0].path == "glob[1].txt");

        // 경로 목록을 stdin으로 넘기는 판
        write(repo, "a.txt", "changed again\n");
        run_ok(repo, make_git_add_from_stdin_args(), std::string { "a.txt" } + '\0' + "glob[1].txt" + '\0');
        CHECK(status_of(repo).staged.size() == 2);
        run_ok(repo, make_git_restore_staged_from_stdin_args(), std::string { "a.txt" } + '\0');
        status = status_of(repo);
        CHECK(status.staged.size() == 1);
        run_ok(repo, make_git_restore_worktree_from_stdin_args(), std::string { "a.txt" } + '\0');
        CHECK(status_of(repo).unstaged.empty());
    }

    SECTION("apply stages and unstages a patch from stdin")
    {
        write(repo, "a.txt", "one\ntwo\n");
        const std::string patch { run_ok(repo, make_git_diff_args("a.txt", diff_source::unstaged)).standard_output };
        REQUIRE_FALSE(patch.empty());

        git_apply_options check {};
        check.check_only = true;
        run_ok(repo, make_git_apply_args(check), patch);
        CHECK(status_of(repo).staged.empty());

        run_ok(repo, make_git_apply_args(), patch);
        CHECK(status_of(repo).staged.size() == 1);

        git_apply_options reverse {};
        reverse.reverse = true;
        run_ok(repo, make_git_apply_args(reverse), patch);
        CHECK(status_of(repo).staged.empty());
        CHECK(status_of(repo).unstaged.size() == 1);

        // 어긋난 patch는 실패하고 stderr가 남는다.
        const git_run_result failed { run_failing(repo, make_git_apply_args(check), "garbage that is not a patch\n") };
        CHECK_FALSE(failed.standard_error.empty());
    }

    SECTION("stash push pop apply drop")
    {
        write(repo, "a.txt", "stash me\n");
        write(repo, "extra.txt", "extra\n");
        git_stash_push_options options {};
        options.message = "first";
        options.include_untracked = true;
        run_ok(repo, make_git_stash_push_args(options));
        CHECK(status_of(repo).unstaged.empty());

        git_stash_list list { parse_git_stash_list(run_ok(repo, make_git_stash_list_args()).standard_output) };
        REQUIRE(list.entries.size() == 1);
        CHECK(list.entries[0].has_untracked);

        run_ok(repo, make_git_stash_apply_args(0));
        CHECK(status_of(repo).unstaged.size() == 2);
        run_ok(repo, make_git_restore_worktree_args(strings { "a.txt" }));
        std::filesystem::remove(repo / "extra.txt");
        CHECK(status_of(repo).unstaged.empty());

        run_ok(repo, make_git_stash_pop_args(0));
        CHECK(status_of(repo).unstaged.size() == 2);
        list = parse_git_stash_list(run_ok(repo, make_git_stash_list_args()).standard_output);
        CHECK(list.entries.empty());

        write(repo, "a.txt", "second stash\n");
        run_ok(repo, make_git_stash_push_args());
        run_ok(repo, make_git_stash_drop_args(0));
        list = parse_git_stash_list(run_ok(repo, make_git_stash_list_args()).standard_output);
        CHECK(list.entries.empty());
    }

    SECTION("cherry-pick and revert")
    {
        run_ok(repo, make_git_switch_create_args("side"));
        commit_file(repo, "side.txt", "side\n", "side commit");
        const std::string side_sha { head_sha(repo) };
        run_ok(repo, make_git_switch_args("main"));

        run_ok(repo, make_git_cherry_pick_args(strings { side_sha }));
        CHECK(log_of(repo).front().subject == "side commit");

        run_ok(repo, make_git_revert_args(strings { "HEAD" }));
        CHECK(log_of(repo).front().subject == "Revert \"side commit\"");
    }

    SECTION("a conflicting merge is classified and can be aborted")
    {
        run_ok(repo, make_git_switch_create_args("topic"));
        commit_file(repo, "a.txt", "topic side\n", "topic change");
        run_ok(repo, make_git_switch_args("main"));
        commit_file(repo, "a.txt", "main side\n", "main change");

        const git_run_result merge { run_failing(repo, make_git_merge_args("topic")) };
        CHECK(classify_git_failure(merge.standard_error, merge.standard_output) == git_failure_kind::conflict);
        CHECK(operation_state_of(repo) == repository_operation_state::merge);

        const git_status_result status { status_of(repo) };
        REQUIRE(status.conflicts.size() == 1);
        CHECK(status.conflicts[0].path == "a.txt");
        CHECK(status.conflicts[0].conflict_code == "UU");

        run_ok(repo, make_git_merge_abort_args());
        CHECK(operation_state_of(repo) == repository_operation_state::none);
        CHECK(status_of(repo).conflicts.empty());
    }

    SECTION("a conflicting rebase is classified and can be aborted or continued")
    {
        run_ok(repo, make_git_switch_create_args("topic"));
        commit_file(repo, "a.txt", "topic side\n", "topic change");
        run_ok(repo, make_git_switch_args("main"));
        commit_file(repo, "a.txt", "main side\n", "main change");

        // main을 topic 위로 옮긴다.
        const git_run_result rebase { run_failing(repo, make_git_rebase_args("topic")) };
        CHECK(classify_git_failure(rebase.standard_error, rebase.standard_output) == git_failure_kind::conflict);
        CHECK(operation_state_of(repo) == repository_operation_state::rebase);
        CHECK(status_of(repo).conflicts.size() == 1);

        run_ok(repo, make_git_rebase_abort_args());
        CHECK(operation_state_of(repo) == repository_operation_state::none);
        CHECK(status_of(repo).branch.head == "main");

        // 다시 충돌시킨 뒤 직접 풀고 계속한다.
        run_failing(repo, make_git_rebase_args("topic"));
        write(repo, "a.txt", "resolved\n");
        run_ok(repo, make_git_add_args(strings { "a.txt" }));
        run_ok(repo, make_git_rebase_continue_args());
        CHECK(operation_state_of(repo) == repository_operation_state::none);
        CHECK(log_of(repo).front().subject == "main change");
    }

    SECTION("a conflicting cherry-pick sets its marker")
    {
        run_ok(repo, make_git_switch_create_args("topic"));
        commit_file(repo, "a.txt", "topic side\n", "topic change");
        const std::string topic_sha { head_sha(repo) };
        run_ok(repo, make_git_switch_args("main"));
        commit_file(repo, "a.txt", "main side\n", "main change");

        run_failing(repo, make_git_cherry_pick_args(strings { topic_sha }));
        CHECK(operation_state_of(repo) == repository_operation_state::cherry_pick);
        run_ok(repo, make_git_cherry_pick_abort_args());
        CHECK(operation_state_of(repo) == repository_operation_state::none);
    }

    SECTION("failures carry stable classifiable text")
    {
        // 작업 트리 변경을 덮어쓸 체크아웃
        run_ok(repo, make_git_switch_create_args("other"));
        commit_file(repo, "a.txt", "other\n", "other change");
        run_ok(repo, make_git_switch_args("main"));
        write(repo, "a.txt", "dirty\n");
        const git_run_result blocked { run_failing(repo, make_git_switch_args("other")) };
        CHECK(classify_git_failure(blocked.standard_error, blocked.standard_output) == git_failure_kind::local_changes_would_be_overwritten);
        run_ok(repo, make_git_restore_worktree_args(strings { "a.txt" }));

        // index.lock
        cyella::test::write_file(repo / ".git" / "index.lock", "");
        const git_run_result locked { run_failing(repo, make_git_add_args(strings { "a.txt" })) };
        CHECK(classify_git_failure(locked.standard_error, locked.standard_output) == git_failure_kind::index_lock_exists);
        std::filesystem::remove(repo / ".git" / "index.lock");

        // 없는 저장소
        const git_run_result missing { run_failing(repo, make_git_ls_remote_tags_args((directory.path() / "nowhere").string())) };
        CHECK(classify_git_failure(missing.standard_error, missing.standard_output) == git_failure_kind::repository_not_found);
    }
}

TEST_CASE("integration: remote commands run against a local bare repository", "[git][integration]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    const cyella::test::temp_directory directory {};
    const std::filesystem::path bare { directory.path() / "origin.git" };
    run_ok(directory.path(), std::vector<std::string> { "init", "--bare", "-b", "main", bare.string() });

    // clone한 빈 저장소에서 첫 커밋을 만들어 push한다.
    git_clone_options clone_options {};
    clone_options.url = bare.string();
    clone_options.directory = (directory.path() / "one").string();
    run_ok(directory.path(), make_git_clone_args(clone_options));
    const std::filesystem::path one { directory.path() / "one" };
    commit_file(one, "a.txt", "one\n", "first");

    git_push_options first_push {};
    first_push.remote = "origin";
    first_push.refspec = "main";
    first_push.set_upstream = true;
    run_ok(one, make_git_push_args(first_push));

    git_ref_list list { refs_of(one) };
    const ref* main_branch { find_ref(list, "refs/heads/main") };
    REQUIRE(main_branch != nullptr);
    CHECK(main_branch->upstream == "origin/main");
    CHECK(main_branch->ahead == 0);
    CHECK(main_branch->behind == 0);
    CHECK_FALSE(main_branch->upstream_gone);
    CHECK(status_of(one).branch.upstream == "origin/main");

    // 두 번째 clone이 진행시킨다.
    clone_options.directory = (directory.path() / "two").string();
    run_ok(directory.path(), make_git_clone_args(clone_options));
    const std::filesystem::path two { directory.path() / "two" };
    commit_file(two, "b.txt", "two\n", "second");
    run_ok(two, make_git_push_args());

    SECTION("fetch shows behind and pull fast-forwards")
    {
        run_ok(one, make_git_fetch_args());
        list = refs_of(one);
        CHECK(find_ref(list, "refs/heads/main")->behind == 1);
        CHECK(find_ref(list, "refs/heads/main")->ahead == 0);

        run_ok(one, make_git_pull_args(git_pull_mode::fast_forward_only));
        CHECK(log_of(one).size() == 2);
    }

    SECTION("a diverged push is rejected as fetch first and force-with-lease replaces it")
    {
        commit_file(one, "c.txt", "local\n", "local commit");
        // 원격 커밋을 아직 가져오지 않았으므로 git은 `(fetch first)`로 거절한다.
        const git_run_result rejected { run_failing(one, make_git_push_args()) };
        CHECK(classify_git_failure(rejected.standard_error, rejected.standard_output) == git_failure_kind::push_rejected_fetch_first);

        // 가져와서 rebase로 통합한 뒤 push한다.
        run_ok(one, make_git_fetch_args());
        list = refs_of(one);
        CHECK(find_ref(list, "refs/heads/main")->ahead == 1);
        CHECK(find_ref(list, "refs/heads/main")->behind == 1);
        run_ok(one, make_git_pull_args(git_pull_mode::rebase));
        run_ok(one, make_git_push_args());
        CHECK(log_of(one).size() == 3);

        git_push_options force {};
        force.force_with_lease = true;
        run_ok(one, make_git_push_args(force));
    }

    SECTION("tags are pushed, discovered on the remote and deleted")
    {
        run_ok(one, make_git_tag_args("v1.0"));
        run_ok(one, make_git_annotated_tag_args("v2.0"), "release 2\n");
        list = refs_of(one);

        auto apply_remote_state = [&] {
            const std::string output { run_ok(one, make_git_ls_remote_tags_args("origin")).standard_output };
            apply_remote_tags(list.refs, "origin", parse_ls_remote_tags(output));
        };
        apply_remote_state();
        CHECK(find_ref(list, "refs/tags/v1.0")->remotes.empty());
        CHECK(find_ref(list, "refs/tags/v2.0")->remotes.empty());

        run_ok(one, make_git_push_tag_args("origin", "v1.0"));
        apply_remote_state();
        CHECK(find_ref(list, "refs/tags/v1.0")->remotes == strings { "origin" });
        // annotated 태그는 태그 객체가 아니라 커밋으로 비교하므로 여전히 올바르게 구분된다.
        CHECK(find_ref(list, "refs/tags/v2.0")->remotes.empty());

        run_ok(one, make_git_push_all_tags_args("origin"));
        apply_remote_state();
        CHECK(find_ref(list, "refs/tags/v2.0")->remotes == strings { "origin" });

        run_ok(one, make_git_delete_remote_tag_args("origin", "v1.0"));
        apply_remote_state();
        CHECK(find_ref(list, "refs/tags/v1.0")->remotes.empty());
        run_ok(one, make_git_delete_tag_args("v1.0"));
        CHECK(find_ref(refs_of(one), "refs/tags/v1.0") == nullptr);
    }

    SECTION("branch push, remote branch tracking checkout and remote branch delete")
    {
        run_ok(one, make_git_switch_create_args("feature"));
        commit_file(one, "f.txt", "f\n", "feature commit");
        git_push_options push_feature {};
        push_feature.remote = "origin";
        push_feature.refspec = "feature";
        push_feature.set_upstream = true;
        run_ok(one, make_git_push_args(push_feature));

        run_ok(two, make_git_fetch_args());
        run_ok(two, make_git_switch_create_args("feature", "origin/feature", true));
        const git_status_result status { status_of(two) };
        CHECK(status.branch.head == "feature");
        CHECK(status.branch.upstream == "origin/feature");

        run_ok(one, make_git_switch_args("main"));
        run_ok(one, make_git_delete_remote_branch_args("origin", "feature"));
        run_ok(two, make_git_fetch_args());
        // --prune으로 추적 ref가 사라져 upstream이 gone이 된다.
        const ref* feature { find_ref(refs_of(two), "refs/heads/feature") };
        REQUIRE(feature != nullptr);
        CHECK(feature->upstream_gone);
    }

    SECTION("a missing remote is classified")
    {
        git_fetch_options fetch {};
        fetch.remote = (directory.path() / "does-not-exist").string();
        const git_run_result result { run_failing(one, make_git_fetch_args(fetch)) };
        CHECK(classify_git_failure(result.standard_error, result.standard_output) == git_failure_kind::repository_not_found);
    }
}
