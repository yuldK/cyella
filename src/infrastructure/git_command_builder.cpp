#include "infrastructure/git_command_builder.h"

#include "domain/ref_name.h"
#include "domain/repository_summary.h"

#include <initializer_list>
#include <stdexcept>
#include <utility>

namespace cyella {
    namespace {
        // 아래 형식은 해석기와 한 벌이다. 필드를 바꾸면 해석기와 고정 출력도 함께 바꾼다.
        // 구분자 0x1F는 ref 이름·이메일·제목에 나오지 않는다.
        constexpr std::string_view for_each_ref_format { "--format=%(refname)%1f%(objectname)%1f%(*objectname)%1f%(HEAD)%1f%(upstream)%1f%(upstream:short)%1f%(upstream:track)%1f%(symref)" };
        constexpr std::string_view log_format { "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%ct%x1f%s" };
        constexpr std::string_view log_format_with_body { "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%ct%x1f%s%x1f%b" };
        constexpr std::string_view literal_pathspecs { "--literal-pathspecs" };
        constexpr std::string_view stash_format { "--format=%gd%x1f%H%x1f%P%x1f%ct%x1f%gs" };

        void push_text(git_arguments& arguments, const std::string_view value)
        {
            arguments.emplace_back(value.begin(), value.end());
        }

        void push_all(git_arguments& arguments, const std::initializer_list<std::string_view> values)
        {
            for (const std::string_view value : values)
                push_text(arguments, value);
        }

        [[noreturn]] void reject(const std::string_view what, const std::string_view reason)
        {
            std::string message { what };
            message.append(": ").append(reason);
            throw std::invalid_argument { message };
        }

        std::string_view require_revision(const std::string_view value, const std::string_view what)
        {
            if (const auto problem { find_revision_problem(value) })
                reject(what, ref_name_problem_text(*problem));
            return value;
        }

        std::string_view require_new_ref_name(const std::string_view value, const std::string_view what)
        {
            if (const auto problem { find_ref_name_problem(value) })
                reject(what, ref_name_problem_text(*problem));
            return value;
        }

        std::string_view require_path(const std::string_view value)
        {
            if (value.empty())
                reject("path", "path is empty");
            if (value.find('\0') != std::string_view::npos)
                reject("path", "path contains NUL");
            return value;
        }

        // 경로 목록을 `-- <paths>`로 붙인다. 비어 있으면 모든 변경을 건드릴 수 있어 거절한다.
        void push_paths(git_arguments& arguments, const std::span<const std::string> paths)
        {
            if (paths.empty())
                reject("paths", "no paths given");
            push_text(arguments, "--");
            for (const std::string& path : paths)
                push_text(arguments, require_path(path));
        }

        std::string diff_context(const std::uint32_t context_lines)
        {
            return std::string { "-U" }.append(std::to_string(context_lines));
        }

        // diff 출력을 해석기가 기대하는 모양으로 고정하는 공통 인자다.
        void push_diff_basics(git_arguments& arguments, const std::uint32_t context_lines)
        {
            push_all(arguments, { "--no-color", "--no-ext-diff", "--src-prefix=a/", "--dst-prefix=b/" });
            push_text(arguments, diff_context(context_lines));
        }

        std::string stash_selector(const std::uint32_t index)
        {
            return std::string { "stash@{" }.append(std::to_string(index)).append("}");
        }

        git_arguments make_literal_pathspec_command(const std::initializer_list<std::string_view> command)
        {
            git_arguments arguments {};
            push_text(arguments, "--literal-pathspecs");
            push_all(arguments, command);
            return arguments;
        }

        git_arguments make_sequence_control_args(const std::string_view command, const std::string_view control, const bool without_editor)
        {
            git_arguments arguments {};
            if (without_editor)
                push_all(arguments, { "-c", "core.editor=true" });
            push_text(arguments, command);
            push_text(arguments, control);
            return arguments;
        }

