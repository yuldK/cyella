#include "infrastructure/git_command_builder.h"

#include "domain/ref_name.h"
#include "domain/repository_summary.h"

#include <algorithm>
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
        // 파일 이력의 레코드다. 필드는 `log_format`과 같고 (`git_log_parser`로 읽는다), 뒤따르는 name-status와 섞이지 않게 레코드를
        // 0x1E로 시작한다.
        constexpr std::string_view file_log_format { "--format=%x1e%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%ct%x1f%s" };
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

        constexpr std::string_view local_ref_prefix { "refs/heads/" };
        constexpr std::string_view remote_ref_prefix { "refs/remotes/" };
        constexpr std::string_view tag_ref_prefix { "refs/tags/" };

        // 브랜치 필터의 ref 이름이다. 세 묶음 가운데 하나의 전체 이름이어야 `--exclude`의 짧은 이름으로 옮길 수 있고,
        // `only`의 시작점으로 주었을 때 같은 이름의 브랜치·태그와 헷갈리지 않는다.
        std::string_view require_graph_ref(const std::string_view value)
        {
            const auto under = [value](const std::string_view prefix) { return value.size() > prefix.size() && value.starts_with(prefix); };
            if (under(local_ref_prefix) == false && under(remote_ref_prefix) == false && under(tag_ref_prefix) == false)
                reject("graph ref", "not a full branch, remote branch or tag ref name");
            // `--exclude`의 값은 glob이다. git은 ref 이름에 이 글자들을 받지 않으므로(`git check-ref-format`) 실제 ref에는 없고,
            // 있다면 다른 ref까지 걸러 낼 패턴이다.
            if (value.find_first_of("*?[\\") != std::string_view::npos)
                reject("graph ref", "name contains a glob character");
            return require_revision(value, "graph ref");
        }

        // 숨긴 ref 가운데 `prefix` 묶음의 것을 `--exclude=<짧은 이름>`으로 넣는다. 하나라도 넣었으면 true다.
        // `--exclude`는 바로 다음 묶음 옵션 하나에만 걸리므로 부르는 쪽이 곧이어 그 묶음 옵션을 넣는다.
        bool push_hidden_refs(git_arguments& arguments, const git_graph_refs& graph, const std::string_view prefix)
        {
            bool pushed { false };
            for (const std::string& name : graph.filter.hidden)
            {
                const std::string_view full { require_graph_ref(name) };
                if (full.starts_with(prefix) == false)
                    continue;
                const std::string_view short_name { full.substr(prefix.size()) };
                // 현재 브랜치는 숨기지 않는다. 화면이 먼저 막지만, 숨긴 브랜치로 옮겨 간 직후처럼 낡은 필터가 올 수 있다.
                if (prefix == local_ref_prefix && graph.current_branch.empty() == false && short_name == graph.current_branch)
                    continue;
                push_text(arguments, std::string { "--exclude=" }.append(short_name));
                pushed = true;
            }
            return pushed;
        }

        // 그래프에 보이는 커밋의 시작점이다. 그래프 log와 커밋 검색이 이 한곳을 함께 써야, 검색이 그래프에 없는 커밋을 내거나
        // 그래프를 좁힐 때 한쪽만 좁혀지지 않는다. 첫 커밋 전에는 HEAD를 풀 수 없어 git이 실패하므로 뺀다.
        // 인자의 꼴과 git으로 확인한 `--exclude`의 동작은 `git_graph_refs`의 설명에 적었다.
        void push_graph_refs(git_arguments& arguments, const git_graph_refs& graph)
        {
            if (graph.filter.only.empty() == false)
            {
                for (const std::string& name : graph.filter.only)
                    push_text(arguments, require_graph_ref(name));
                return;
            }
            static_cast<void>(push_hidden_refs(arguments, graph, local_ref_prefix));
            push_text(arguments, "--branches");
            // 원격의 기본 브랜치를 가리키는 `<remote>/HEAD`가 남으면 숨긴 원격 브랜치의 커밋이 그리로 닿는다.
            if (push_hidden_refs(arguments, graph, remote_ref_prefix))
                push_text(arguments, "--exclude=*/HEAD");
            push_text(arguments, "--remotes");
            static_cast<void>(push_hidden_refs(arguments, graph, tag_ref_prefix));
            push_text(arguments, "--tags");
            if (graph.include_head)
                push_text(arguments, "HEAD");
        }

        constexpr std::size_t minimum_hash_prefix_length { 4 };
        constexpr std::size_t maximum_hash_length { 64 };

        std::string_view require_search_text(const std::string_view value)
        {
            if (value.empty())
                reject("query", "query is empty");
            // 인자에 NUL은 실을 수 없고, 줄바꿈은 git이 패턴 여럿으로 나눠 읽는다 (글자 그대로의 한 덩어리가 아니게 된다).
            if (value.find_first_of(std::string_view { "\0\n", 2 }) != std::string_view::npos)
                reject("query", "query contains NUL or a line break");
            return value;
        }

        std::string_view require_hash_prefix(const std::string_view value)
        {
            if (is_commit_hash_prefix(value) == false)
                reject("commit", "not a hexadecimal object name of 4 to 64 digits");
            return value;
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
        push_all(arguments, { "log", "--topo-order", "-z", options.include_body ? log_format_with_body : log_format });
        push_graph_refs(arguments, options.graph);
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

    git_arguments make_git_check_attr_diff_args(const std::string_view path)
    {
        git_arguments arguments {};
        push_all(arguments, { "check-attr", "-z", "diff", "--", require_path(path) });
        return arguments;
    }

    git_arguments make_git_config_get_args(const std::string_view key)
    {
        if (key.empty() || key.front() == '-' || key.find('\0') != std::string_view::npos || key.find('\n') != std::string_view::npos)
            reject("config key", "key is empty or looks like an option");
        git_arguments arguments {};
        push_all(arguments, { "config", "--get", key });
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

    git_arguments make_git_staged_patch_args(const std::span<const std::string> excluded_paths)
    {
        git_arguments arguments {};
        push_text(arguments, "diff");
        push_diff_basics(arguments, 3);
        push_all(arguments, { "--cached", "-M", "--" });
        // 빼는 pathspec만 주면 git이 "전부에서 뺀다"로 읽지만, 그 기본 동작에 기대지 않고 저장소 전체를 먼저 적는다.
        push_text(arguments, ":(top)");
        for (const std::string& path : excluded_paths)
            push_text(arguments, std::string { ":(top,exclude,literal)" }.append(require_path(path)));
        return arguments;
    }

    git_arguments make_git_recent_subjects_args(const std::uint32_t count)
    {
        git_arguments arguments {};
        push_all(arguments, { "log", std::string { "--max-count=" }.append(std::to_string(count)), "--format=%s", "HEAD", "--" });
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

    git_arguments make_git_branch_set_upstream_args(const std::string_view branch, const std::string_view remote, const std::string_view remote_branch)
    {
        // 원격 이름과 브랜치 이름은 ref 이름 규칙을 지켜야 한다. `--set-upstream-to=` 뒤에 붙으므로 맨 앞 `-`는 옵션이 되지 않지만 이름 규칙으로 함께 거른다.
        const std::string_view checked_remote { require_new_ref_name(remote, "remote") };
        const std::string_view checked_remote_branch { require_new_ref_name(remote_branch, "remote branch") };
        std::string upstream { "--set-upstream-to=" };
        upstream.append(checked_remote).push_back('/');
        upstream.append(checked_remote_branch);
        git_arguments arguments {};
        push_all(arguments, { "branch", upstream, "--", require_revision(branch, "branch") });
        return arguments;
    }

    git_arguments make_git_branch_unset_upstream_args(const std::string_view branch)
    {
        git_arguments arguments {};
        push_all(arguments, { "branch", "--unset-upstream", "--", require_revision(branch, "branch") });
        return arguments;
    }

    git_arguments make_git_remote_add_args(const std::string_view name, const std::string_view url)
    {
        // 원격 이름은 `refs/remotes/<name>/` 아래 ref가 되므로 ref 이름 규칙을 지켜야 한다.
        const std::string_view checked_name { require_new_ref_name(name, "remote") };
        if (url.empty())
            reject("remote url", "url is empty");
        if (url.front() == '-')
            reject("remote url", "url looks like an option");
        if (std::any_of(url.begin(), url.end(), [](const char value) { return static_cast<unsigned char>(value) < 0x20 || value == 0x7f; }))
            reject("remote url", "url contains a control character");
        git_arguments arguments {};
        push_all(arguments, { "remote", "add", "--", checked_name, url });
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
        if (options.sign.has_value())
            push_text(arguments, *options.sign ? "-S" : "--no-gpg-sign");
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
        // `restore --staged`는 원본을 HEAD로 잡아 첫 커밋 전 저장소에서 "could not resolve HEAD"로 실패한다.
        // `reset`은 HEAD가 없으면 빈 트리를 기준으로 삼으므로 두 경우 모두 index 항목을 HEAD 상태로 되돌린다.
        git_arguments arguments { make_literal_pathspec_command({ "reset", "-q" }) };
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
        return make_literal_pathspec_command({ "reset", "-q", "--pathspec-from-file=-", "--pathspec-file-nul" });
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
        // 명령줄에 없으면 사용자의 `apply.whitespace`가 기본값이 된다. `fix`면 줄 끝 공백을 지운 내용이
        // index에 들어가 화면에 보인 hunk와 달라지고, `error`면 그런 hunk의 stage가 실패한다.
        push_text(arguments, "--whitespace=nowarn");
        push_text(arguments, "-");
        return arguments;
    }

    // ------------------------------------------------------------------ 원격

    git_arguments make_git_fetch_args(const git_fetch_options& options)
    {
        git_arguments arguments {};
        push_text(arguments, "fetch");
        if (options.refspec.empty() == false)
        {
            if (options.remote.empty())
                reject("refspec", "requires remote");
            // refspec은 `[+]src:dst` 꼴이고 ref 이름 규칙을 따른다. 옵션처럼 보이거나 공백·제어 문자가 있으면 거절한다.
            if (options.refspec.front() == '-' || std::ranges::any_of(options.refspec, [](const char value) { return static_cast<unsigned char>(value) <= 0x20 || value == 0x7f || value == '\\'; }))
                reject("refspec", "invalid characters");
            push_all(arguments, { "--progress", "--", require_revision(options.remote, "remote"), options.refspec });
            return arguments;
        }
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

    git_arguments make_git_tag_args(const std::string_view name, const std::string_view target, const bool no_sign)
    {
        git_arguments arguments {};
        push_text(arguments, "tag");
        if (no_sign)
            push_text(arguments, "--no-sign");
        push_all(arguments, { "--", require_new_ref_name(name, "tag name") });
        if (target.empty() == false)
            push_text(arguments, require_revision(target, "tag target"));
        return arguments;
    }

    git_arguments make_git_annotated_tag_args(const std::string_view name, const std::string_view target, const bool no_sign)
    {
        git_arguments arguments {};
        push_all(arguments, { "tag", "-a" });
        if (no_sign)
            push_text(arguments, "--no-sign");
        push_all(arguments, { "-F", "-", "--", require_new_ref_name(name, "tag name") });
        if (target.empty() == false)
            push_text(arguments, require_revision(target, "tag target"));
        return arguments;
    }

    git_arguments make_git_signed_tag_args(const std::string_view name, const std::string_view target)
    {
        git_arguments arguments {};
        push_all(arguments, { "tag", "-s", "-F", "-", "--", require_new_ref_name(name, "tag name") });
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
        // `--root`가 없으면 사용자의 `log.showRoot=false`가 첫 커밋의 diff를 빼서 파일 목록이 빈다.
        push_all(arguments, { "log", "-1", "-z", "--format=", "--name-status", "--find-renames", "--root" });
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

    git_arguments make_git_diff_revisions_name_status_args(const std::string_view base, const std::string_view tip)
    {
        git_arguments arguments {};
        push_all(arguments, { "diff", "-z", "--name-status", "--find-renames", require_revision(base, "base"), require_revision(tip, "tip"), "--" });
        return arguments;
    }

    git_arguments make_git_diff_revisions_numstat_args(const std::string_view base, const std::string_view tip)
    {
        git_arguments arguments {};
        push_all(arguments, { "diff", "--numstat", "-z", "--find-renames", require_revision(base, "base"), require_revision(tip, "tip"), "--" });
        return arguments;
    }

    git_arguments make_git_diff_revisions_file_args(const std::string_view base, const std::string_view tip, const std::string_view path, const std::uint32_t context_lines)
    {
        git_arguments arguments {};
        push_all(arguments, { literal_pathspecs, "diff" });
        push_diff_basics(arguments, context_lines);
        push_all(arguments, { require_revision(base, "base"), require_revision(tip, "tip"), "--", require_path(path) });
        return arguments;
    }

    git_arguments make_git_count_reachable_args(const std::string_view tip)
    {
        git_arguments arguments {};
        push_all(arguments, { "rev-list", "--count", require_revision(tip, "tip"), "--" });
        return arguments;
    }

    git_arguments make_git_empty_tree_args()
    {
        git_arguments arguments {};
        push_all(arguments, { "hash-object", "-t", "tree", "--stdin" });
        return arguments;
    }

    git_arguments make_git_blame_args(const std::string_view path, const std::string_view revision)
    {
        git_arguments arguments {};
        push_all(arguments, { "blame", "--porcelain" });
        if (revision.empty() == false)
            push_text(arguments, require_revision(revision, "revision"));
        push_all(arguments, { "--", require_path(path) });
        return arguments;
    }

    git_arguments make_git_file_log_args(const std::string_view path, const std::string_view revision, const std::uint32_t max_count)
    {
        git_arguments arguments {};
        push_all(arguments, { literal_pathspecs, "log", "--follow", "-z", "--name-status", "--root", file_log_format });
        if (max_count != 0)
            push_text(arguments, std::string { "--max-count=" }.append(std::to_string(max_count)));
        push_all(arguments, { revision.empty() ? std::string_view { "HEAD" } : require_revision(revision, "revision"), "--", require_path(path) });
        return arguments;
    }

    bool is_commit_hash_prefix(const std::string_view text) noexcept
    {
        const auto is_hex_digit = [](const char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') || (value >= 'A' && value <= 'F'); };
        return text.size() >= minimum_hash_prefix_length && text.size() <= maximum_hash_length && std::all_of(text.begin(), text.end(), is_hex_digit);
    }

    git_arguments make_git_search_log_args(const std::string_view query, const commit_search_field field, const git_graph_refs& graph, const std::uint32_t max_count)
    {
        git_arguments arguments {};
        push_all(arguments, { "log", "--topo-order", "-z", log_format, "--no-mailmap", "-i", "--fixed-strings" });
        // 값을 `=`로 옵션에 붙여 한 인자로 넘긴다. `--all`처럼 옵션으로 보이는 질의도 찾을 글일 뿐이다.
        push_text(arguments, std::string { field == commit_search_field::author ? "--author=" : "--grep=" }.append(require_search_text(query)));
        if (max_count != 0)
            push_text(arguments, std::string { "--max-count=" }.append(std::to_string(max_count)));
        push_graph_refs(arguments, graph);
        push_text(arguments, "--");
        return arguments;
    }

    git_arguments make_git_graph_reachability_args(const std::string_view sha, const git_graph_refs& graph)
    {
        git_arguments arguments {};
        push_all(arguments, { "rev-list", "--max-count=1", require_hash_prefix(sha), "--not" });
        push_graph_refs(arguments, graph);
        push_text(arguments, "--");
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
