#include "application/watch_refresh_policy.h"

#include <catch2/catch_test_macros.hpp>

using namespace cyella;
using namespace std::chrono_literals;

namespace {
    constexpr repository_activity idle {};
    constexpr repository_activity writing { .write_pending = true, .refresh_pending = false };
    constexpr repository_activity refreshing { .write_pending = false, .refresh_pending = true };
    constexpr repository_activity writing_and_refreshing { .write_pending = true, .refresh_pending = true };

    const std::string repo { "c:/work/repo" };
    const std::string other { "c:/work/other" };
    const watch_refresh_policy::clock::time_point start { watch_refresh_policy::clock::time_point {} + 1h };

    // optional을 Catch가 풀어 쓰지 않도록 글로 바꿔 견준다.
    std::string scope_text(const std::optional<refresh_scope>& scope)
    {
        if (scope.has_value() == false)
            return "none";
        return *scope == refresh_scope::full ? "full" : "status";
    }
} // namespace

TEST_CASE("a change on an idle repository refreshes at once with the requested scope", "[watcher][policy]")
{
    watch_refresh_policy policy {};
    CHECK(scope_text(policy.on_change(repo, refresh_scope::status, idle, start)) == "status");
    CHECK(scope_text(policy.on_change(repo, refresh_scope::full, idle, start)) == "full");
    CHECK(scope_text(policy.pending(repo)) == "none");
}

TEST_CASE("changes while a write runs or is queued are dropped because the post-write refresh covers them", "[watcher][policy]")
{
    watch_refresh_policy policy {};
    CHECK(scope_text(policy.on_change(repo, refresh_scope::full, writing, start)) == "none");
    CHECK(scope_text(policy.on_change(repo, refresh_scope::status, writing_and_refreshing, start)) == "none");
    // 보류도 남기지 않는다. 쓰기가 끝나고 새로 고침이 끝났을 때 다시 내지 않는다.
    CHECK(scope_text(policy.pending(repo)) == "none");
    CHECK(scope_text(policy.on_activity(repo, idle)) == "none");
}

TEST_CASE("changes while a refresh runs leave one pending refresh that fires when it ends", "[watcher][policy]")
{
    watch_refresh_policy policy {};
    CHECK(scope_text(policy.on_change(repo, refresh_scope::status, refreshing, start)) == "none");
    CHECK(scope_text(policy.pending(repo)) == "status");
    // 여러 번 와도 보류는 하나다. 넓은 쪽이 이긴다.
    CHECK(scope_text(policy.on_change(repo, refresh_scope::status, refreshing, start)) == "none");
    CHECK(scope_text(policy.on_change(repo, refresh_scope::full, refreshing, start)) == "none");
    CHECK(scope_text(policy.on_change(repo, refresh_scope::status, refreshing, start)) == "none");
    CHECK(scope_text(policy.pending(repo)) == "full");

    // 아직 도는 동안에는 내지 않는다.
    CHECK(scope_text(policy.on_activity(repo, refreshing)) == "none");
    CHECK(scope_text(policy.pending(repo)) == "full");
    // 끝나면 한 번만 낸다.
    CHECK(scope_text(policy.on_activity(repo, idle)) == "full");
    CHECK(scope_text(policy.pending(repo)) == "none");
    CHECK(scope_text(policy.on_activity(repo, idle)) == "none");
}

TEST_CASE("a pending refresh is dropped when a write arrives before it could run", "[watcher][policy]")
{
    watch_refresh_policy policy {};
    CHECK(scope_text(policy.on_change(repo, refresh_scope::full, refreshing, start)) == "none");
    // 새로 고침이 끝나기 전에 사용자가 쓰기를 냈다. 그 쓰기의 후속 refresh가 덮는다.
    CHECK(scope_text(policy.on_activity(repo, writing_and_refreshing)) == "none");
    CHECK(scope_text(policy.pending(repo)) == "none");
    CHECK(scope_text(policy.on_activity(repo, idle)) == "none");
}