        void push_revisions(git_arguments& arguments, const std::span<const std::string> revisions, const std::uint32_t mainline)
        {
            if (revisions.empty())
                reject("revisions", "no revisions given");
            if (mainline != 0)
            {
                push_text(arguments, "-m");
                push_text(arguments, std::to_string(mainline));
            }
            push_text(arguments, "--");
            for (const std::string& revision : revisions)
                push_text(arguments, require_revision(revision, "revision"));
        }
    } // namespace

    // ------------------------------------------------------------------ 조회

    git_arguments make_git_rev_parse_args()
    {
        git_arguments arguments {};
        push_all(arguments, { "rev-parse", "--absolute-git-dir", "--show-toplevel" });
        return arguments;
    }

    std::vector<std::string> make_in_progress_marker_paths(const std::string_view git_dir)
    {
        std::vector<std::string> paths {};
        paths.reserve(repository_operation_marker_names.size());
        for (const std::string_view name : repository_operation_marker_names)
        {
            std::string path { git_dir };
            if (path.empty() == false && path.back() != '/' && path.back() != '\\')
                path.push_back('/');
            path.append(name);
            paths.push_back(std::move(path));
        }
        return paths;
    }

    git_arguments make_git_status_args(const bool include_ignored)
    {
        git_arguments arguments {};
        push_all(arguments, { "status", "--porcelain=v2", "--branch", "-z", "--untracked-files=all" });
        if (include_ignored)
            push_text(arguments, "--ignored");
        return arguments;
    }

    git_arguments make_git_for_each_ref_args()
    {
        git_arguments arguments {};
        push_all(arguments, { "for-each-ref", for_each_ref_format, "--", "refs/heads", "refs/remotes", "refs/tags" });
        return arguments;
    }

    git_arguments make_git_log_args(const git_log_options& options)
    {
        git_arguments arguments {};
        push_all(arguments, { "log", "--topo-order", "-z", options.include_body ? log_format_with_body : log_format, "--branches", "--remotes", "--tags" });
        if (options.include_head)
            push_text(arguments, "HEAD");
        if (options.max_count != 0)
            push_text(arguments, std::string { "--max-count=" }.append(std::to_string(options.max_count)));
        if (options.skip != 0)
            push_text(arguments, std::string { "--skip=" }.append(std::to_string(options.skip)));
        push_text(arguments, "--");
        return arguments;
    }

    git_arguments make_git_show_commit_args(const std::string_view sha, const bool is_merge, const std::uint32_t context_lines)
    {
        git_arguments arguments {};
        push_all(arguments, { "show", "--format=" });
        if (is_merge)
            push_all(arguments, { "-m", "--first-parent" });
        push_diff_basics(arguments, context_lines);
        push_text(arguments, "--find-renames");
        push_all(arguments, { require_revision(sha, "commit"), "--" });
        return arguments;
    }

    git_arguments make_git_show_numstat_args(const std::string_view sha, const bool is_merge)
    {
        git_arguments arguments {};
        push_all(arguments, { "show", "--format=", "--numstat", "-z", "--find-renames" });
        if (is_merge)
            push_all(arguments, { "-m", "--first-parent" });
        push_all(arguments, { require_revision(sha, "commit"), "--" });
        return arguments;
    }

    git_arguments make_git_diff_args(const std::string_view path, const diff_source source, const std::uint32_t context_lines)
    {
        git_arguments arguments {};
        push_all(arguments, { "--literal-pathspecs", "diff" });
        push_diff_basics(arguments, context_lines);
        if (source == diff_source::staged)
            push_text(arguments, "--cached");
        push_all(arguments, { "--", require_path(path) });
        return arguments;
    }

    git_arguments make_git_diff_untracked_args(const std::string_view path, const std::uint32_t context_lines)
    {
        git_arguments arguments {};
        push_all(arguments, { "diff", "--no-index" });
        push_diff_basics(arguments, context_lines);
        push_all(arguments, { "--", "/dev/null", require_path(path) });
        return arguments;
    }

