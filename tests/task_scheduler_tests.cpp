#include "application/task_scheduler.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
    using namespace cyella;
    using namespace std::chrono_literals;

    // 여러 스레드가 모이는 지점이다. 모두 도착해야 통과하므로, 직렬로 실행되면 제한 시간 안에
    // 모이지 못해 실패로 드러난다.
    class rendezvous
    {
    public:
        explicit rendezvous(const std::size_t expected) noexcept
            : expected_ { expected }
        {}

        // 모두 도착하면 true, 제한 시간이 지나면 false다.
        [[nodiscard]] bool arrive_and_wait(const std::chrono::milliseconds limit)
        {
            std::unique_lock<std::mutex> lock { mutex_ };
            ++arrived_;
            condition_.notify_all();
            return condition_.wait_for(lock, limit, [this]() { return arrived_ >= expected_; });
        }

    private:
        std::mutex mutex_ {};
        std::condition_variable condition_ {};
        std::size_t expected_ { 0 };
        std::size_t arrived_ { 0 };
    };

    // 열릴 때까지 job을 붙잡아 두는 문이다.
    class gate
    {
    public:
        void open()
        {
            {
                const std::lock_guard<std::mutex> lock { mutex_ };
                opened_ = true;
            }
            condition_.notify_all();
        }

        [[nodiscard]] bool wait(const std::chrono::milliseconds limit)
        {
            std::unique_lock<std::mutex> lock { mutex_ };
            return condition_.wait_for(lock, limit, [this]() { return opened_; });
        }

    private:
        std::mutex mutex_ {};
        std::condition_variable condition_ {};
        bool opened_ { false };
    };

    // 일을 마친 job 수를 센다. 모두 끝날 때까지 기다리는 용도다.
    class completion_counter
    {
    public:
        void add()
        {
            {
                const std::lock_guard<std::mutex> lock { mutex_ };
                ++count_;
            }
            condition_.notify_all();
        }

        [[nodiscard]] bool wait_for_count(const std::size_t target, const std::chrono::milliseconds limit)
        {
            std::unique_lock<std::mutex> lock { mutex_ };
            return condition_.wait_for(lock, limit, [this, target]() { return count_ >= target; });
        }

    private:
        std::mutex mutex_ {};
        std::condition_variable condition_ {};
        std::size_t count_ { 0 };
    };

    operation_request make_request(const operation_id id, std::string repository_key, operation_job job, const bool interactive = false)
    {
        operation_request request {};
        request.id = id;
        request.repository_key = std::move(repository_key);
        request.interactive = interactive;
        request.job = std::move(job);
        return request;
    }
} // namespace

TEST_CASE("the default worker count is capped at four", "[scheduler]")
{
    const std::size_t count { default_scheduler_worker_count() };

    CHECK(count >= 1);
    CHECK(count <= 4);
}

TEST_CASE("jobs of one repository run in submission order and never overlap", "[scheduler]")
{
    task_scheduler scheduler { task_scheduler_options { 4, 1 } };
    std::mutex mutex {};
    std::vector<operation_id> order {};
    std::atomic<int> running { 0 };
    std::atomic<int> maximum_running { 0 };
    completion_counter done {};

    constexpr std::size_t job_count { 40 };
    for (std::size_t index = 0; index < job_count; ++index)
    {
        REQUIRE(scheduler.submit(make_request(index + 1, "repo-a", [&](const operation_context& context) {
            const int now_running { ++running };
            int observed { maximum_running.load() };
            while (observed < now_running && maximum_running.compare_exchange_weak(observed, now_running))
            {
            }
            // 앞 job이 오래 걸려도 뒤 job이 끼어들지 못함을 보이려고 짧게 쉰다.
            std::this_thread::sleep_for(1ms);
            {
                const std::lock_guard<std::mutex> lock { mutex };
                order.push_back(context.id);
            }
            --running;
            done.add();
        })));
    }

    REQUIRE(done.wait_for_count(job_count, 20s));
    CHECK(maximum_running.load() == 1);
    REQUIRE(order.size() == job_count);
    for (std::size_t index = 0; index < job_count; ++index)
        CHECK(order[index] == index + 1);
}

