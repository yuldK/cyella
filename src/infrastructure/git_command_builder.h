#pragma once

#include <cstdint>
#include <optional>
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

    // `check-attr -z diff -- <path>`. 출력은 `<path>\0diff\0<value>\0`이다 (`parse_git_diff_driver`).
    [[nodiscard]] git_arguments make_git_check_attr_diff_args(std::string_view path);

    // `config --get <key>`. 값이 없으면 종료 코드 1이다. 키는 `diff.<driver>.textconv`처럼 호출자가 만든다.
    [[nodiscard]] git_arguments make_git_config_get_args(std::string_view key);

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

    // `branch --set-upstream-to=<remote>/<remote_branch> -- <branch>`. push 없이 upstream만 잇는다. 원격 이름과 브랜치 이름이
    // 옵션처럼 보이거나 잘못된 ref 이름이면 `std::invalid_argument`다.
    [[nodiscard]] git_arguments make_git_branch_set_upstream_args(std::string_view branch, std::string_view remote, std::string_view remote_branch);

    // `branch --unset-upstream -- <branch>`.
    [[nodiscard]] git_arguments make_git_branch_unset_upstream_args(std::string_view branch);

    // `remote add -- <name> <url>`. 이름이 잘못된 ref 이름이거나 URL이 비었거나 옵션처럼 보이거나 제어 문자를 품으면 `std::invalid_argument`다.
    [[nodiscard]] git_arguments make_git_remote_add_args(std::string_view name, std::string_view url);

    // ------------------------------------------------------------------ 커밋과 index

    struct git_commit_options
    {
        bool amend { false };
        // amend할 때 메시지를 바꾸지 않는다(`--no-edit`). 이 경우 stdin이 필요 없다.
        bool keep_message { false };
        bool allow_empty { false };
        // 서명 여부다. 값이 없으면 git 설정(`commit.gpgsign`)을 따른다. 참이면 `-S`(`user.signingkey`의 키로 서명),
        // 거짓이면 설정과 무관하게 `--no-gpg-sign`이다.
        std::optional<bool> sign {};
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
        // 원격에서 사라진 추적 브랜치를 지운다(`--prune`).
        bool prune { true };
        // 비어 있지 않으면 이것만 가져온다(`fetch --progress -- <remote> <refspec>`). `remote`가 있어야 하며 `all`·`prune`·`tags`는 무시한다.
        std::string refspec {};
    };

    // `fetch [--prune] --progress [--tags] [--all | -- <remote> [<refspec>]]`.
    [[nodiscard]] git_arguments make_git_fetch_args(const git_fetch_options& options = {});

    enum class git_pull_mode
    {
        fast_forward_only,
        rebase,
        // 갈라졌으면 병합 커밋을 만든다(`--no-rebase`). 사용자 설정 `pull.rebase`를 덮는다.
        merge,
    };

    // `pull --ff-only|--rebase|--no-rebase [-- <remote> [<branch>]]`.
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

    // `tag [--no-sign] -- <name> [<target>]`. lightweight. `tag.gpgsign`이 켜져 있으면 git이 가벼운 태그도 서명한 주석 태그로 만들려다
    // 메시지가 없어 실패하므로, 사용자가 서명을 껐을 때는 `no_sign`으로 설정을 덮는다.
    [[nodiscard]] git_arguments make_git_tag_args(std::string_view name, std::string_view target = {}, bool no_sign = false);

    // `tag -a [--no-sign] -F - -- <name> [<target>]`. 메시지는 stdin으로 넘긴다.
    [[nodiscard]] git_arguments make_git_annotated_tag_args(std::string_view name, std::string_view target = {}, bool no_sign = false);

    // `tag -s -F - -- <name> [<target>]`. 서명한 주석 태그다. 서명 키는 `user.signingkey`다. 메시지는 stdin으로 넘긴다.
    [[nodiscard]] git_arguments make_git_signed_tag_args(std::string_view name, std::string_view target = {});

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

    // ------------------------------------------------------------------ 저장소 세션

    // 체크아웃이 작업 트리의 변경을 어떻게 다룰지다.
    enum class git_switch_mode
    {
        // 겹치지 않는 변경은 가지고 가고, 덮어써질 변경이 있으면 거절한다.
        normal,
        // 추적 파일의 변경을 버린다(`--discard-changes`). 추적하지 않는 파일은 남는다.
        discard_changes,
        // 덮어써질 변경을 3-way로 합쳐 가지고 간다(`--merge`). 충돌이 남을 수 있다.
        merge,
    };

    // `switch [--discard-changes|--merge] -- <branch>`.
    [[nodiscard]] git_arguments make_git_switch_args(std::string_view branch, git_switch_mode mode);

    // `switch --detach [--discard-changes|--merge] -- <revision>`.
    [[nodiscard]] git_arguments make_git_switch_detach_args(std::string_view revision, git_switch_mode mode);

    // `switch -c <new_branch> [--track] [--discard-changes|--merge] [-- <start_point>]`.
    [[nodiscard]] git_arguments make_git_switch_create_args(std::string_view new_branch, std::string_view start_point, bool track, git_switch_mode mode);

    // `fetch -- . <source>:<target>`. 체크아웃하지 않은 로컬 브랜치를 fast-forward한다.
    // `branch -f`와 달리 git이 non-fast-forward 갱신을 거절한다. 둘 다 전체 ref 이름을 준다
    // (`refs/heads/main`, `refs/remotes/origin/main`). 대상이 체크아웃된 브랜치면 git이 거절한다.
    [[nodiscard]] git_arguments make_git_fetch_fast_forward_args(std::string_view source_ref, std::string_view target_ref);

    // `merge-base --is-ancestor <ancestor> <descendant>`. 종료 코드 0이면 조상, 1이면 아니다.
    [[nodiscard]] git_arguments make_git_is_ancestor_args(std::string_view ancestor, std::string_view descendant);

    // `rev-list --count <from>..<to> --`. from에서 닿지 않고 to에서 닿는 커밋 수다.
    [[nodiscard]] git_arguments make_git_count_commits_args(std::string_view from, std::string_view to);

    // `remote -v`. 원격마다 fetch·push 두 줄이 나온다.
    [[nodiscard]] git_arguments make_git_remote_list_args();

    // `add --all`. 추적하지 않는 파일과 삭제까지 모두 stage한다.
    [[nodiscard]] git_arguments make_git_add_all_args();

    // `checkout --ours|--theirs -- <path>`. 충돌 파일을 한쪽 판으로 덮는다. 이어서 `add`가 필요하다.
    [[nodiscard]] git_arguments make_git_checkout_conflict_side_args(std::string_view path, bool ours);

    // `merge --continue`. 메시지는 git이 준비한 MERGE_MSG를 쓴다(GIT_EDITOR=true).
    [[nodiscard]] git_arguments make_git_merge_continue_args();
    [[nodiscard]] git_arguments make_git_cherry_pick_skip_args();
    [[nodiscard]] git_arguments make_git_revert_skip_args();

    // `log -1 -z --format=<본문 포함 log 형식> <sha> --`. git_log_parser(has_body=true)로 읽는다.
    [[nodiscard]] git_arguments make_git_commit_info_args(std::string_view sha);

    // `log -1 -z --format= --name-status --find-renames [-m --first-parent] <sha> --`.
    // 상태 글자(A·M·D·R)와 경로를 NUL로 나눠 준다. 이름 변경은 `R100\0old\0new\0`이다.
    [[nodiscard]] git_arguments make_git_commit_name_status_args(std::string_view sha, bool is_merge);

    // 한 커밋에서 파일 하나의 diff다. `show --format= [-m --first-parent] ... <sha> -- <path>`.
    [[nodiscard]] git_arguments make_git_show_file_args(std::string_view sha, std::string_view path, bool is_merge, std::uint32_t context_lines = 3);

    // 두 리비전의 비교다 (그래프에서 여러 커밋을 골랐을 때). 왼쪽이 `base`, 오른쪽이 `tip`이다.
    // `diff -z --name-status --find-renames <base> <tip> --`. 해석은 `make_git_commit_name_status_args`와 같다.
    [[nodiscard]] git_arguments make_git_diff_revisions_name_status_args(std::string_view base, std::string_view tip);
    // `diff --numstat -z --find-renames <base> <tip> --`.
    [[nodiscard]] git_arguments make_git_diff_revisions_numstat_args(std::string_view base, std::string_view tip);
    // 파일 하나의 diff다. `diff ... <base> <tip> -- <path>`.
    [[nodiscard]] git_arguments make_git_diff_revisions_file_args(std::string_view base, std::string_view tip, std::string_view path, std::uint32_t context_lines = 3);
    // `rev-list --count <tip> --`. 루트 커밋부터의 범위처럼 왼쪽이 빈 트리일 때의 커밋 수다.
    [[nodiscard]] git_arguments make_git_count_reachable_args(std::string_view tip);
    // `hash-object -t tree --stdin`. 빈 입력을 주면 이 저장소 객체 형식의 빈 트리 해시다 (객체를 쓰지 않는다).
    [[nodiscard]] git_arguments make_git_empty_tree_args();

    // `symbolic-ref --quiet --short HEAD`. 현재 브랜치 이름이다. detached면 종료 코드가 1이다.
    [[nodiscard]] git_arguments make_git_current_branch_args();

    // `show-ref --verify --quiet <full_ref>`. ref가 있으면 0, 없으면 1이다.
    [[nodiscard]] git_arguments make_git_ref_exists_args(std::string_view full_ref);

    // `rev-parse --symbolic-full-name <revision>`. ref면 전체 이름, SHA면 빈 출력이다.
    [[nodiscard]] git_arguments make_git_symbolic_full_name_args(std::string_view revision);
} // namespace cyella