    git_arguments make_git_diff_numstat_args(const diff_source source)
    {
        git_arguments arguments {};
        push_all(arguments, { "diff", "--numstat", "-z" });
        if (source == diff_source::staged)
            push_all(arguments, { "--cached", "-M" });
        return arguments;
    }

    git_arguments make_git_stash_list_args()
    {
        git_arguments arguments {};
        push_all(arguments, { "stash", "list", "-z", stash_format });
        return arguments;
    }

    git_arguments make_git_ls_remote_tags_args(const std::string_view remote)
    {
        git_arguments arguments {};
        push_all(arguments, { "ls-remote", "--tags", "--", require_revision(remote, "remote") });
        return arguments;
    }

    // ------------------------------------------------------------------ 브랜치

    git_arguments make_git_switch_args(const std::string_view branch)
    {
        git_arguments arguments {};
        push_all(arguments, { "switch", "--", require_revision(branch, "branch") });
        return arguments;
    }

    git_arguments make_git_switch_detach_args(const std::string_view revision)
    {
        git_arguments arguments {};
        push_all(arguments, { "switch", "--detach", "--", require_revision(revision, "revision") });
        return arguments;
    }

    git_arguments make_git_switch_create_args(const std::string_view new_branch, const std::string_view start_point, const bool track)
    {
        git_arguments arguments {};
        push_all(arguments, { "switch", "-c", require_new_ref_name(new_branch, "branch name") });
        if (track)
            push_text(arguments, "--track");
        if (start_point.empty() == false)
            push_all(arguments, { "--", require_revision(start_point, "start point") });
        return arguments;
    }

    git_arguments make_git_branch_create_args(const std::string_view name, const std::string_view start_point)
    {
        git_arguments arguments {};
        push_all(arguments, { "branch", "--", require_new_ref_name(name, "branch name") });
        if (start_point.empty() == false)
            push_text(arguments, require_revision(start_point, "start point"));
        return arguments;
    }

    git_arguments make_git_branch_rename_args(const std::string_view old_name, const std::string_view new_name)
    {
        git_arguments arguments {};
        push_all(arguments, { "branch", "-m", "--", require_revision(old_name, "branch"), require_new_ref_name(new_name, "branch name") });
        return arguments;
    }

    git_arguments make_git_branch_delete_args(const std::string_view name, const bool force)
    {
        git_arguments arguments {};
        push_all(arguments, { "branch", force ? "-D" : "-d", "--", require_revision(name, "branch") });
        return arguments;
    }

    // ------------------------------------------------------------------ 커밋과 index

    git_arguments make_git_commit_args(const git_commit_options& options)
    {
        if (options.keep_message && options.amend == false)
            reject("commit", "keep_message requires amend");

        git_arguments arguments {};
        push_text(arguments, "commit");
        if (options.amend)
            push_text(arguments, "--amend");
        if (options.allow_empty)
            push_text(arguments, "--allow-empty");
        if (options.keep_message)
            push_text(arguments, "--no-edit");
        else
            push_all(arguments, { "-F", "-" });
        return arguments;
    }

    git_arguments make_git_add_args(const std::span<const std::string> paths)
    {
        git_arguments arguments { make_literal_pathspec_command({ "add" }) };
        push_paths(arguments, paths);
        return arguments;
    }

    git_arguments make_git_restore_staged_args(const std::span<const std::string> paths)
    {
        git_arguments arguments { make_literal_pathspec_command({ "restore", "--staged" }) };
        push_paths(arguments, paths);
        return arguments;
    }

    git_arguments make_git_restore_worktree_args(const std::span<const std::string> paths)
    {
        git_arguments arguments { make_literal_pathspec_command({ "restore" }) };
        push_paths(arguments, paths);
        return arguments;
    }

    git_arguments make_git_add_from_stdin_args()
    {
        return make_literal_pathspec_command({ "add", "--pathspec-from-file=-", "--pathspec-file-nul" });
    }

