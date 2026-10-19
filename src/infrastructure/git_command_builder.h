#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // git에 넘길 argv[1..]이다. 셸을 거치지 않으므로 인자의 메타 문자는 값의 일부다.
    // `process_request.arguments`와 같은 모양이라 프로세스 실행기에 그대로 담는다.
    //
    // 이 파일의 함수는 명령줄만 만들고 프로세스를 실행하지 않는다. 실행할 때의 약속:
    //   - 환경: LC_ALL=C(지역화 없는 stderr), GIT_TERMINAL_PROMPT=0, GIT_EDITOR=true.
    //   - 공통 앞 인자: `-c core.quotepath=false -c gc.auto=0 -c color.ui=false --no-pager`.
    //     해석기는 quotepath 설정과 무관하게 인용된 경로를 풀므로 필수는 아니다.
    //   - stdin이 필요한 명령은 각 함수의 설명에 적었다. 내용은 호출자가 넘긴다.
    //
    // 인자 검증: 새로 만드는 브랜치·태그 이름은 `find_ref_name_problem`으로, 기존 ref나
    // 커밋을 가리키는 식과 원격 이름은 `find_revision_problem`으로 검사한다. 어긋나면
    // std::invalid_argument를 던진다. 호출 전에 같은 함수로 미리 검사하면 사용자에게
    // 이유를 보일 수 있다. 가능한 모든 명령은 ref와 경로 앞에 `--`를 둔다. `rev-parse`
    // `reset`처럼 `--` 뒤를 경로로 읽는 명령은 식 뒤에 `--`를 둔다.
    using git_arguments = std::vector<std::u8string>;

    // ------------------------------------------------------------------ 조회

    // `rev-parse --absolute-git-dir --show-toplevel`. 출력은 두 줄이다. bare 저장소나
    // git 디렉터리 안에서는 `--show-toplevel`이 실패하므로 마지막에 둔다. 앞의 값은
    // 이미 출력된다. `--git-dir`은 상대 경로를 줄 수 있어 절대 경로 판을 쓴다.
    [[nodiscard]] git_arguments make_git_rev_parse_args();

    // 진행 중 작업의 표지 경로(`<git_dir>/MERGE_HEAD` 등)다. 존재 여부는 호출자가
    // 확인하고, 존재하는 것만 detect_repository_operation_state에 넘긴다.
    [[nodiscard]] std::vector<std::string> make_in_progress_marker_paths(std::string_view git_dir);

    // `status --porcelain=v2 --branch -z --untracked-files=all [--ignored]`.
    [[nodiscard]] git_arguments make_git_status_args(bool include_ignored = false);

    // refs/heads, refs/remotes, refs/tags를 parse_git_refs의 필드 순서로 나열한다.
    [[nodiscard]] git_arguments make_git_for_each_ref_args();

    struct git_log_options
    {
        // 0이면 제한이 없다.
        std::uint32_t max_count { 0 };
        std::uint32_t skip { 0 };
        // 첫 커밋 전 저장소에서는 HEAD를 해석할 수 없으므로 false로 둔다.
        bool include_head { true };
        // 본문(%b)도 받는다. git_log_parser의 has_body와 같아야 한다.
        bool include_body { false };
    };

    // `log --topo-order -z --format=... --branches --remotes --tags [HEAD] [--max-count]
    // [--skip] --`. 쪽 나누기는 `--skip`과 `--max-count`로 하며, 그 사이에 ref가
    // 바뀌면 쪽 경계가 어긋날 수 있다.
    [[nodiscard]] git_arguments make_git_log_args(const git_log_options& options = {});

    // 한 커밋의 diff다. 병합 커밋은 `-m --first-parent`로 첫 부모와의 일반 diff만 받는다.
    // 헤더는 `--format=`으로 없앤다. 첫 커밋(root)도 전체 내용을 diff로 보여 준다.
    [[nodiscard]] git_arguments make_git_show_commit_args(std::string_view sha, bool is_merge, std::uint32_t context_lines = 3);

    // 한 커밋의 `--numstat -z`다.
    [[nodiscard]] git_arguments make_git_show_numstat_args(std::string_view sha, bool is_merge);

    enum class diff_source
    {
        // 작업 트리와 index의 차이.
        unstaged,
        // index와 HEAD의 차이(`--cached`).
        staged,
    };

    // 파일 하나의 diff다. 접두어를 `a/` `b/`로 고정해 사용자 설정(`diff.noprefix` 등)과
    // 무관하게 한다. 이름 변경 감지는 경로 하나로는 의미가 없어 켜지 않는다.
    [[nodiscard]] git_arguments make_git_diff_args(std::string_view path, diff_source source, std::uint32_t context_lines = 3);

    // 추적하지 않는 파일의 diff다. 새 파일로 보이게 `/dev/null`과 비교한다. 차이가
    // 있으면 종료 코드가 1이므로 1을 실패로 보지 않아야 한다.
    [[nodiscard]] git_arguments make_git_diff_untracked_args(std::string_view path, std::uint32_t context_lines = 3);

    // 모든 파일의 `--numstat -z`다. staged에서는 이름 변경 감지(`-M`)를 켜서 status의
    // 이름 변경과 맞춘다.
    [[nodiscard]] git_arguments make_git_diff_numstat_args(diff_source source);

    // `stash list -z --format=...`. parse_git_stash_list의 필드 순서다.
    [[nodiscard]] git_arguments make_git_stash_list_args();

    // `ls-remote --tags -- <remote>`. 네트워크에 접근한다.
    [[nodiscard]] git_arguments make_git_ls_remote_tags_args(std::string_view remote);

    // ------------------------------------------------------------------ 브랜치

    // `switch -- <branch>`.
    [[nodiscard]] git_arguments make_git_switch_args(std::string_view branch);

    // `switch --detach -- <revision>`.
    [[nodiscard]] git_arguments make_git_switch_detach_args(std::string_view revision);

    // `switch -c <new_branch> [--track] [-- <start_point>]`. 원격 브랜치에서 추적
    // 브랜치를 만들 때는 start_point에 `origin/x`를 주고 track을 true로 한다.
    [[nodiscard]] git_arguments make_git_switch_create_args(std::string_view new_branch, std::string_view start_point = {}, bool track = false);

    // `branch -- <name> [<start_point>]`. 체크아웃하지 않고 만든다.
    [[nodiscard]] git_arguments make_git_branch_create_args(std::string_view name, std::string_view start_point = {});

    // `branch -m -- <old_name> <new_name>`.
    [[nodiscard]] git_arguments make_git_branch_rename_args(std::string_view old_name, std::string_view new_name);

    // `branch -d|-D -- <name>`. force면 병합되지 않은 브랜치도 지운다.
    [[nodiscard]] git_arguments make_git_branch_delete_args(std::string_view name, bool force);

    // ------------------------------------------------------------------ 커밋과 index

    struct git_commit_options
    {
        bool amend { false };
        // amend할 때 메시지를 바꾸지 않는다(`--no-edit`). 이 경우 stdin이 필요 없다.
        bool keep_message { false };
        bool allow_empty { false };
    };

    // 메시지는 stdin으로 받는다(`commit -F -`). 명령줄 길이와 인용 문제를 피하고 여러 줄
    // 메시지를 그대로 보존한다. keep_message이면 `commit --amend --no-edit`다.
    [[nodiscard]] git_arguments make_git_commit_args(const git_commit_options& options = {});

    // 경로는 리터럴이다. 글롭 문자가 든 파일 이름도 그 파일 하나를 뜻하도록
    // `--literal-pathspecs`를 앞에 둔다.
    // `add -- <paths>`.
    [[nodiscard]] git_arguments make_git_add_args(std::span<const std::string> paths);

    // `restore --staged -- <paths>`. unstage.
    [[nodiscard]] git_arguments make_git_restore_staged_args(std::span<const std::string> paths);

    // `restore -- <paths>`. 작업 트리의 변경을 버린다.
    [[nodiscard]] git_arguments make_git_restore_worktree_args(std::span<const std::string> paths);

    // 파일이 많아 명령줄 길이 한도(Windows 32 KiB)에 걸릴 때의 판이다. 경로를 NUL로
    // 구분해 stdin으로 넘긴다(`--pathspec-from-file=- --pathspec-file-nul`).
    [[nodiscard]] git_arguments make_git_add_from_stdin_args();
    [[nodiscard]] git_arguments make_git_restore_staged_from_stdin_args();
    [[nodiscard]] git_arguments make_git_restore_worktree_from_stdin_args();

    struct git_apply_options
    {
        // index에 적용한다(`--cached`). hunk·줄 단위 stage에 쓴다.
        bool cached { true };
        // 반대로 적용한다(`-R`). unstage·discard에 쓴다.
        bool reverse { false };
        // 적용하지 않고 가능한지만 본다(`--check`).
        bool check_only { false };
        // hunk 헤더의 줄 수를 다시 센다(`--recount`). 줄 단위로 patch를 직접 만들 때 필요하다.
        bool recount { false };
        // 문맥 없는 patch(`-U0`)를 받는다(`--unidiff-zero`).
        bool unidiff_zero { false };
    };

    // patch는 stdin으로 넘긴다(`apply ... -`).
    [[nodiscard]] git_arguments make_git_apply_args(const git_apply_options& options = {});

    // ------------------------------------------------------------------ 원격

    struct git_fetch_options
    {
        // 비우면 `--all`이 아닌 기본 원격이다. all이 true이면 무시된다.
        std::string remote {};
        bool all { false };
        // 모든 태그를 가져온다(`--tags`). 원격 태그 표시를 새로 하려 할 때 쓴다.
        bool tags { false };
    };

    // `fetch --prune --progress [--tags] [--all | -- <remote>]`.
    [[nodiscard]] git_arguments make_git_fetch_args(const git_fetch_options& options = {});

    enum class git_pull_mode
    {
        fast_forward_only,
        rebase,
    };

    // `pull --ff-only|--rebase [-- <remote> [<branch>]]`.
    [[nodiscard]] git_arguments make_git_pull_args(git_pull_mode mode, std::string_view remote = {}, std::string_view branch = {});

    struct git_push_options
    {
        // 비우면 기본 원격(upstream)이다.
        std::string remote {};
        // 비우면 현재 브랜치 설정을 따른다. `main` 또는 `refs/heads/main:refs/heads/main`.
        std::string refspec {};
        bool set_upstream { false };
        bool follow_tags { false };
        bool force_with_lease { false };
    };

    // `push --progress [--set-upstream] [--follow-tags] [--force-with-lease] [-- <remote> [<refspec>]]`.
    [[nodiscard]] git_arguments make_git_push_args(const git_push_options& options = {});

    // `push --progress -- <remote> refs/tags/<tag>`.
    [[nodiscard]] git_arguments make_git_push_tag_args(std::string_view remote, std::string_view tag);

    // `push --progress --tags -- <remote>`.
    [[nodiscard]] git_arguments make_git_push_all_tags_args(std::string_view remote);

    // `push --progress --delete -- <remote> refs/tags/<tag>`.
    [[nodiscard]] git_arguments make_git_delete_remote_tag_args(std::string_view remote, std::string_view tag);

    // `push --progress --delete -- <remote> refs/heads/<branch>`.
    [[nodiscard]] git_arguments make_git_delete_remote_branch_args(std::string_view remote, std::string_view branch);

    struct git_clone_options
    {
        std::string url {};
        std::string directory {};
        // 비우면 원격의 기본 브랜치다.
        std::string branch {};
        // 0이면 전체 이력이다.
        std::uint32_t depth { 0 };
    };

    // `clone --progress [--branch <b>] [--depth <n>] -- <url> <directory>`.
    [[nodiscard]] git_arguments make_git_clone_args(const git_clone_options& options);

    // `init [--initial-branch=<b>] -- <directory>`.
    [[nodiscard]] git_arguments make_git_init_args(std::string_view directory, std::string_view initial_branch = {});

    // ------------------------------------------------------------------ 태그

    // `tag -- <name> [<target>]`. lightweight.
    [[nodiscard]] git_arguments make_git_tag_args(std::string_view name, std::string_view target = {});

    // `tag -a -F - -- <name> [<target>]`. 메시지는 stdin으로 넘긴다.
    [[nodiscard]] git_arguments make_git_annotated_tag_args(std::string_view name, std::string_view target = {});

    // `tag -d -- <name>`.
    [[nodiscard]] git_arguments make_git_delete_tag_args(std::string_view name);

    // ------------------------------------------------------------------ 병합과 rebase

    enum class git_merge_mode
    {
        // fast-forward가 되면 하고, 아니면 병합 커밋을 만든다.
        automatic,
        fast_forward_only,
        // 항상 병합 커밋을 만든다.
        no_fast_forward,
    };

    // `merge --no-edit [--ff-only|--no-ff] -- <revision>`.
    [[nodiscard]] git_arguments make_git_merge_args(std::string_view revision, git_merge_mode mode = git_merge_mode::automatic);

    // `merge --abort`.
    [[nodiscard]] git_arguments make_git_merge_abort_args();

    // `rebase [--autostash] -- <onto> [<branch>]`. branch를 주면 먼저 그 브랜치를
    // 체크아웃한 뒤 onto 위로 옮긴다.
    [[nodiscard]] git_arguments make_git_rebase_args(std::string_view onto, std::string_view branch = {}, bool autostash = false);

    // `rebase --continue|--skip|--abort`. continue는 편집기를 띄우지 않도록
    // `-c core.editor=true`를 앞에 둔다.
    [[nodiscard]] git_arguments make_git_rebase_continue_args();
    [[nodiscard]] git_arguments make_git_rebase_skip_args();
    [[nodiscard]] git_arguments make_git_rebase_abort_args();

    // `cherry-pick [-m <mainline>] -- <revisions>`. mainline은 병합 커밋을 고를 때의 부모
    // 번호(1부터)이고 0이면 주지 않는다.
    [[nodiscard]] git_arguments make_git_cherry_pick_args(std::span<const std::string> revisions, std::uint32_t mainline = 0);
    [[nodiscard]] git_arguments make_git_cherry_pick_continue_args();
    [[nodiscard]] git_arguments make_git_cherry_pick_abort_args();

    // `revert --no-edit [-m <mainline>] -- <revisions>`.
    [[nodiscard]] git_arguments make_git_revert_args(std::span<const std::string> revisions, std::uint32_t mainline = 0);
    [[nodiscard]] git_arguments make_git_revert_continue_args();
    [[nodiscard]] git_arguments make_git_revert_abort_args();

    enum class git_reset_mode
    {
        soft,
        mixed,
        hard,
    };

    // `reset --soft|--mixed|--hard <revision> --`.
    [[nodiscard]] git_arguments make_git_reset_args(git_reset_mode mode, std::string_view revision);

    // ------------------------------------------------------------------ stash

    struct git_stash_push_options
    {
        std::string message {};
        bool include_untracked { false };
        bool keep_index { false };
    };

    // `stash push [--include-untracked] [--keep-index] [-m <message>]`.
    [[nodiscard]] git_arguments make_git_stash_push_args(const git_stash_push_options& options = {});

    // `stash apply [--index] stash@{N}`.
    [[nodiscard]] git_arguments make_git_stash_apply_args(std::uint32_t index, bool restore_index = false);

    // `stash pop [--index] stash@{N}`.
    [[nodiscard]] git_arguments make_git_stash_pop_args(std::uint32_t index, bool restore_index = false);

    // `stash drop stash@{N}`.
    [[nodiscard]] git_arguments make_git_stash_drop_args(std::uint32_t index);
} // namespace cyella