TEST_CASE("the post-write refresh of cyella's own change leaves at most one extra refresh", "[watcher][policy]")
{
    // 쓰기가 끝나고 후속 refresh가 도는 중에 감시가 그 쓰기의 변경을 뒤늦게 알린다 (디바운스 300 ms).
    // 외부 변경과 가를 수 없으므로 후속 refresh가 끝난 뒤 한 번 더 읽는다. 둘 이상 쌓이지 않는다.
    watch_refresh_policy policy {};
    CHECK(scope_text(policy.on_change(repo, refresh_scope::full, refreshing, start)) == "none");
    CHECK(scope_text(policy.on_change(repo, refresh_scope::status, refreshing, start)) == "none");
    CHECK(scope_text(policy.on_activity(repo, idle)) == "full");
    CHECK(scope_text(policy.on_activity(repo, idle)) == "none");
}

TEST_CASE("repositories do not share pending refreshes", "[watcher][policy]")
{
    watch_refresh_policy policy {};
    CHECK(scope_text(policy.on_change(repo, refresh_scope::full, refreshing, start)) == "none");
    CHECK(scope_text(policy.on_change(other, refresh_scope::status, idle, start)) == "status");
    CHECK(scope_text(policy.on_activity(other, idle)) == "none");
    CHECK(scope_text(policy.on_activity(repo, idle)) == "full");
    policy.forget(repo);
    CHECK(scope_text(policy.pending(repo)) == "none");
}

TEST_CASE("focus refreshes read the status, or everything when the watch is lost, and are debounced", "[watcher][policy]")
{
    watch_refresh_policy policy {};
    CHECK(scope_text(policy.on_focus(repo, idle, start)) == "status");
    // 곧바로 다시 초점을 받아도 무시한다.
    CHECK(scope_text(policy.on_focus(repo, idle, start + 300ms)) == "none");
    CHECK(scope_text(policy.on_focus(repo, idle, start + 999ms)) == "none");
    CHECK(scope_text(policy.on_focus(repo, idle, start + 1s)) == "status");

    policy.mark_watch_lost(repo);
    CHECK(policy.watch_lost(repo));
    CHECK_FALSE(policy.watch_lost(other));
    CHECK(scope_text(policy.on_focus(repo, idle, start + 5s)) == "full");
    policy.mark_watch_started(repo);
    CHECK_FALSE(policy.watch_lost(repo));
    CHECK(scope_text(policy.on_focus(repo, idle, start + 10s)) == "status");
}

TEST_CASE("focus follows the same coalescing rules as watcher changes", "[watcher][policy]")
{
    watch_refresh_policy policy {};
    CHECK(scope_text(policy.on_focus(repo, writing, start)) == "none");
    CHECK(scope_text(policy.on_focus(repo, refreshing, start + 2s)) == "none");
    CHECK(scope_text(policy.pending(repo)) == "status");
    CHECK(scope_text(policy.on_activity(repo, idle)) == "status");
}
TEST_CASE("a status change right after a full refresh in the same watcher batch is covered by that refresh", "[watcher][policy]")
{
    // 감시기는 `.git` 변경(repository_state)과 작업 트리 변경(working_tree)을 연달아 알린다. 커밋 하나에 새로 고침이 둘 돌면 안 된다.
    watch_refresh_policy policy {};
    CHECK(scope_text(policy.on_change(repo, refresh_scope::full, idle, start)) == "full");
    CHECK(scope_text(policy.on_change(repo, refresh_scope::status, refreshing, start + 5ms)) == "none");
    CHECK(scope_text(policy.pending(repo)) == "none");
    CHECK(scope_text(policy.on_activity(repo, idle)) == "none");

    // 한참 뒤의 변경은 같은 묶음이 아니다. 보류해 두었다가 새로 고침이 끝나면 한 번 낸다.
    CHECK(scope_text(policy.on_change(repo, refresh_scope::status, refreshing, start + 400ms)) == "none");
    CHECK(scope_text(policy.pending(repo)) == "status");
    CHECK(scope_text(policy.on_activity(repo, idle)) == "status");

    // 전체 범위의 변경은 같은 묶음이어도 덮이지 않는다 (도는 새로 고침보다 늦은 상태 변경일 수 있다).
    CHECK(scope_text(policy.on_change(repo, refresh_scope::full, idle, start + 1s)) == "full");
    CHECK(scope_text(policy.on_change(repo, refresh_scope::full, refreshing, start + 1s + 5ms)) == "none");
    CHECK(scope_text(policy.pending(repo)) == "full");
}