TEST_CASE("jobs of different repositories run in parallel", "[scheduler]")
{
    // 두 job이 서로를 기다린다. 같은 worker에 직렬로 놓이면 영영 모이지 못해 실패한다.
    task_scheduler scheduler { task_scheduler_options { 2, 1 } };
    rendezvous both_started { 2 };
    std::atomic<int> met { 0 };
    completion_counter done {};

    for (const char* const key : { "repo-a", "repo-b" })
    {
        REQUIRE(scheduler.submit(make_request(1, key, [&](const operation_context&) {
            if (both_started.arrive_and_wait(10s))
                ++met;
            done.add();
        })));
    }

    REQUIRE(done.wait_for_count(2, 20s));
    CHECK(met.load() == 2);
}

TEST_CASE("many repositories never share a worker while others are idle", "[scheduler]")
{
    // 저장소 이름이 어떻게 묶이든 worker 수만큼은 동시에 실행되어야 한다.
    constexpr std::size_t workers { 4 };
    task_scheduler scheduler { task_scheduler_options { workers, 1 } };
    rendezvous all_started { workers };
    std::atomic<int> met { 0 };
    completion_counter done {};

    for (std::size_t index = 0; index < workers; ++index)
    {
        REQUIRE(scheduler.submit(make_request(index + 1, "repository-" + std::to_string(index * 7 + 3), [&](const operation_context&) {
            if (all_started.arrive_and_wait(10s))
                ++met;
            done.add();
        })));
    }

    REQUIRE(done.wait_for_count(workers, 20s));
    CHECK(met.load() == static_cast<int>(workers));
}

TEST_CASE("a long job does not delay another repository", "[scheduler]")
{
    task_scheduler scheduler { task_scheduler_options { 2, 1 } };
    gate release_long {};
    completion_counter long_done {};
    completion_counter short_done {};

    REQUIRE(scheduler.submit(make_request(1, "slow", [&](const operation_context&) {
        static_cast<void>(release_long.wait(20s));
        long_done.add();
    })));
    REQUIRE(scheduler.submit(make_request(2, "fast", [&](const operation_context&) { short_done.add(); })));

    CHECK(short_done.wait_for_count(1, 10s));
    release_long.open();
    CHECK(long_done.wait_for_count(1, 10s));
}

TEST_CASE("an interactive job is not blocked by a long job of the same repository", "[scheduler]")
{
    task_scheduler scheduler { task_scheduler_options { 1, 1 } };
    gate release_fetch {};
    completion_counter fetch_done {};
    completion_counter diff_done {};

    // 같은 저장소의 fetch가 lane을 점유한다. worker가 하나뿐이라, 대화형 job이 일반 lane에
    // 들어갔다면 fetch가 끝나기 전에는 실행될 수 없다.
    REQUIRE(scheduler.submit(make_request(1, "repo-a", [&](const operation_context&) {
        static_cast<void>(release_fetch.wait(20s));
        fetch_done.add();
    })));
    REQUIRE(scheduler.submit(make_request(2, "repo-a", [&](const operation_context&) { diff_done.add(); }, true)));

    CHECK(diff_done.wait_for_count(1, 10s));
    release_fetch.open();
    CHECK(fetch_done.wait_for_count(1, 10s));
}

TEST_CASE("interactive jobs run in submission order", "[scheduler]")
{
    task_scheduler scheduler { task_scheduler_options { 2, 1 } };
    std::mutex mutex {};
    std::vector<operation_id> order {};
    completion_counter done {};

    for (operation_id id = 1; id <= 10; ++id)
    {
        REQUIRE(scheduler.submit(make_request(
            id, "repo-a",
            [&](const operation_context& context) {
                {
                    const std::lock_guard<std::mutex> lock { mutex };
                    order.push_back(context.id);
                }
                done.add();
            },
            true)));
    }

    REQUIRE(done.wait_for_count(10, 10s));
    for (std::size_t index = 0; index < order.size(); ++index)
        CHECK(order[index] == index + 1);
}

