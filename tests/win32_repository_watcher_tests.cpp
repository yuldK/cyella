#include "platform/win32/win32_repository_watcher.h"
#include "temporary_directory_support.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace cyella;
using namespace std::chrono_literals;

namespace {
    // 감시 스레드에서 오는 이벤트를 모아 test 스레드가 기다릴 수 있게 한다.
    class event_collector
    {
    public:
        void operator()(const repository_change_kind kind)
        {
            {
                const std::lock_guard lock { mutex_ };
                events_.push_back(kind);
            }
            condition_.notify_all();
        }

        [[nodiscard]] bool wait_for(const repository_change_kind kind, const std::chrono::milliseconds limit)
        {
            std::unique_lock lock { mutex_ };
            return condition_.wait_for(lock, limit, [&] { return std::ranges::find(events_, kind) != events_.end(); });
        }

        [[nodiscard]] std::size_t count(const repository_change_kind kind)
        {
            const std::lock_guard lock { mutex_ };
            return static_cast<std::size_t>(std::ranges::count(events_, kind));
        }

        [[nodiscard]] std::size_t total()
        {
            const std::lock_guard lock { mutex_ };
            return events_.size();
        }

        void clear()
        {
            const std::lock_guard lock { mutex_ };
            events_.clear();
        }

    private:
        std::mutex mutex_ {};
        std::condition_variable condition_ {};
        std::vector<repository_change_kind> events_ {};
    };

    constexpr auto working_tree_changed { repository_change_kind::working_tree_changed };
    constexpr auto repository_state_changed { repository_change_kind::repository_state_changed };

    // 이벤트가 없어야 하는 검사에서 기다리는 시간이다. 디바운스 창(50 ms)의 열 배가 넘는다.
    constexpr auto quiet_period { 700ms };

    repository_watch_options fast_options()
    {
        repository_watch_options options {};
        options.debounce.window = 50ms;
        options.debounce.maximum_wait = 500ms;
        return options;
    }

    // 방금 만든 디렉터리 트리의 마지막 쓰기 시각 갱신을 디스크에 확정시킨다. NTFS는 디렉터리의
    // 시각을 늦게 반영해서, 이걸 하지 않으면 감시를 시작한 뒤 첫 쓰기에 딸려 `.git\refs` 같은
    // 상위 디렉터리의 MODIFIED 알림이 한 번 뒤늦게 올라온다. 이 알림은 `refs/**`로 분류돼
    // `repository_state_changed`가 되므로 "이벤트 없음"을 검사하는 test가 부하에 따라 깨진다.
    // 기다린다고 사라지는 알림이 아니라(쓰기가 없으면 오지 않는다) 감시를 걸기 전에 미리 치른다.
    void settle_directory_timestamps(const std::filesystem::path& root)
    {
        static_cast<void>(std::filesystem::last_write_time(root));
        for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator { root })
            if (entry.is_directory())
                static_cast<void>(std::filesystem::last_write_time(entry.path()));
    }

    // 최소한의 `.git` 디렉터리가 있는 작업 트리를 만든다. 감시를 걸기 직전에 부르는 함수이므로
    // 디렉터리 시각도 확정해 둔다.
    std::filesystem::path make_repository(const std::filesystem::path& root)
    {
        const std::filesystem::path repository { root / "repo" };
        std::filesystem::create_directories(repository / ".git" / "refs" / "heads");
        std::filesystem::create_directories(repository / ".git" / "objects");
        std::filesystem::create_directories(repository / ".git" / "logs");
        test::write_file(repository / ".git" / "HEAD", "ref: refs/heads/main\n");
        test::write_file(repository / "tracked.txt", "one");
        settle_directory_timestamps(repository);
        return repository;
    }

    struct started_watcher
    {
        std::unique_ptr<repository_watcher> watcher {};
        std::vector<diagnostic> diagnostics {};
    };

    started_watcher start_watching(const std::filesystem::path& working_tree, event_collector& collector, const repository_watch_options& options = fast_options())
    {
        repository_watch_start_result result { win32::start_repository_watcher(working_tree.u8string(), [&collector](const repository_change_kind kind) { collector(kind); }, options) };
        return { std::move(result.watcher), std::move(result.diagnostics) };
    }
} // namespace

