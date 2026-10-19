#include "application/cancellation_registry.h"

#include <utility>

namespace cyella {
    void cancellation_registry::add(const process_cancellation_source& source)
    {
        // 손잡이는 약한 참조라 보관소가 source의 수명을 늘리지 않는다.
        std::function<bool()> canceller { source.weak_canceller() };

        const std::lock_guard<std::mutex> guard { mutex_ };
        collect();
        cancellers_.push_back(std::move(canceller));
    }

    void cancellation_registry::cancel_all() noexcept
    {
        try
        {
            const std::lock_guard<std::mutex> guard { mutex_ };
            collect();
            // 취소 콜백은 event 하나를 신호하는 짧은 일이라(process_cancellation의
            // 계약) lock 안에서 불러도 된다. 손잡이는 살아 있는 것만 남아 있다.
            for (const std::function<bool()>& canceller : cancellers_)
                canceller();
        }
        catch (...)
        {
            // request_cancellation은 noexcept가 아니다. 종료 경로라 삼킨다.
        }
    }

    std::size_t cancellation_registry::size()
    {
        const std::lock_guard<std::mutex> guard { mutex_ };
        collect();
        return cancellers_.size();
    }

    void cancellation_registry::collect() noexcept
    {
        // 살아 있는 항목만 앞으로 접어 모은다. 손잡이를 부르지 않으므로 정리가
        // 취소를 일으키지 않는다.
        std::size_t kept { 0 };
        for (std::size_t index = 0; index < cancellers_.size(); ++index)
        {
            if (weak_canceller_alive(cancellers_[index]) == false)
                continue;
            if (kept != index)
                cancellers_[kept] = std::move(cancellers_[index]);
            ++kept;
        }
        cancellers_.resize(kept);
    }
} // namespace cyella