    git_arguments make_git_restore_staged_from_stdin_args()
    {
        return make_literal_pathspec_command({ "restore", "--staged", "--pathspec-from-file=-", "--pathspec-file-nul" });
    }

    git_arguments make_git_restore_worktree_from_stdin_args()
    {
        return make_literal_pathspec_command({ "restore", "--pathspec-from-file=-", "--pathspec-file-nul" });
    }

    git_arguments make_git_apply_args(const git_apply_options& options)
    {
        git_arguments arguments {};
        push_text(arguments, "apply");
        if (options.cached)
            push_text(arguments, "--cached");
        if (options.reverse)
            push_text(arguments, "-R");
        if (options.check_only)
            push_text(arguments, "--check");
        if (options.recount)
            push_text(arguments, "--recount");
        if (options.unidiff_zero)
            push_text(arguments, "--unidiff-zero");
        push_text(arguments, "-");
        return arguments;
    }

    // ------------------------------------------------------------------ 원격

    git_arguments make_git_fetch_args(const git_fetch_options& options)
    {
        git_arguments arguments {};
        push_text(arguments, "fetch");
        if (options.prune)
            push_text(arguments, "--prune");
        push_text(arguments, "--progress");
        if (options.tags)
            push_text(arguments, "--tags");
        if (options.all)
            push_text(arguments, "--all");
        else if (options.remote.empty() == false)
            push_all(arguments, { "--", require_revision(options.remote, "remote") });
        return arguments;
    }

    git_arguments make_git_pull_args(const git_pull_mode mode, const std::string_view remote, const std::string_view branch)
    {
        git_arguments arguments {};
        push_all(arguments, { "pull", mode == git_pull_mode::rebase ? "--rebase" : mode == git_pull_mode::merge ? "--no-rebase" : "--ff-only" });
        if (remote.empty() == false)
        {
            push_all(arguments, { "--", require_revision(remote, "remote") });
            if (branch.empty() == false)
                push_text(arguments, require_revision(branch, "branch"));
        }
        else if (branch.empty() == false)
        {
            reject("pull", "branch requires remote");
        }
        return arguments;
    }

    git_arguments make_git_push_args(const git_push_options& options)
    {
        git_arguments arguments {};
        push_all(arguments, { "push", "--progress" });
        if (options.set_upstream)
            push_text(arguments, "--set-upstream");
        if (options.follow_tags)
            push_text(arguments, "--follow-tags");
        if (options.force_with_lease)
            push_text(arguments, "--force-with-lease");
        if (options.remote.empty() == false)
        {
            push_all(arguments, { "--", require_revision(options.remote, "remote") });
            if (options.refspec.empty() == false)
                push_text(arguments, require_revision(options.refspec, "refspec"));
        }
        else if (options.refspec.empty() == false)
        {
            reject("push", "refspec requires remote");
        }
        return arguments;
    }

    git_arguments make_git_push_tag_args(const std::string_view remote, const std::string_view tag)
    {
        git_arguments arguments {};
        push_all(arguments, { "push", "--progress", "--", require_revision(remote, "remote") });
        push_text(arguments, std::string { "refs/tags/" }.append(require_new_ref_name(tag, "tag name")));
        return arguments;
    }

    git_arguments make_git_push_all_tags_args(const std::string_view remote)
    {
        git_arguments arguments {};
        push_all(arguments, { "push", "--progress", "--tags", "--", require_revision(remote, "remote") });
        return arguments;
    }

    git_arguments make_git_delete_remote_tag_args(const std::string_view remote, const std::string_view tag)
    {
        git_arguments arguments {};
        push_all(arguments, { "push", "--progress", "--delete", "--", require_revision(remote, "remote") });
        push_text(arguments, std::string { "refs/tags/" }.append(require_new_ref_name(tag, "tag name")));
        return arguments;
    }

