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
        //  - 배출 전에는 깨우기 하나면 된다. post마다 보내면 결과가 쏟아질 때 깨우기가 app inbox를 채워 사용자의 입력이 버려진다.
        //  - 보낸 깨우기가 inbox에 들어가지 못했으면(포화) 다음 post가 다시 보낸다. 포화한 inbox는 곧 `handle`을 부르고
        //    `handle`은 늘 먼저 배출한다.
        if (host_ != nullptr && wake_pending_ == false)
            wake_pending_ = host_->post_app_message_ticketed(make_wake_message()) != 0;
    }

    void operation_result_relay::bind(luil::app_host& host)
    {
        const std::lock_guard<std::mutex> lock { mutex_ };
        host_ = &host;
        wake_pending_ = false;
        if (queue_.empty() == false)
            wake_pending_ = host_->post_app_message_ticketed(make_wake_message()) != 0;
    }

    void operation_result_relay::unbind() noexcept
    {
        const std::lock_guard<std::mutex> lock { mutex_ };
        host_ = nullptr;
        wake_pending_ = false;
    }

    void operation_result_relay::drain(const std::function<void(luil::app_message)>& sink)
    {
        std::vector<luil::app_message> drained {};
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            // 큐를 비우는 이 순간부터 들어오는 결과는 새 깨우기가 있어야 한다.
            wake_pending_ = false;
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

    void operation_result_relay::copy_to_clipboard(std::u8string text)
    {
        const std::lock_guard<std::mutex> lock { mutex_ };
        if (host_ != nullptr)
            host_->post_clipboard_request(luil::clipboard_copy_request { std::move(text) });
    }

    std::size_t operation_result_relay::pending()
    {
        const std::lock_guard<std::mutex> lock { mutex_ };
        return queue_.size();
    }
} // namespace cyella::win32
