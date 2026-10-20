#pragma once

#include "domain/interactive_rebase.h"
#include "domain/operation_status.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cyella {
    // UI가 내는 operation의 요청 값이다. 값 하나가 operation 하나이고 종류는 타입이 정한다.
    // 경로는 저장소 루트 기준 UTF-8,
    // 브랜치·태그는 짧은 이름(`main`, `origin/main`, `v1.0`)이다.

    // 체크아웃이 작업 트리 변경에 막혔을 때 고르는 정책이다. `ask`면 먼저 그냥 `switch`를
    // 시도하고, 덮어써질 변경이 있으면 `needs_input{dirty_worktree}`로 끝난다.
    enum class checkout_policy
    {
        ask,
        // `stash push -u` → `switch` → `stash pop`. pop이 충돌하면 stash를 남기고 알린다.
        stash_and_reapply,
        // `switch --discard-changes`. 추적하지 않는 파일은 남는다.
        discard,
        // `switch --merge`. 덮어써질 변경을 3-way로 합쳐 가지고 간다.
        carry,
    };

    enum class diff_view
    {
        unstaged,
        staged,
        commit,
    };

    enum class pull_strategy
    {
        ff_only,
        rebase,
        merge,
    };

    enum class reset_mode
    {
        soft,
        mixed,
        hard,
    };

    enum class conflict_side
    {
        ours,
        theirs,
    };

    // "이번만 연결"이다. 사용자가 호스트 키 지문을 확인하고 이 한 번만 믿기로 했을 때, 그 operation의
    // 모든 git 명령이 임시 known_hosts로 연결한다. 사용자의 `known_hosts`와 `core.sshCommand`는
    // 바뀌지 않는다. 네트워크를 쓰는 operation(fetch·pull·push_execute·clone·refresh_remote_tags)만
    // 받는다.
    struct temporary_ssh_trust
    {
        // `GIT_SSH_COMMAND` 값이다. `make_temporary_known_hosts_ssh_command`가 만든다. 사용자가 쓰는
        // ssh 실행 파일을 넘겨야 한다. 이 값은 그 operation에서만 `core.sshCommand`를 덮어쓴다.
        std::u8string command {};
        // 위 명령이 쓰는 임시 known_hosts 파일의 절대 경로다. operation이 어떻게 끝나든(성공, 실패,
        // 취소) 끝나면 실행기가 지운다. 비우면 지우지 않는다.
        std::u8string known_hosts_path {};
    };

    // ------------------------------------------------------------------ 읽기

    // status → refs → stash → 원격 → 진행 중 작업 → log 스트림 순서다. HEAD를 먼저 알아야
    // WIP 행을 첫 쪽 앞에 끼울 수 있다.
    struct refresh_request
    {};

    // 같은 generation의 log를 이어서 읽는다. 시작 위치는 세션이 이미 받은 커밋 수다.
    struct load_more_log_request
    {};

    struct show_commit_request
    {
        std::string sha {};
    };

    struct diff_file_request
    {
        std::string path {};
        diff_view view { diff_view::unstaged };
        // view가 commit일 때의 커밋이다.
        std::string sha {};
        // 추적하지 않는 파일이다. `/dev/null`과 비교한다.
        bool untracked { false };
        // hunk 둘레의 문맥 줄 수(`git diff -U<n>`)다. 환경 설정의 diff 문맥 줄 수를 UI가 채운다.
        std::uint32_t context_lines { 3 };

        [[nodiscard]] bool operator==(const diff_file_request&) const noexcept = default;
    };

    // `merge-base --is-ancestor ancestor descendant`. 그래프를 다 읽지 않아 UI가 조상 관계를
    // 모를 때 끌어 놓기 메뉴가 묻는다.
    struct is_ancestor_request
    {
        std::string ancestor {};
        std::string descendant {};
    };

    // `rev-list --count from..to`. 끌어 놓기 메뉴의 "커밋 N개" 문구다.
    struct count_commits_request
    {
        std::string from {};
        std::string to {};
    };

    // 원격마다 `ls-remote --tags`로 태그의 원격 존재 여부를 고친다. 비우면 모든 원격이다.
    struct refresh_remote_tags_request
    {
        std::vector<std::string> remotes {};
        std::optional<temporary_ssh_trust> ssh_command_override {};
    };

    // ------------------------------------------------------------------ 작업 영역

    struct stage_files_request
    {
        std::vector<std::string> paths {};
    };

    struct unstage_files_request
    {
        std::vector<std::string> paths {};
    };

    // 추적 파일의 작업 트리 변경을 버린다(`restore`). 추적하지 않는 파일은 `trash_files_request`가 맡는다.
    struct discard_files_request
    {
        std::vector<std::string> paths {};
    };

    // `.gitignore` 끝에 한 줄을 더한다. 값은 gitignore 패턴 그대로다.
    struct ignore_path_request
    {
        std::string pattern {};
    };

    // 메시지는 stdin(`commit -F -`)으로 넘긴다.
    struct commit_request
    {
        std::string message {};
        bool amend { false };
        // 커밋 전에 `add --all`을 한다.
        bool stage_all { false };
        // 서명 여부다. 값이 없으면 git 설정(`commit.gpgsign`)을 따르고, 참이면 `-S`, 거짓이면 `--no-gpg-sign`이다.
        // 서명 키는 항상 `user.signingkey`다. 참이면 passphrase를 기다릴 수 있어 시간 제한 없이 돌고 취소만 된다.
        std::optional<bool> sign {};
    };

    // hunk·줄 단위 stage에 쓴다. C++이 만든 patch를 stdin으로 `git apply`에 넘긴다.
    struct apply_patch_request
    {
        std::string patch {};
        // index에 적용한다(`--cached`). stage·unstage가 켜고, 작업 트리 discard는 끈다.
        bool cached { true };
        // 반대로 적용한다(`-R`). unstage·discard가 켠다.
        bool reverse { false };
        // 줄 단위로 만든 patch는 hunk 헤더의 줄 수가 맞지 않을 수 있다(`--recount`).
        bool recount { true };
        // 문맥 줄이 없는 patch(`-U0`로 읽은 diff)를 받는다(`--unidiff-zero`).
        bool unidiff_zero { false };
    };

    // ------------------------------------------------------------------ 브랜치

    struct checkout_branch_request
    {
        std::string name {};
        checkout_policy policy { checkout_policy::ask };
    };

    // `origin/release/1.0`처럼 원격 이름을 붙인 브랜치다. 같은 이름의 로컬 브랜치가 있으면
    // 그리로 옮기고, 없으면 추적 브랜치를 만든다.
    struct checkout_remote_branch_request
    {
        std::string remote_branch {};
        checkout_policy policy { checkout_policy::ask };
    };

    struct checkout_detached_request
    {
        std::string revision {};
        checkout_policy policy { checkout_policy::ask };
    };

    struct create_branch_request
    {
        std::string name {};
        // 비우면 HEAD다.
        std::string at {};
        bool checkout { false };
    };

    struct rename_branch_request
    {
        std::string old_name {};
        std::string new_name {};
    };

    struct delete_branch_request
    {
        std::string name {};
        bool force { false };
    };

    // ------------------------------------------------------------------ 원격

    struct fetch_request
    {
        // 비우면 기본 원격이다. all이면 무시한다.
        std::string remote {};
        bool all { false };
        bool prune { true };
        std::optional<temporary_ssh_trust> ssh_command_override {};
        // 이 refspec 하나만 가져온다 (`pull/7/head:pr/7`). 비우면 기본 refspec이다. 있으면 `remote`가 있어야 하고 `all`·`prune`은 무시한다
        // (prune은 명시한 refspec 밖의 추적 브랜치를 건드리지 않게 끈다).
        std::string refspec {};
    };

    struct pull_request
    {
        pull_strategy strategy { pull_strategy::ff_only };
        std::optional<temporary_ssh_trust> ssh_command_override {};
    };

    struct push_execute_request
    {
        std::string remote {};
        // 비우면 현재 브랜치다.
        std::string branch {};
        // 비우면 branch와 같은 이름이다.
        std::string remote_branch {};
        bool set_upstream { false };
        bool follow_tags { false };
        bool force_with_lease { false };
        std::optional<temporary_ssh_trust> ssh_command_override {};
    };

    // ------------------------------------------------------------------ 이력

    // into가 현재 브랜치가 아니면 먼저 into로 체크아웃한다(명령줄 둘).
    struct merge_request
    {
        std::string into {};
        std::string from {};
    };

    struct rebase_request
    {
        std::string branch {};
        std::string onto {};
        bool autostash { false };
    };

    // target 브랜치를 to로 fast-forward한다. target이 현재 브랜치면 `merge --ff-only`, 아니면
    // `fetch . to:target`이다. 둘 다 non-fast-forward를 git이 거절한다.
    struct fast_forward_request
    {
        std::string target {};
        std::string to {};
    };

    struct reset_request
    {
        reset_mode mode { reset_mode::mixed };
        std::string to {};
    };

    struct cherry_pick_request
    {
        std::string sha {};
    };

    struct revert_request
    {
        std::string sha {};
    };

    // ------------------------------------------------------------------ stash

    struct stash_push_request
    {
        std::string message {};
        bool include_untracked { true };
    };

    struct stash_apply_request
    {
        std::uint32_t index { 0 };
    };

    struct stash_pop_request
    {
        std::uint32_t index { 0 };
    };

    struct stash_drop_request
    {
        std::uint32_t index { 0 };
    };

    // ------------------------------------------------------------------ 태그

    struct create_tag_request
    {
        std::string name {};
        // 비우면 HEAD다.
        std::string sha {};
        // UI가 종류를 먼저 고른다. 메시지는 annotated일 때만 쓴다.
        bool annotated { false };
        std::string message {};
        // 서명 여부다. 참이면 `tag -s`(서명한 주석 태그, `annotated`를 무시한다), 거짓이면 설정 `tag.gpgsign`을 덮어 서명하지 않는다
        // (`--no-sign`). 값이 없으면 설정을 따른다.
        std::optional<bool> sign {};
    };

    struct push_tag_request
    {
        std::string remote {};
        std::string name {};
    };

    struct push_tags_request
    {
        std::string remote {};
    };

    struct delete_tag_request
    {
        std::string name {};
    };

    struct delete_remote_tag_request
    {
        std::string remote {};
        std::string name {};
    };

    // ------------------------------------------------------------------ 충돌

    // rebase·merge·cherry-pick·revert의 continue·skip·abort다. 종류는 kind가 정한다.
    struct sequence_control_request
    {
        operation_kind kind { operation_kind::rebase_abort };
    };

    // `checkout --ours|--theirs -- path` 뒤 `add -- path`.
    struct resolve_file_request
    {
        std::string path {};
        conflict_side side { conflict_side::ours };
    };

    // 사용자가 직접 고친 파일을 해결됨으로 표시한다(`add`).
    struct mark_resolved_request
    {
        std::vector<std::string> paths {};
    };

    // ------------------------------------------------------------------ 복구

    // 남은 `index.lock`을 지운다. 이 저장소에서 cyella가 띄운 git이 돌고 있으면 거절한다.
    struct remove_stale_index_lock_request
    {};

    // ------------------------------------------------------------------ 저장소 준비

    struct clone_request
    {
        std::string url {};
        std::u8string target_path {};
        std::optional<temporary_ssh_trust> ssh_command_override {};
    };

    struct init_request
    {
        std::u8string target_path {};
        std::string initial_branch {};
    };

    // 경로가 저장소 루트인지, 저장소 안쪽인지, 일반 폴더인지, 없는지 본다.
    struct inspect_path_request
    {
        std::u8string path {};
    };

    // ------------------------------------------------------------------ SSH 도구 (환경 설정)

    // `ssh-keygen -t ed25519`로 새 키를 만든다. passphrase는 명령줄이 아니라 askpass로 묻는다
    // (`ssh_new_key_passphrase`와 확인 질문이 이 operation의 `needs_input`으로 온다). 대상은 개인 키의 절대 경로다.
    struct ssh_keygen_request
    {
        std::u8string key_path {};
        std::u8string comment {};
    };

    // `ssh -T <user>@<host>`로 인증을 시험한다. 결과 분류는 `ssh_connection_tested` 사건으로 온다.
    struct ssh_connection_test_request
    {
        std::u8string host {};
        std::u8string user_name { u8"git" };
        // 0이면 ssh 기본값이다.
        std::uint16_t port { 0 };
    };

    // ------------------------------------------------------------------ submodule·worktree·Git LFS

    // `submodule update [--init] [--recursive] -- <paths>`. 기록된 커밋으로 체크아웃한다. paths가 비면 모든 submodule이다.
    struct submodule_update_request
    {
        bool init { true };
        bool recursive { true };
        // 저장소 루트 기준 경로다.
        std::vector<std::string> paths {};
    };

    // `submodule sync`. `.gitmodules`의 url을 submodule 설정에 옮긴다.
    struct submodule_sync_request
    {
        bool recursive { true };
        std::vector<std::string> paths {};
    };

    // `worktree add`. 새 폴더에 작업 트리를 만든다. new_branch가 있으면 그 이름의 새 브랜치를 start_point에서 만들고,
    // 없고 detach면 start_point(비면 HEAD)를 detached로, 둘 다 아니면 branch를 체크아웃한다.
    struct worktree_add_request
    {
        // 새 폴더의 절대 경로다.
        std::u8string path {};
        std::string branch {};
        std::string new_branch {};
        std::string start_point {};
        bool detach { false };
    };

    // `worktree remove`. 변경이 있는 작업 트리는 force여야 지운다. 잠긴 작업 트리도 force여야 한다. 주 작업 트리는 거절한다.
    struct worktree_remove_request
    {
        std::u8string path {};
        bool force { false };
    };

    struct worktree_prune_request
    {};

    // `lfs pull`. 객체를 받고 작업 트리를 실제 내용으로 채운다. 저장소에 `git lfs install --local`이 없으면 먼저 한다.
    struct lfs_pull_request
    {};

    // `lfs fetch [--all]`. 객체만 받는다.
    struct lfs_fetch_request
    {
        bool all { false };
    };

    // `lfs track -- <pattern>`. `.gitattributes`를 git-lfs가 고친다. stage하지 않는다.
    struct lfs_track_request
    {
        std::string pattern {};
    };

    // ------------------------------------------------------------------ 인터랙티브 rebase

    // 편집기를 채울 범위를 읽는다 (읽기 lane). branch를 비우면 HEAD다. 결과는 `rebase_plan_loaded` 사건이고
    // `git_workspace::rebase_plan_of`로 읽는다. 커밋은 git이 todo에 넣을 것과 같다 (`log --reverse --topo-order
    // --max-parents=1 --right-only --cherry-pick onto...branch`). 병합 커밋 수와 원격에 이미 있는 커밋도 함께 준다.
    struct load_rebase_plan_request
    {
        std::string onto {};
        std::string branch {};
    };

    // `rebase -i`를 앱이 고친 목록으로 실행한다. git의 순서 편집기와 메시지 편집기 자리에 cyella의 편집기 도우미가
    // 들어간다 (ADR-005). 충돌하면 `needs_input{conflict}`로 끝나고, 남은 reword·squash 메시지는
    // `rebase_continue`·`rebase_skip`에서도 적용된다.
    struct interactive_rebase_request
    {
        std::string onto {};
        // 비우면 현재 HEAD다.
        std::string branch {};
        // 범위의 커밋 전부가 한 번씩 들어 있어야 한다. 지울 커밋은 drop으로 둔다.
        std::vector<rebase_todo_entry> todo {};
        bool autostash { false };
        // 범위에 병합 커밋이 있어도 실행한다. 병합 커밋은 빠지고 이력이 한 줄로 펴진다. 끄면 `refused`로 끝난다.
        bool allow_flatten_merges { false };
    };

    // ------------------------------------------------------------------ 가벼운 새로 고침, upstream, 휴지통

    // 작업 트리 변경만 다시 읽는다(status와 줄 수). 그래프·ref·stash는 건드리지 않는다. 파일 감시의
    // `working_tree_changed`가 낸다. WIP 행이 생기거나 사라지면 세션이 전체 refresh를 청한다.
    struct refresh_status_request
    {};

    // `branch --set-upstream-to=<remote>/<remote_branch> -- <branch>`. 원격 추적 브랜치(`refs/remotes/<remote>/<remote_branch>`)가
    // 이미 있어야 한다. push 없이 upstream만 잇는다.
    struct set_upstream_request
    {
        std::string branch {};
        std::string remote {};
        std::string remote_branch {};
    };

    // `branch --unset-upstream -- <branch>`.
    struct unset_upstream_request
    {
        std::string branch {};
    };

    // 추적하지 않는 파일을 휴지통으로 보낸다(되돌릴 수 있다). 추적 파일은 `discard_files_request`(`restore`)가 맡는다.
    // 경로는 저장소 루트 기준이고 저장소 밖을 가리키면 거절한다.
    struct trash_files_request
    {
        std::vector<std::string> paths {};
    };

    // ------------------------------------------------------------------ 커밋 서명

    // 커밋들의 서명을 검증한다 (읽기 lane). `log --no-walk --stdin`이 `%G?`·`%GS`·`%GK`를 돌려주고 결과는 `signatures_verified` 사건이다.
    // 서명된 커밋마다 gpg·ssh-keygen이 한 번씩 돌아 느리므로 그래프 로그 스트림에 넣지 않고 화면이 필요한 커밋만 요청한다.
    struct verify_signatures_request
    {
        // 전체 SHA다.
        std::vector<std::string> shas {};
    };

    using git_operation = std::variant<refresh_request, load_more_log_request, show_commit_request, diff_file_request, is_ancestor_request, count_commits_request, refresh_remote_tags_request,
        stage_files_request, unstage_files_request, discard_files_request, ignore_path_request, commit_request, apply_patch_request, checkout_branch_request, checkout_remote_branch_request,
        checkout_detached_request, create_branch_request, rename_branch_request, delete_branch_request, fetch_request, pull_request, push_execute_request, merge_request, rebase_request,
        fast_forward_request, reset_request, cherry_pick_request, revert_request, stash_push_request, stash_apply_request, stash_pop_request, stash_drop_request, create_tag_request, push_tag_request,
        push_tags_request, delete_tag_request, delete_remote_tag_request, sequence_control_request, resolve_file_request, mark_resolved_request, remove_stale_index_lock_request, clone_request,
        init_request, inspect_path_request, ssh_keygen_request, ssh_connection_test_request, submodule_update_request, submodule_sync_request, worktree_add_request, worktree_remove_request,
        worktree_prune_request, lfs_pull_request, lfs_fetch_request, lfs_track_request, load_rebase_plan_request, interactive_rebase_request, refresh_status_request, set_upstream_request,
        unset_upstream_request, trash_files_request, verify_signatures_request>;

    // 실행 lane이다.
    enum class operation_lane
    {
        // 읽기 lane. `--no-optional-locks`로 잠금을 잡지 않고 병렬로 돈다.
        read,
        // 저장소의 직렬 쓰기 lane. index·ref·작업 트리를 바꾼다(fetch 포함).
        write,
        // 저장소가 아직 없는 작업(clone·init). 대상 경로마다 직렬이다.
        setup,
    };

    [[nodiscard]] operation_kind operation_kind_of(const git_operation& operation) noexcept;
    [[nodiscard]] operation_lane operation_lane_of(operation_kind kind) noexcept;
    // clone·init·inspect_path처럼 저장소 id 없이 대상 경로로 묶이는 작업이다.
    [[nodiscard]] bool is_setup_operation(operation_kind kind) noexcept;
    // operation이 "이번만 연결"을 들고 있으면 그 값이다. 받지 않는 operation은 null이다.
    [[nodiscard]] const temporary_ssh_trust* ssh_override_of(const git_operation& operation) noexcept;
} // namespace cyella
