#include "application/task_scheduler.h"

#include <algorithm>
#include <utility>

namespace cyella {
    std::size_t default_scheduler_worker_count() noexcept
    {
        const std::size_t hardware { std::thread::hardware_concurrency() };
        // 값을 알 수 없으면 0이 돌아온다. 그때도 병렬 실행이 되도록 상한인 4를 쓴다.
        return std::max<std::size_t>(1, std::min<std::size_t>(4, hardware == 0 ? 4 : hardware));
    }

    task_scheduler::task_scheduler(const task_scheduler_options& options)
        : worker_count_ { std::max<std::size_t>(options.worker_count, 1) }
    {
        const std::size_t interactive_count { std::max<std::size_t>(options.interactive_worker_count, 1) };
        workers_.reserve(worker_count_ + interactive_count);
        try
        {
            for (std::size_t index = 0; index < worker_count_; ++index)
                workers_.emplace_back(&task_scheduler::repository_worker_loop, this);
            for (std::size_t index = 0; index < interactive_count; ++index)
                workers_.emplace_back(&task_scheduler::interactive_worker_loop, this);
        }
        catch (...)
        {
            // 생성자가 예외로 나가면 소멸자가 불리지 않아 joinable 스레드가 남는다.
            shutdown();
            throw;
        }
    }

    task_scheduler::~task_scheduler()
    {
        shutdown();
    }

    bool task_scheduler::submit(operation_request request)
    {
        if (request.job == nullptr)
            return false;

        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            if (stopping_)
                return false;

            if (request.interactive)
            {
                interactive_pending_.push_back(std::move(request));
                interactive_ready_.notify_one();
                return true;
            }

            std::string key { request.repository_key };
            lane& target { lanes_[key] };
            target.pending.push_back(std::move(request));
            // 실행 중인 lane은 끝날 때 스스로 다시 줄을 서고, 이미 줄 서 있는 lane은 건드릴
            // 필요가 없다. 비어 있다가 처음 채워진 lane만 줄에 세운다.
            if (target.running == false && target.pending.size() == 1)
            {
                ready_.push_back(std::move(key));
                repository_ready_.notify_one();
            }
        }
        return true;
    }

    void task_scheduler::shutdown() noexcept
    {
        const std::lock_guard<std::mutex> shutdown_guard { shutdown_mutex_ };
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            stopping_ = true;
        }
        // 취소를 먼저 건다. 큐에 남은 job이 취소된 token을 보고 빠르게 끝나고, 실행 중인
        // process_runner도 같은 token으로 자식을 정리한다.
        shutdown_source_.request_cancellation();
        repository_ready_.notify_all();
        interactive_ready_.notify_all();

        for (std::thread& worker : workers_)
            if (worker.joinable())
                worker.join();
    }

    std::size_t task_scheduler::worker_count() const noexcept
    {
        return worker_count_;
    }

    void task_scheduler::repository_worker_loop()
    {
        std::unique_lock<std::mutex> lock { mutex_ };
        while (true)
        {
            repository_ready_.wait(lock, [this]() { return stopping_ || ready_.empty() == false; });
            // 종료 중에도 줄 서 있는 lane은 끝까지 소진한다. 접수한 job은 한 번씩 불려야 한다.
            if (ready_.empty())
                return;

            std::string key { std::move(ready_.front()) };
            ready_.pop_front();
            lane& source { lanes_[key] };
            operation_request request { std::move(source.pending.front()) };
            source.pending.pop_front();
            source.running = true;

            lock.unlock();
            run_job(request);
            lock.lock();

            // 실행 중에 submit이 lanes_를 늘려도 unordered_map의 요소 참조는 유효하지만,
            // 다시 찾아 쓰면 이 함수가 참조 수명에 기대지 않는다.
            lane& finished { lanes_[key] };
            finished.running = false;
            if (finished.pending.empty())
            {
                lanes_.erase(key);
                continue;
            }
            ready_.push_back(std::move(key));
            repository_ready_.notify_one();
        }
    }

    void task_scheduler::interactive_worker_loop()
    {
        std::unique_lock<std::mutex> lock { mutex_ };
        while (true)
        {
            interactive_ready_.wait(lock, [this]() { return stopping_ || interactive_pending_.empty() == false; });
            if (interactive_pending_.empty())
                return;

            operation_request request { std::move(interactive_pending_.front()) };
            interactive_pending_.pop_front();

            lock.unlock();
            run_job(request);
            lock.lock();
        }
    }

    void task_scheduler::run_job(const operation_request& request) noexcept
    {
        try
        {
            const operation_context context { request.id, request.repository_key, shutdown_source_.token() };
            request.job(context);
        }
        catch (...)
        {
            // job은 예외를 던지지 않는 것이 계약이다. 어겨도 worker가 죽으면 그 lane의
            // 나머지 작업이 영영 실행되지 않으므로 삼키고 다음으로 넘어간다.
        }
    }
} // namespace cyella
