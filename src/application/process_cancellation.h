#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace cyella {
    namespace detail {
        class process_cancellation_state;
    } // namespace detail

    // 취소 콜백 등록의 수명을 관리한다. 소멸 시 콜백을 해제하며, 다른 스레드에서
    // 콜백이 실행 중이면 그 실행이 끝날 때까지 기다린다. 따라서 콜백은 자신의
    // registration을 소멸시키면 안 된다.
    class process_cancellation_registration
    {
    public:
        process_cancellation_registration() noexcept = default;
        process_cancellation_registration(const process_cancellation_registration&) = delete;
        process_cancellation_registration(process_cancellation_registration&& other) noexcept;
        process_cancellation_registration& operator=(const process_cancellation_registration&) = delete;
        process_cancellation_registration& operator=(process_cancellation_registration&& other) noexcept;
        ~process_cancellation_registration();

        void release() noexcept;

    private:
        process_cancellation_registration(std::shared_ptr<detail::process_cancellation_state> state, std::uint64_t id) noexcept;

        std::shared_ptr<detail::process_cancellation_state> state_ {};
        std::uint64_t id_ {};

        friend class process_cancellation_token;
    };

    // 값으로 복사해 전달하는 취소 관찰자다. 기본 생성한 token은 절대 취소되지 않으므로
    // 취소가 필요 없는 호출자는 빈 token을 그대로 넘길 수 있다.
    class process_cancellation_token
    {
    public:
        process_cancellation_token() noexcept = default;
        process_cancellation_token(const process_cancellation_token&) noexcept = default;
        process_cancellation_token(process_cancellation_token&&) noexcept = default;
        process_cancellation_token& operator=(const process_cancellation_token&) noexcept = default;
        process_cancellation_token& operator=(process_cancellation_token&&) noexcept = default;
        ~process_cancellation_token() = default;

        [[nodiscard]] bool cancellable() const noexcept;
        [[nodiscard]] bool cancelled() const noexcept;
        // 이미 취소된 token이면 콜백을 즉시 같은 스레드에서 실행한다. 콜백은 예외를
        // 던지지 않고 짧아야 하며, Win32 runner는 event 하나를 신호하는 데만 사용한다.
        [[nodiscard]] process_cancellation_registration register_callback(std::function<void()> callback) const;

    private:
        explicit process_cancellation_token(std::shared_ptr<detail::process_cancellation_state> state) noexcept;

        std::shared_ptr<detail::process_cancellation_state> state_ {};

        friend class process_cancellation_source;
    };

    // 취소를 요청하는 소유자 측 handle이다. 여러 스레드에서 동시에 요청해도 안전하다.
    class process_cancellation_source
    {
    public:
        process_cancellation_source();
        process_cancellation_source(const process_cancellation_source&) = delete;
        process_cancellation_source(process_cancellation_source&&) noexcept = default;
        process_cancellation_source& operator=(const process_cancellation_source&) = delete;
        process_cancellation_source& operator=(process_cancellation_source&&) noexcept = default;
        ~process_cancellation_source() = default;

        void request_cancellation();
        [[nodiscard]] bool cancellation_requested() const noexcept;
        [[nodiscard]] process_cancellation_token token() const noexcept;
        // 소유자 밖에서 취소를 걸 수 있는 약한 손잡이다. source의 상태가 이미 사라졌으면
        // false를 돌려주고 아무 일도 하지 않는다. 여러 스레드에서 안전하다.
        [[nodiscard]] std::function<bool()> weak_canceller() const;

    private:
        std::shared_ptr<detail::process_cancellation_state> state_ {};
    };

    // `weak_canceller()`가 돌려준 손잡이가 가리키는 source가 아직 살아 있는지 본다.
    // 취소를 걸지 않으므로 보관소가 죽은 항목을 정리하는 데 쓸 수 있다. 다른 곳에서
    // 만든 함수는 알 수 없으므로 살아 있다고 본다.
    [[nodiscard]] bool weak_canceller_alive(const std::function<bool()>& canceller) noexcept;
} // namespace cyella
