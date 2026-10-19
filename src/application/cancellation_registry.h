#pragma once

#include "application/process_cancellation.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

namespace cyella {
    // 종료·변경 작업·탐색의 취소 source를 모아 두는 thread-safe 보관소다.
    // driver의 cancel()이 UI thread에서 cancel_all()을 불러 close 메시지가 유실돼도
    // worker join이 매달리지 않게 한다.
    class cancellation_registry
    {
    public:
        // source.weak_canceller()를 보관한다. 사라진 항목은 add·cancel_all 때 정리한다.
        void add(const process_cancellation_source& source);
        // 살아 있는 source 전부에 request_cancellation을 건다. noexcept, 어느 스레드든.
        void cancel_all() noexcept;
        // 살아 있는 항목 수다 (test 관찰용; 정리 후 값).
        [[nodiscard]] std::size_t size();

    private:
        // 상태가 사라진 손잡이를 지운다. 호출자가 mutex_를 잡고 있어야 한다.
        void collect() noexcept;

        std::mutex mutex_ {};
        std::vector<std::function<bool()>> cancellers_ {};
    };
} // namespace cyella