TEST_CASE("a working tree write is reported as working_tree_changed", "[integration][watcher]")
{
    test::temporary_directory directory { "watch-worktree" };
    const std::filesystem::path repository { make_repository(directory.path()) };
    event_collector collector {};
    // 기본 디바운스(300 ms)로 시작해 2초 안에 오는지 본다.
    started_watcher started { start_watching(repository, collector, repository_watch_options {}) };
    REQUIRE(started.watcher != nullptr);
    CHECK(started.diagnostics.empty());

    test::write_file(repository / "new file.txt", "hello");

    CHECK(collector.wait_for(working_tree_changed, 2s));
    std::this_thread::sleep_for(quiet_period);
    CHECK(collector.count(working_tree_changed) == 1);
    CHECK(collector.count(repository_state_changed) == 0);
}

TEST_CASE("changes in subfolders and deletions are seen", "[integration][watcher]")
{
    test::temporary_directory directory { "watch-subfolder" };
    const std::filesystem::path repository { make_repository(directory.path()) };
    event_collector collector {};
    started_watcher started { start_watching(repository, collector) };
    REQUIRE(started.watcher != nullptr);

    test::write_file(repository / "src" / "deep" / u8"한글 파일.cpp", "x");
    CHECK(collector.wait_for(working_tree_changed, 2s));

    collector.clear();
    std::filesystem::remove(repository / "tracked.txt");
    CHECK(collector.wait_for(working_tree_changed, 2s));
}

TEST_CASE("a write to .git/HEAD is reported as repository_state_changed only", "[integration][watcher]")
{
    test::temporary_directory directory { "watch-head" };
    const std::filesystem::path repository { make_repository(directory.path()) };
    event_collector collector {};
    started_watcher started { start_watching(repository, collector) };
    REQUIRE(started.watcher != nullptr);

    test::write_file(repository / ".git" / "HEAD", "ref: refs/heads/other\n");

    CHECK(collector.wait_for(repository_state_changed, 2s));
    std::this_thread::sleep_for(quiet_period);
    // `.git` 안의 변경은 작업 트리 변경으로 세지 않는다.
    CHECK(collector.count(working_tree_changed) == 0);
    CHECK(collector.count(repository_state_changed) == 1);
}

TEST_CASE("refs index and operation state files are repository state", "[integration][watcher]")
{
    test::temporary_directory directory { "watch-state" };
    const std::filesystem::path repository { make_repository(directory.path()) };
    event_collector collector {};
    started_watcher started { start_watching(repository, collector) };
    REQUIRE(started.watcher != nullptr);

    const std::vector<std::filesystem::path> files {
        repository / ".git" / "refs" / "heads" / "feature",
        repository / ".git" / "index",
        repository / ".git" / "MERGE_HEAD",
        repository / ".git" / "rebase-merge" / "git-rebase-todo",
        repository / ".git" / "packed-refs",
    };
    for (const std::filesystem::path& file : files)
    {
        collector.clear();
        test::write_file(file, "x");
        INFO(file.string());
        CHECK(collector.wait_for(repository_state_changed, 2s));
        std::this_thread::sleep_for(150ms);
    }
}

TEST_CASE("git's lock then rename pattern reports the final file once", "[integration][watcher]")
{
    test::temporary_directory directory { "watch-lock-rename" };
    const std::filesystem::path repository { make_repository(directory.path()) };
    event_collector collector {};
    started_watcher started { start_watching(repository, collector) };
    REQUIRE(started.watcher != nullptr);

    test::write_file(repository / ".git" / "index.lock", "new index");
    std::this_thread::sleep_for(quiet_period);
    CHECK(collector.total() == 0);

    std::filesystem::rename(repository / ".git" / "index.lock", repository / ".git" / "index");
    CHECK(collector.wait_for(repository_state_changed, 2s));
}