    git_arguments make_git_delete_remote_branch_args(const std::string_view remote, const std::string_view branch)
    {
        git_arguments arguments {};
        push_all(arguments, { "push", "--progress", "--delete", "--", require_revision(remote, "remote") });
        push_text(arguments, std::string { "refs/heads/" }.append(require_new_ref_name(branch, "branch name")));
        return arguments;
    }

    git_arguments make_git_clone_args(const git_clone_options& options)
    {
        git_arguments arguments {};
        push_all(arguments, { "clone", "--progress" });
        if (options.branch.empty() == false)
            push_all(arguments, { "--branch", require_revision(options.branch, "branch") });
        if (options.depth != 0)
            push_all(arguments, { "--depth", std::to_string(options.depth) });
        push_all(arguments, { "--", require_revision(options.url, "url"), require_path(options.directory) });
        return arguments;
    }

    git_arguments make_git_init_args(const std::string_view directory, const std::string_view initial_branch)
    {
        git_arguments arguments {};
        push_text(arguments, "init");
        if (initial_branch.empty() == false)
            push_text(arguments, std::string { "--initial-branch=" }.append(require_new_ref_name(initial_branch, "branch name")));
        push_all(arguments, { "--", require_path(directory) });
        return arguments;
    }

    // ------------------------------------------------------------------ 태그

    git_arguments make_git_tag_args(const std::string_view name, const std::string_view target)
    {
        git_arguments arguments {};
        push_all(arguments, { "tag", "--", require_new_ref_name(name, "tag name") });
        if (target.empty() == false)
            push_text(arguments, require_revision(target, "tag target"));
        return arguments;
    }

    git_arguments make_git_annotated_tag_args(const std::string_view name, const std::string_view target)
    {
        git_arguments arguments {};
        push_all(arguments, { "tag", "-a", "-F", "-", "--", require_new_ref_name(name, "tag name") });
        if (target.empty() == false)
            push_text(arguments, require_revision(target, "tag target"));
        return arguments;
    }

    git_arguments make_git_delete_tag_args(const std::string_view name)
    {
        git_arguments arguments {};
        push_all(arguments, { "tag", "-d", "--", require_revision(name, "tag") });
        return arguments;
    }

    // ------------------------------------------------------------------ 병합과 rebase

    git_arguments make_git_merge_args(const std::string_view revision, const git_merge_mode mode)
    {
        git_arguments arguments {};
        push_all(arguments, { "merge", "--no-edit" });
        if (mode == git_merge_mode::fast_forward_only)
            push_text(arguments, "--ff-only");
        else if (mode == git_merge_mode::no_fast_forward)
            push_text(arguments, "--no-ff");
        push_all(arguments, { "--", require_revision(revision, "revision") });
        return arguments;
    }

    git_arguments make_git_merge_abort_args()
    {
        git_arguments arguments {};
        push_all(arguments, { "merge", "--abort" });
        return arguments;
    }

    git_arguments make_git_rebase_args(const std::string_view onto, const std::string_view branch, const bool autostash)
    {
        git_arguments arguments {};
        push_text(arguments, "rebase");
        if (autostash)
            push_text(arguments, "--autostash");
        push_all(arguments, { "--", require_revision(onto, "onto") });
        if (branch.empty() == false)
            push_text(arguments, require_revision(branch, "branch"));
        return arguments;
    }

    git_arguments make_git_rebase_continue_args()
    {
        return make_sequence_control_args("rebase", "--continue", true);
    }

    git_arguments make_git_rebase_skip_args()
    {
        return make_sequence_control_args("rebase", "--skip", false);
    }

    git_arguments make_git_rebase_abort_args()
    {
        return make_sequence_control_args("rebase", "--abort", false);
    }

    git_arguments make_git_cherry_pick_args(const std::span<const std::string> revisions, const std::uint32_t mainline)
    {
        git_arguments arguments {};
        push_text(arguments, "cherry-pick");
        push_revisions(arguments, revisions, mainline);
        return arguments;
    }

    git_arguments make_git_cherry_pick_continue_args()
    {
        return make_sequence_control_args("cherry-pick", "--continue", true);
    }

