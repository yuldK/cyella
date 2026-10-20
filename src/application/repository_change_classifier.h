#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace cyella {
    // 감시가 변경 하나를 어떻게 다루는지의 분류다.
    enum class change_classification
    {
        // 아무도 알 필요가 없는 변경이다 (잠금 파일, objects, logs 등).
        ignored,
        // 작업 트리 파일이 바뀌었다. 소비자는 `status`를 다시 읽는다.
        working_tree,
        // HEAD·index·ref·진행 중 작업 상태가 바뀌었다. 소비자는 그래프와 ref 목록도 읽는다.
        repository_state,
    };

    // 이벤트가 어느 감시 뿌리에서 왔는지다. 뿌리마다 상대 경로의 기준이 다르다.
    enum class watched_root_kind
    {
        // 작업 트리다. 안의 `.git/` 변경은 git 디렉터리 규칙으로 다시 분류한다.
        working_tree,
        // 작업 트리 밖에 있는 gitdir이다 (`.git` 파일이 가리키는 곳: worktree, submodule).
        git_directory,
        // 연결된 worktree의 공용 디렉터리(`commondir`)다. ref는 여기에 산다.
        common_directory,
    };

    // 변경 하나를 분류한다. `relative_path`는 뿌리에 대한 상대 경로이고 구분자는 `\`와 `/`
    // 모두 받는다. 순수 함수다.
    //
    // git 디렉터리 안의 규칙은 아래와 같다. 목록에 없는 항목(`hooks`, `MERGE_MSG`
    // 등)은 소비자가 다시 읽을 이유가 없어 무시한다.
    //
    // - 마지막 구성 요소가 `.lock`으로 끝나면 무시한다. git은 `index.lock`에 쓰고 `index`로
    //   바꾸므로 실제 변경은 이름이 바뀐 `index` 이벤트로 온다. 잠금은 다른 규칙보다 먼저 본다.
    // - `objects/**`, `logs/**`는 무시한다. fetch·gc·commit마다 파일이 대량으로 생긴다.
    // - `config`, `HEAD`, `index`, `packed-refs`, `FETCH_HEAD`, `ORIG_HEAD`, `MERGE_HEAD`,
    //   `CHERRY_PICK_HEAD`, `REVERT_HEAD`, `refs/**`, `rebase-merge/**`, `rebase-apply/**`는
    //   `repository_state`다.
    //
    // 작업 트리 뿌리에서는 첫 구성 요소가 `.git`이면 나머지를 위 규칙으로 보내고, 그 밖의
    // 구성 요소에 `.git`이 있으면(중첩 저장소·submodule 내부) 무시한다. `.git` 자체는 무시한다.
    // 공용 디렉터리에서는 `refs/**`와 `packed-refs`만 본다. HEAD·index는 그 worktree의 것이
    // 아니기 때문이다. 이름은 ASCII 대소문자를 구분하지 않는다 (NTFS).
    [[nodiscard]] change_classification classify_repository_change(watched_root_kind root, std::u8string_view relative_path) noexcept;

    struct debounce_options
    {
        // 마지막 변경 뒤 이만큼 조용하면 알린다.
        std::chrono::milliseconds window { 300 };
        // 변경이 끊기지 않아도 처음 변경 뒤 이 시간 안에는 알린다. 빌드 중 산출물처럼 계속 쓰는
        // 폴더를 감시할 때 알림이 영영 밀리는 것을 막는다. `window`보다 작으면 `window`로 본다.
        std::chrono::milliseconds maximum_wait { 2000 };
    };

    // 모인 변경이다. 한 종류당 한 번만 알린다.
    struct pending_changes
    {
        bool working_tree { false };
        bool repository_state { false };

        [[nodiscard]] bool operator==(const pending_changes&) const noexcept = default;

        [[nodiscard]] bool any() const noexcept
        {
            return working_tree || repository_state;
        }
    };

    // 변경을 모아 한 번에 내보내는 순수 상태 기계다. 시계를 직접 읽지 않고 시각을 받으므로
    // 타이밍 없이 test할 수 있다. 스레드 안전하지 않다 (감시 스레드 하나가 쓴다).
    class change_debouncer
    {
    public:
        using clock = std::chrono::steady_clock;

        explicit change_debouncer(const debounce_options& options = {}) noexcept;

        // `ignored`는 아무것도 하지 않는다.
        void note(change_classification classification, clock::time_point now) noexcept;
        // 이벤트를 놓쳤을 수 있을 때(버퍼 넘침, 감시 끊김)다. 둘 다 바뀐 것으로 본다.
        void note_overflow(clock::time_point now) noexcept;

        // 가장 이른 방출 시각이다. 모인 것이 없으면 값이 없다. 감시 스레드가 이 시각까지만 잔다.
        [[nodiscard]] std::optional<clock::time_point> due_time() const noexcept;
        // `now`가 방출 시각이면 모인 것을 꺼내고 비운다. 아직이면 빈 값이다.
        [[nodiscard]] pending_changes take_ready(clock::time_point now) noexcept;

    private:
        debounce_options options_ {};
        pending_changes pending_ {};
        clock::time_point first_change_ {};
        clock::time_point last_change_ {};
    };

    // `.git` 파일의 내용(`gitdir: <path>`)에서 gitdir을 뽑는다. 상대 경로는 `.git` 파일이 있는
    // 폴더 기준으로 이어 붙인다 (submodule은 상대, worktree는 절대로 쓴다). 형식이 아니면 값이
    // 없다. 순수 문자열 처리이며 파일 시스템을 조회하지 않는다.
    [[nodiscard]] std::optional<std::u8string> resolve_gitdir_pointer(std::u8string_view dot_git_content, std::u8string_view dot_git_directory);
    // gitdir 안의 `commondir` 파일 내용을 gitdir 기준으로 풀어 공용 디렉터리를 얻는다.
    [[nodiscard]] std::optional<std::u8string> resolve_commondir_pointer(std::u8string_view commondir_content, std::u8string_view git_directory);
} // namespace cyella