TEST_CASE("lock files and objects and logs noise produce no event", "[integration][watcher]")
{
    test::temporary_directory directory { "watch-noise" };
    const std::filesystem::path repository { make_repository(directory.path()) };
    event_collector collector {};
    started_watcher started { start_watching(repository, collector) };
    REQUIRE(started.watcher != nullptr);

    test::write_file(repository / ".git" / "index.lock", "x");
    test::write_file(repository / ".git" / "HEAD.lock", "x");
    test::write_file(repository / ".git" / "refs" / "heads" / "main.lock", "x");
    test::write_file(repository / ".git" / "objects" / "ab" / "cdef0123", "x");
    test::write_file(repository / ".git" / "logs" / "HEAD", "x");
    test::write_file(repository / ".git" / "COMMIT_EDITMSG", "x");

    std::this_thread::sleep_for(quiet_period);
    CHECK(collector.total() == 0);
}

TEST_CASE("a burst of writes is reported once", "[integration][watcher]")
{
    test::temporary_directory directory { "watch-burst" };
    const std::filesystem::path repository { make_repository(directory.path()) };
    event_collector collector {};
    repository_watch_options options {};
    options.debounce.window = 200ms;
    options.debounce.maximum_wait = 2s;
    started_watcher started { start_watching(repository, collector, options) };
    REQUIRE(started.watcher != nullptr);

    // 창(200 ms)보다 짧은 간격으로 계속 쓴다.
    for (int index = 0; index < 8; ++index)
    {
        test::write_file(repository / ("burst" + std::to_string(index) + ".txt"), "x");
        std::this_thread::sleep_for(30ms);
    }

    CHECK(collector.wait_for(working_tree_changed, 2s));
    std::this_thread::sleep_for(quiet_period);
    CHECK(collector.count(working_tree_changed) == 1);
}

TEST_CASE("a .git file with a relative gitdir is resolved", "[integration][watcher]")
{
    test::temporary_directory directory { "watch-gitfile" };
    // submodule처럼 gitdir이 작업 트리 밖에 있고 상대 경로로 가리킨다.
    const std::filesystem::path gitdir { directory.path() / "store" / "modules" / "sub" };
    std::filesystem::create_directories(gitdir / "refs" / "heads");
    test::write_file(gitdir / "HEAD", "ref: refs/heads/main\n");
    const std::filesystem::path worktree { directory.path() / "super" / "sub" };
    std::filesystem::create_directories(worktree);
    test::write_file(worktree / ".git", "gitdir: ../../store/modules/sub\n");
    settle_directory_timestamps(directory.path());

    event_collector collector {};
    started_watcher started { start_watching(worktree, collector) };
    REQUIRE(started.watcher != nullptr);
    CHECK(started.diagnostics.empty());

    test::write_file(gitdir / "HEAD", "ref: refs/heads/other\n");
    CHECK(collector.wait_for(repository_state_changed, 2s));
    std::this_thread::sleep_for(quiet_period);
    CHECK(collector.count(working_tree_changed) == 0);

    collector.clear();
    test::write_file(gitdir / "index.lock", "x");
    test::write_file(gitdir / "objects" / "ab" / "cd", "x");
    std::this_thread::sleep_for(quiet_period);
    CHECK(collector.total() == 0);

    // 작업 트리의 `.git` 파일 자체는 작업 트리 변경이 아니다. 일반 파일은 그렇다.
    test::write_file(worktree / "file.txt", "x");
    CHECK(collector.wait_for(working_tree_changed, 2s));
}