    git_arguments make_git_cherry_pick_abort_args()
    {
        return make_sequence_control_args("cherry-pick", "--abort", false);
    }

    git_arguments make_git_revert_args(const std::span<const std::string> revisions, const std::uint32_t mainline)
    {
        git_arguments arguments {};
        push_all(arguments, { "revert", "--no-edit" });
        push_revisions(arguments, revisions, mainline);
        return arguments;
    }

    git_arguments make_git_revert_continue_args()
    {
        return make_sequence_control_args("revert", "--continue", true);
    }

    git_arguments make_git_revert_abort_args()
    {
        return make_sequence_control_args("revert", "--abort", false);
    }

    git_arguments make_git_reset_args(const git_reset_mode mode, const std::string_view revision)
    {
        const std::string_view option { mode == git_reset_mode::soft ? "--soft" : mode == git_reset_mode::hard ? "--hard" : "--mixed" };
        git_arguments arguments {};
        // `--`를 식 앞에 두면 식이 경로로 읽히므로 식 뒤에 둔다.
        push_all(arguments, { "reset", option, require_revision(revision, "revision"), "--" });
        return arguments;
    }

    // ------------------------------------------------------------------ stash

    git_arguments make_git_stash_push_args(const git_stash_push_options& options)
    {
        git_arguments arguments {};
        push_all(arguments, { "stash", "push" });
        if (options.include_untracked)
            push_text(arguments, "--include-untracked");
        if (options.keep_index)
            push_text(arguments, "--keep-index");
        if (options.message.empty() == false)
        {
            if (options.message.find('\0') != std::string::npos)
                reject("stash message", "message contains NUL");
            push_all(arguments, { "-m", options.message });
        }
        return arguments;
    }

    git_arguments make_git_stash_apply_args(const std::uint32_t index, const bool restore_index)
    {
        git_arguments arguments {};
        push_all(arguments, { "stash", "apply" });
        if (restore_index)
            push_text(arguments, "--index");
        push_text(arguments, stash_selector(index));
        return arguments;
    }

    git_arguments make_git_stash_pop_args(const std::uint32_t index, const bool restore_index)
    {
        git_arguments arguments {};
        push_all(arguments, { "stash", "pop" });
        if (restore_index)
            push_text(arguments, "--index");
        push_text(arguments, stash_selector(index));
        return arguments;
    }

    git_arguments make_git_stash_drop_args(const std::uint32_t index)
    {
        git_arguments arguments {};
        push_all(arguments, { "stash", "drop", stash_selector(index) });
        return arguments;
    }

    // ------------------------------------------------------------------ 저장소 세션

    namespace {
        void push_switch_mode(git_arguments& arguments, const git_switch_mode mode)
        {
            if (mode == git_switch_mode::discard_changes)
                push_text(arguments, "--discard-changes");
            else if (mode == git_switch_mode::merge)
                push_text(arguments, "--merge");
        }
    } // namespace

    git_arguments make_git_switch_args(const std::string_view branch, const git_switch_mode mode)
    {
        git_arguments arguments {};
        push_text(arguments, "switch");
        push_switch_mode(arguments, mode);
        push_all(arguments, { "--", require_revision(branch, "branch") });
        return arguments;
    }

    git_arguments make_git_switch_detach_args(const std::string_view revision, const git_switch_mode mode)
    {
        git_arguments arguments {};
        push_all(arguments, { "switch", "--detach" });
        push_switch_mode(arguments, mode);
        push_all(arguments, { "--", require_revision(revision, "revision") });
        return arguments;
    }

    git_arguments make_git_switch_create_args(const std::string_view new_branch, const std::string_view start_point, const bool track, const git_switch_mode mode)
    {
        git_arguments arguments {};
        push_all(arguments, { "switch", "-c", require_new_ref_name(new_branch, "branch name") });
        if (track)
            push_text(arguments, "--track");
        push_switch_mode(arguments, mode);
        if (start_point.empty() == false)
            push_all(arguments, { "--", require_revision(start_point, "start point") });
        return arguments;
    }

