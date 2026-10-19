#include "platform/win32/operation_result_relay.h"

#include <utility>

namespace cyella::win32 {
    namespace {
        // 깨우기 메시지다. 내용은 큐에 있으므로 이 값 자체는 아무 뜻도 나르지 않는다.
        [[nodiscard]] luil::app_message make_wake_message()
        {
            return luil::app_message { relay_drain_intent {} };
        }
    } // namespace

    void operation_result_relay::post(luil::app_message message)
    {
        const std::lock_guard<std::mutex> lock { mutex_ };
        queue_.push_back(std::move(message));
        // 아직 묶이지 않았으면 큐에만 쌓는다. `bind`가 그때 한 번 깨운다.
        if (host_ != nullptr)
            host_->post_app_message(make_wake_message());
    }

    void operation_result_relay::bind(luil::app_host& host)
    {
        const std::lock_guard<std::mutex> lock { mutex_ };
        host_ = &host;
        if (queue_.empty() == false)
            host_->post_app_message(make_wake_message());
    }

    void operation_result_relay::unbind() noexcept
    {
        const std::lock_guard<std::mutex> lock { mutex_ };
        host_ = nullptr;
    }

    void operation_result_relay::drain(const std::function<void(luil::app_message)>& sink)
    {
        std::vector<luil::app_message> drained {};
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            if (queue_.empty())
                return;
            drained.swap(queue_);
        }

        // sink는 logic을 돌리므로 lock 밖에서 부른다. 그 안에서 worker가 결과를
        // 더 넣어도 다음 배출에서 순서대로 나온다.
        if (sink == nullptr)
            return;
        for (luil::app_message& message : drained)
            sink(std::move(message));
    }

    std::size_t operation_result_relay::pending()
    {
        const std::lock_guard<std::mutex> lock { mutex_ };
        return queue_.size();
    }
} // namespace cyella::win32