TEST_CASE("a linked worktree also watches the common directory refs", "[integration][watcher]")
{
    test::temporary_directory directory { "watch-linked" };
    const std::filesystem::path common { directory.path() / "main" / ".git" };
    const std::filesystem::path gitdir { common / "worktrees" / "wt" };
    std::filesystem::create_directories(common / "refs" / "heads");
    std::filesystem::create_directories(gitdir);
    test::write_file(common / "HEAD", "ref: refs/heads/main\n");
    test::write_file(gitdir / "HEAD", "ref: refs/heads/wt\n");
    test::write_file(gitdir / "commondir", "../..\n");
    const std::filesystem::path worktree { directory.path() / "wt" };
    std::filesystem::create_directories(worktree);
    // worktree는 gitdir을 절대 경로로 쓴다.
    test::write_file(worktree / ".git", "gitdir: " + gitdir.generic_string() + "\n");
    settle_directory_timestamps(directory.path());

    event_collector collector {};
    started_watcher started { start_watching(worktree, collector) };
    REQUIRE(started.watcher != nullptr);
    CHECK(started.diagnostics.empty());

    test::write_file(common / "refs" / "heads" / "new-branch", "abc");
    CHECK(collector.wait_for(repository_state_changed, 2s));

    collector.clear();
    // 주 worktree의 HEAD는 이 worktree와 무관하다.
    test::write_file(common / "HEAD", "ref: refs/heads/elsewhere\n");
    std::this_thread::sleep_for(quiet_period);
    CHECK(collector.total() == 0);

    test::write_file(gitdir / "HEAD", "ref: refs/heads/wt2\n");
    CHECK(collector.wait_for(repository_state_changed, 2s));
}

TEST_CASE("an unreadable .git file falls back to the working tree with a warning", "[integration][watcher]")
{
    test::temporary_directory directory { "watch-badgitfile" };
    const std::filesystem::path worktree { directory.path() / "wt" };
    std::filesystem::create_directories(worktree);
    test::write_file(worktree / ".git", "this is not a gitdir pointer");

    event_collector collector {};
    started_watcher started { start_watching(worktree, collector) };

    REQUIRE(started.watcher != nullptr);
    REQUIRE(started.diagnostics.size() == 1);
    CHECK(started.diagnostics[0].code == diagnostic_code::repository_watch_failed);
    CHECK(started.diagnostics[0].severity == diagnostic_severity::warning);

    test::write_file(worktree / "a.txt", "x");
    CHECK(collector.wait_for(working_tree_changed, 2s));
}

TEST_CASE("starting on a missing or relative path fails with a diagnostic", "[integration][watcher]")
{
    event_collector collector {};
    test::temporary_directory directory { "watch-missing" };

    started_watcher missing { start_watching(directory.path() / "does-not-exist", collector) };
    CHECK(missing.watcher == nullptr);
    REQUIRE_FALSE(missing.diagnostics.empty());
    CHECK(missing.diagnostics[0].severity == diagnostic_severity::error);
    CHECK(missing.diagnostics[0].native_error.has_value());

    repository_watch_start_result relative { win32::start_repository_watcher(u8"relative\\path", [](repository_change_kind) {}, fast_options()) };
    CHECK(relative.watcher == nullptr);
    CHECK_FALSE(relative.diagnostics.empty());
}

TEST_CASE("stop is fast idempotent and silences the callback", "[integration][watcher]")
{
    test::temporary_directory directory { "watch-stop" };
    const std::filesystem::path repository { make_repository(directory.path()) };
    event_collector collector {};

    // 한 번쯤은 느려질 수 있으니 세 번 재고 가장 빠른 값이 목표 안인지 본다.
    std::chrono::milliseconds fastest { 10s };
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        started_watcher started { start_watching(repository, collector) };
        REQUIRE(started.watcher != nullptr);

        const auto begin { std::chrono::steady_clock::now() };
        started.watcher->stop();
        const auto elapsed { std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin) };
        fastest = std::min(fastest, elapsed);
        CHECK(elapsed < 1s);

        // 여러 번 불러도 안전하다.
        started.watcher->stop();
    }
    CHECK(fastest < 100ms);

    // 멈춘 뒤의 변경은 알리지 않는다.
    started_watcher stopped { start_watching(repository, collector) };
    REQUIRE(stopped.watcher != nullptr);
    test::write_file(repository / "before-stop.txt", "x");
    stopped.watcher->stop();
    collector.clear();
    test::write_file(repository / "after-stop.txt", "x");
    std::this_thread::sleep_for(quiet_period);
    CHECK(collector.total() == 0);
}

