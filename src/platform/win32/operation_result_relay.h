#pragma once

#include "application/app_messages.h"
#include "luil/app/app_host.h"
#include "luil/ui/app_message.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <vector>

namespace cyella::win32 {
    // worker → logic 중계 큐다. luil의 app inbox는 포화하면 조용히 버리는
    // reject_newest라 worker 결과를 그대로 실을 수 없다. 그래서 **큐가 정본**이고
    // host의 inbox에는 깨우기(`relay_drain_intent`)만 넣는다. 깨우기가 유실되는
    // 경우는 이미 1,024개가 대기 중이라는 뜻이라 곧 `handle`이 불리고 그때 비워진다.
    //
    // `post`는 worker thread가, `drain`은 logic thread가, `bind`·`unbind`는 UI
    // thread가 부른다. mutex 하나로 전부 지킨다.
    class operation_result_relay
    {
    public:
        operation_result_relay() = default;
        operation_result_relay(const operation_result_relay&) = delete;
        operation_result_relay(operation_result_relay&&) = delete;
        operation_result_relay& operator=(const operation_result_relay&) = delete;
        operation_result_relay& operator=(operation_result_relay&&) = delete;
        ~operation_result_relay() = default;

        // 어느 스레드든 부를 수 있다. lock 안에서 큐에 넣고, host가 묶여 있으면
        // 깨우기 하나를 보낸다. 내용은 큐에 남는다.
        void post(luil::app_message message);

        // UI thread(delegate의 `on_started`)가 부른다. 한 lock 안에서 host를 세우고
        // 큐가 비어 있지 않으면 깨우기 하나를 보낸다 — 묶이기 전에 도착한 결과가
        // 다음 입력까지 잠들지 않게 하는 조치다.
        void bind(luil::app_host& host);

        // 종료의 `stop_workers`가 부른다. host를 놓으므로 이후 `post`는 큐에만
        // 쌓이고 아무도 깨우지 않는다.
        void unbind() noexcept;

        // logic thread가 `handle`·`tick` 첫 줄에서 부른다. 큐를 통째로 꺼내
        // lock 밖에서 sink에 도착 순서대로 넘긴다.
        void drain(const std::function<void(luil::app_message)>& sink);

        // logic thread가 UI thread의 클립보드에 글을 넣어 달라고 청한다 (메뉴의 SHA·이름·경로 복사). host가 묶이지 않았으면
        // 아무 일도 없다.
        void copy_to_clipboard(std::u8string text);

        // 아직 배출되지 않은 메시지 수다 (test 관찰용).
        [[nodiscard]] std::size_t pending();

    private:
        std::mutex mutex_ {};
        std::vector<luil::app_message> queue_ {};
        luil::app_host* host_ { nullptr };
    };
} // namespace cyella::win32
