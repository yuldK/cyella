#pragma once

#include "application/operation_executor.h"
#include "application/process_cancellation.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace cyella {
    // 일반 worker 수의 기본값이다. `min(4, hardware_concurrency)`이며 최소 1이다.
    [[nodiscard]] std::size_t default_scheduler_worker_count() noexcept;

    // 읽기 lane worker 수의 기본값이다.
    inline constexpr std::size_t default_interactive_worker_count { 2 };

    struct task_scheduler_options
    {
        // 저장소 lane을 실행하는 worker 수다. 0이면 1로 본다.
        std::size_t worker_count { default_scheduler_worker_count() };
        // 읽기 조회 전용 worker 수다. 같은 큐를 나눠 먹는다. 0이면 1로 본다. 기본이 둘인 이유는
        // 한 저장소의 10만 커밋 log 스트림이 다른 저장소의 diff를 막지 않게 하기 위해서다.
        std::size_t interactive_worker_count { default_interactive_worker_count };
    };

    // lane 규칙이다.
    //
    // - 같은 `repository_key`의 작업은 접수 순서대로 하나씩 실행한다. index·ref·작업 트리를
    //   바꾸는 작업(fetch 포함)을 저장소마다 같은 키로 보낸다. 읽기(status·log·diff)는
    //   `--no-optional-locks`로 잠금을 잡지 않으므로 이 lane에 넣지 않는다(ADR-003).
    // - 서로 다른 키는 병렬이다. 동시 실행 상한은 일반 worker 수다. lane을 worker에 고정하지
    //   않고 일이 있는 lane을 노는 worker가 가져가므로, 서로 다른 저장소가 우연히 같은
    //   worker에 몰려 서로를 기다리는 일이 없다. 준비된 lane은 접수 순서로 돌아가며 실행한다.
    // - `interactive` 작업(읽기)은 전용 worker들이 접수 순서로 꺼내 병렬로 실행한다.
    //
    // 접수한 job은 종료 중에도 반드시 한 번 호출된다 (취소된 token과 함께).
    class task_scheduler final
    {
    public:
        explicit task_scheduler(const task_scheduler_options& options = {});
        task_scheduler(const task_scheduler&) = delete;
        task_scheduler(task_scheduler&&) = delete;
        task_scheduler& operator=(const task_scheduler&) = delete;
        task_scheduler& operator=(task_scheduler&&) = delete;
        ~task_scheduler();

        // 접수하면 true다. 종료가 시작됐거나 job이 비어 있으면 false이며 job은 호출되지 않는다.
        [[nodiscard]] bool submit(operation_request request);

        // 종료 token을 취소하고, 큐에 남은 job을 취소된 token으로 소진한 뒤 worker를 join한다.
        // 멱등이며 여러 스레드에서 불러도 안전하다. 실행 중인 job이 token을 존중하지 않으면
        // 그 job이 끝날 때까지 기다린다.
        void shutdown() noexcept;

        // 저장소 lane을 실행하는 worker 수다. 대화형 worker는 세지 않는다.
        [[nodiscard]] std::size_t worker_count() const noexcept;

    private:
        // lane 하나의 상태다. 키가 `ready_`에 있다는 것은 대기 작업이 있고 실행 중이 아니라는 뜻이다.
        struct lane
        {
            std::deque<operation_request> pending {};
            bool running { false };
        };

        void repository_worker_loop();
        void interactive_worker_loop();
        void run_job(const operation_request& request) noexcept;

        process_cancellation_source shutdown_source_ {};
        std::size_t worker_count_ { 0 };
        std::mutex mutex_ {};
        std::condition_variable repository_ready_ {};
        std::condition_variable interactive_ready_ {};
        std::unordered_map<std::string, lane> lanes_ {};
        std::deque<std::string> ready_ {};
        std::deque<operation_request> interactive_pending_ {};
        bool stopping_ { false };
        // 종료가 끝날 때까지 다른 호출자도 기다리게 하는 잠금이다.
        std::mutex shutdown_mutex_ {};
        std::vector<std::thread> workers_ {};
    };
} // namespace cyella