TEST_CASE("a job receives its id and repository key", "[scheduler]")
{
    task_scheduler scheduler {};
    completion_counter done {};
    operation_id seen_id { 0 };
    std::string seen_key {};

    REQUIRE(scheduler.submit(make_request(42, "C:/work/repo", [&](const operation_context& context) {
        seen_id = context.id;
        seen_key = std::string { context.repository_key };
        done.add();
    })));

    REQUIRE(done.wait_for_count(1, 10s));
    CHECK(seen_id == 42);
    CHECK(seen_key == "C:/work/repo");
}

TEST_CASE("an empty job is rejected and a throwing job does not stop its lane", "[scheduler]")
{
    task_scheduler scheduler { task_scheduler_options { 1, 1 } };
    completion_counter done {};

    CHECK_FALSE(scheduler.submit(make_request(1, "repo-a", nullptr)));
    REQUIRE(scheduler.submit(make_request(2, "repo-a", [](const operation_context&) { throw 1; })));
    REQUIRE(scheduler.submit(make_request(3, "repo-a", [&](const operation_context&) { done.add(); })));

    CHECK(done.wait_for_count(1, 10s));
}

TEST_CASE("shutdown joins workers and drains pending jobs with a cancelled token", "[scheduler][shutdown]")
{
    std::atomic<int> invoked { 0 };
    std::atomic<int> saw_cancelled { 0 };
    gate running_started {};
    // 종료 token이 취소되어야만 빠져나온다. 취소 순서를 어기면 제한 시간까지 매달린다.
    const auto hold_until_cancelled = [](const operation_context& context) {
        const auto deadline { std::chrono::steady_clock::now() + 20s };
        while (context.shutdown.cancelled() == false && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(1ms);
    };
    {
        task_scheduler scheduler { task_scheduler_options { 1, 1 } };

        // 하나뿐인 worker가 긴 job에 묶여 있고 뒤에 같은 저장소의 job과 대화형 job이 쌓인다.
        REQUIRE(scheduler.submit(make_request(1, "repo-a", [&](const operation_context& context) {
            ++invoked;
            running_started.open();
            hold_until_cancelled(context);
            if (context.shutdown.cancelled())
                ++saw_cancelled;
        })));
        REQUIRE(running_started.wait(10s));

        for (operation_id id = 2; id <= 4; ++id)
        {
            REQUIRE(scheduler.submit(make_request(id, "repo-a", [&](const operation_context& context) {
                ++invoked;
                if (context.shutdown.cancelled())
                    ++saw_cancelled;
            })));
        }
        REQUIRE(scheduler.submit(make_request(5, "repo-b", [&](const operation_context& context) {
            ++invoked;
            if (context.shutdown.cancelled())
                ++saw_cancelled;
        })));
        // 대화형 lane은 별도 worker라 바로 시작할 수 있다. 종료 전에 시작했어도 취소를 기다린다.
        REQUIRE(scheduler.submit(make_request(
            6, "repo-a",
            [&](const operation_context& context) {
                ++invoked;
                hold_until_cancelled(context);
                if (context.shutdown.cancelled())
                    ++saw_cancelled;
            },
            true)));

        const auto started { std::chrono::steady_clock::now() };
        scheduler.shutdown();
        CHECK(std::chrono::steady_clock::now() - started < 10s);

        // 접수한 job은 모두 정확히 한 번 불렸고 전부 취소 token을 보았다.
        CHECK(invoked.load() == 6);
        CHECK(saw_cancelled.load() == 6);

        CHECK_FALSE(scheduler.submit(make_request(7, "repo-a", [&](const operation_context&) { ++invoked; })));
        scheduler.shutdown();
    }
    CHECK(invoked.load() == 6);
}

TEST_CASE("destroying an idle scheduler returns promptly", "[scheduler][shutdown]")
{
    const auto started { std::chrono::steady_clock::now() };
    {
        task_scheduler scheduler {};
    }

    CHECK(std::chrono::steady_clock::now() - started < 5s);
}