TEST_CASE("stop works while changes are pending and from many threads", "[integration][watcher]")
{
    test::temporary_directory directory { "watch-stop-threads" };
    const std::filesystem::path repository { make_repository(directory.path()) };
    event_collector collector {};
    repository_watch_options options {};
    options.debounce.window = 5s;
    options.debounce.maximum_wait = 10s;
    started_watcher started { start_watching(repository, collector, options) };
    REQUIRE(started.watcher != nullptr);

    // 방출을 기다리는 변경이 있는 채로 멈춘다. 5초 창이 지나기를 기다리지 않는다.
    test::write_file(repository / "pending.txt", "x");
    std::this_thread::sleep_for(100ms);

    const auto begin { std::chrono::steady_clock::now() };
    std::vector<std::thread> stoppers {};
    for (int index = 0; index < 4; ++index)
        stoppers.emplace_back([&started] { started.watcher->stop(); });
    for (std::thread& stopper : stoppers)
        stopper.join();
    const auto elapsed { std::chrono::steady_clock::now() - begin };

    CHECK(elapsed < 1s);
    CHECK(collector.total() == 0);
}

TEST_CASE("stop may be requested from inside the callback", "[integration][watcher]")
{
    test::temporary_directory directory { "watch-stop-callback" };
    const std::filesystem::path repository { make_repository(directory.path()) };

    std::atomic_bool called { false };
    std::unique_ptr<repository_watcher> watcher {};
    std::mutex mutex {};
    std::condition_variable condition {};
    bool ready { false };

    repository_watch_start_result result {
        win32::start_repository_watcher(
            repository.u8string(),
            [&](repository_change_kind) {
                // 콜백 스레드에서 자기 자신을 멈춘다. 스스로를 기다리다 멈추면 안 된다.
                std::unique_lock lock { mutex };
                condition.wait(lock, [&] { return ready; });
                watcher->stop();
                called = true;
            },
            fast_options()),
    };
    REQUIRE(result.watcher != nullptr);
    {
        const std::lock_guard lock { mutex };
        watcher = std::move(result.watcher);
    }

    test::write_file(repository / "a.txt", "x");
    std::this_thread::sleep_for(300ms);
    {
        const std::lock_guard lock { mutex };
        ready = true;
    }
    condition.notify_all();

    for (int wait = 0; wait < 100 && called.load() == false; ++wait)
        std::this_thread::sleep_for(20ms);
    CHECK(called.load());

    // 콜백이 끝난 뒤에 소멸한다. 밖의 스레드에서 stop()이 스레드를 거둔다.
    watcher.reset();
}

TEST_CASE("a buffer overflow while the consumer is busy reports both kinds", "[integration][watcher]")
{
    test::temporary_directory directory { "watch-overflow" };
    const std::filesystem::path repository { make_repository(directory.path()) };

    std::mutex mutex {};
    std::condition_variable condition {};
    bool release { false };
    bool entered { false };
    event_collector collector {};

    repository_watch_start_result result {
        win32::start_repository_watcher(
            repository.u8string(),
            [&](const repository_change_kind kind) {
                std::unique_lock lock { mutex };
                if (entered == false)
                {
                    // 첫 알림에서 감시 스레드를 붙잡아 커널 버퍼가 차게 한다.
                    entered = true;
                    condition.notify_all();
                    condition.wait(lock, [&] { return release; });
                }
                lock.unlock();
                collector(kind);
            },
            fast_options()),
    };
    REQUIRE(result.watcher != nullptr);

    test::write_file(repository / "first.txt", "x");
    {
        std::unique_lock lock { mutex };
        REQUIRE(condition.wait_for(lock, 2s, [&] { return entered; }));
    }

    // 64 KiB 버퍼를 넘기도록 긴 이름의 파일을 많이 만든다.
    const std::filesystem::path flood { repository / "flood" };
    std::filesystem::create_directories(flood);
    for (int index = 0; index < 1500; ++index)
        test::write_file(flood / (std::string(120, 'f') + std::to_string(index)), "x");

    {
        const std::lock_guard lock { mutex };
        release = true;
    }
    condition.notify_all();

    // `.git` 안은 건드리지 않았으므로 상태 변경 알림은 넘침 처리에서만 나온다.
    CHECK(collector.wait_for(repository_state_changed, 3s));
    CHECK(collector.wait_for(working_tree_changed, 3s));
}