    git_arguments make_git_fetch_fast_forward_args(const std::string_view source_ref, const std::string_view target_ref)
    {
        std::string refspec { require_revision(source_ref, "source ref") };
        refspec.push_back(':');
        refspec.append(require_revision(target_ref, "target ref"));
        git_arguments arguments {};
        push_all(arguments, { "fetch", "--", ".", refspec });
        return arguments;
    }

    git_arguments make_git_is_ancestor_args(const std::string_view ancestor, const std::string_view descendant)
    {
        git_arguments arguments {};
        push_all(arguments, { "merge-base", "--is-ancestor", require_revision(ancestor, "ancestor"), require_revision(descendant, "descendant") });
        return arguments;
    }

    git_arguments make_git_count_commits_args(const std::string_view from, const std::string_view to)
    {
        std::string range { require_revision(from, "from") };
        range.append("..").append(require_revision(to, "to"));
        git_arguments arguments {};
        push_all(arguments, { "rev-list", "--count", range, "--" });
        return arguments;
    }

    git_arguments make_git_remote_list_args()
    {
        git_arguments arguments {};
        push_all(arguments, { "remote", "-v" });
        return arguments;
    }

    git_arguments make_git_add_all_args()
    {
        git_arguments arguments {};
        push_all(arguments, { "add", "--all" });
        return arguments;
    }

    git_arguments make_git_checkout_conflict_side_args(const std::string_view path, const bool ours)
    {
        git_arguments arguments {};
        push_all(arguments, { literal_pathspecs, "checkout", ours ? "--ours" : "--theirs", "--", require_path(path) });
        return arguments;
    }

    git_arguments make_git_merge_continue_args()
    {
        return make_sequence_control_args("merge", "--continue", true);
    }

    git_arguments make_git_cherry_pick_skip_args()
    {
        return make_sequence_control_args("cherry-pick", "--skip", false);
    }

    git_arguments make_git_revert_skip_args()
    {
        return make_sequence_control_args("revert", "--skip", false);
    }

    git_arguments make_git_commit_info_args(const std::string_view sha)
    {
        git_arguments arguments {};
        push_all(arguments, { "log", "-1", "-z", log_format_with_body, require_revision(sha, "commit"), "--" });
        return arguments;
    }

    git_arguments make_git_commit_name_status_args(const std::string_view sha, const bool is_merge)
    {
        git_arguments arguments {};
        push_all(arguments, { "log", "-1", "-z", "--format=", "--name-status", "--find-renames" });
        if (is_merge)
            push_all(arguments, { "-m", "--first-parent" });
        push_all(arguments, { require_revision(sha, "commit"), "--" });
        return arguments;
    }

    git_arguments make_git_show_file_args(const std::string_view sha, const std::string_view path, const bool is_merge, const std::uint32_t context_lines)
    {
        git_arguments arguments {};
        push_all(arguments, { literal_pathspecs, "show", "--format=" });
        if (is_merge)
            push_all(arguments, { "-m", "--first-parent" });
        push_diff_basics(arguments, context_lines);
        push_all(arguments, { require_revision(sha, "commit"), "--", require_path(path) });
        return arguments;
    }

    git_arguments make_git_current_branch_args()
    {
        git_arguments arguments {};
        push_all(arguments, { "symbolic-ref", "--quiet", "--short", "HEAD" });
        return arguments;
    }

    git_arguments make_git_ref_exists_args(const std::string_view full_ref)
    {
        git_arguments arguments {};
        push_all(arguments, { "show-ref", "--verify", "--quiet", require_revision(full_ref, "ref") });
        return arguments;
    }

    git_arguments make_git_symbolic_full_name_args(const std::string_view revision)
    {
        git_arguments arguments {};
        push_all(arguments, { "rev-parse", "--symbolic-full-name", require_revision(revision, "revision") });
        return arguments;
    }
} // namespace cyella
