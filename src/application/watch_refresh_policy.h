#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>

namespace cyella {
    // 파일 감시와 창 초점이 청하는 새로 고침을 언제, 어떤 범위로 낼지 정하는 규칙이다. 순수 상태 기계라 시계와 operation 표를
    // 직접 보지 않고 값으로 받는다 (test가 타이밍 없이 돌린다). logic thread 하나가 쓴다.
    //
    // 규칙은 네 가지다.
    //  1. **쓰기가 돌거나 줄 서 있으면 건너뛴다.** 쓰기가 끝나면 workspace가 스스로 refresh를 내므로 그것이 이 변경을 덮는다.
    //  2. **새로 고침이 이미 돌고 있으면 새로 내지 않고 보류 표시 하나만 남긴다.** 저장소마다 보류는 최대 하나이고, 둘 이상 모이면
    //     더 넓은 범위(`full`)가 이긴다. 도는 새로 고침이 끝난 뒤 한 번 낸다. 도는 새로 고침은 변경이 일어나기 전에 읽기 시작했을 수
    //     있어 이 변경을 못 보았을 수 있으므로, 버리지 않고 한 번 더 읽는다.
    //  3. **보류 중에 쓰기가 오면 보류를 버린다.** 그 쓰기의 후속 refresh가 덮는다.
    //  4. **창 초점은 `focus_debounce` 안에 다시 오면 무시한다.** alt-tab으로 오락가락해도 한 번만 읽는다.
    //  5. **전체 새로 고침을 낸 직후(`same_batch_window`) 같은 저장소에서 온 상태 범위의 변경은 그 새로 고침이 덮는다.** 감시기는 `.git` 변경과
    //     작업 트리 변경을 한 번에 연달아 알리므로(`repository_state_changed` 다음 `working_tree_changed`), 이것을 막지 않으면 커밋 한 번에 새로 고침이 두 번 돈다.
    //
    // 쓰기 직후 감시가 뒤늦게 알리는 자기 자신의 변경(디바운스가 300 ms 뒤에 알린다)은 이미 후속 refresh가 끝난 뒤일 수 있다.
    // 그때는 한 번 더 읽게 된다. 외부 변경을 놓치는 것보다 한 번 더 읽는 쪽을 골랐다.
    enum class refresh_scope
    {
        // 작업 트리 변경만 읽는다(`refresh_status`).
        status,
        // 그래프·ref·stash까지 모두 읽는다(`refresh`).
        full,
    };

    // 저장소 하나에서 지금 도는 일이다. workspace가 operation 표에서 답한다.
    struct repository_activity
    {
        // 끝나지 않은 쓰기(실행 중이거나 줄 서 있음)가 있다.
        bool write_pending { false };
        // 끝나지 않은 `refresh`·`refresh_status`가 있다.
        bool refresh_pending { false };
    };

    struct watch_refresh_options
    {
        // 창 초점으로 청한 새로 고침 사이의 최소 간격이다.
        std::chrono::milliseconds focus_debounce { 1000 };
        // 감시기가 한 번에 연달아 알리는 변경으로 볼 간격이다.
        std::chrono::milliseconds same_batch_window { 150 };
    };

    class watch_refresh_policy
    {
    public:
        using clock = std::chrono::steady_clock;

        explicit watch_refresh_policy(watch_refresh_options options = {}) noexcept;

        // 감시가 변경을 알렸다. 지금 낼 범위를 돌려준다. 값이 없으면 낼 것이 없다(건너뛰었거나 보류했다).
        [[nodiscard]] std::optional<refresh_scope> on_change(const std::string& repository, refresh_scope wanted, repository_activity activity, clock::time_point now);

        // 창이 초점을 되찾았다. 감시가 끊긴 저장소(`mark_watch_lost`)는 놓친 변경이 있을 수 있어 `full`, 아니면 `status`를 청한다.
        // 그 밖의 규칙은 `on_change`와 같다.
        [[nodiscard]] std::optional<refresh_scope> on_focus(const std::string& repository, repository_activity activity, clock::time_point now);

        // operation이 끝나는 등 활동이 바뀐 뒤에 부른다. 보류해 둔 새로 고침을 낼 수 있으면 그 범위를 돌려준다.
        [[nodiscard]] std::optional<refresh_scope> on_activity(const std::string& repository, repository_activity activity);

        // 감시가 끊겼거나 시작하지 못했다. 이 저장소는 창 초점에서 전체를 다시 읽는다.
        void mark_watch_lost(const std::string& repository);
        [[nodiscard]] bool watch_lost(const std::string& repository) const;
        // 새 감시를 시작했다. 끊김 표시를 지운다.
        void mark_watch_started(const std::string& repository);

        // 저장소를 닫았다.
        void forget(const std::string& repository);

        // 보류 중인 범위다 (test와 진단용).
        [[nodiscard]] std::optional<refresh_scope> pending(const std::string& repository) const;

    private:
        struct entry
        {
            std::optional<refresh_scope> pending {};
            bool lost { false };
            // 마지막으로 전체 새로 고침을 내라고 답한 시각이다.
            std::optional<clock::time_point> last_full {};
        };

        watch_refresh_options options_ {};
        std::map<std::string, entry> entries_ {};
        std::optional<clock::time_point> last_focus_ {};
    };

    [[nodiscard]] constexpr refresh_scope wider_scope(const refresh_scope left, const refresh_scope right) noexcept
    {
        return left == refresh_scope::full || right == refresh_scope::full ? refresh_scope::full : refresh_scope::status;
    }
} // namespace cyella
