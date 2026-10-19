#include "application/process_cancellation.h"

#include <atomic>
#include <cstddef>
#include <mutex>
#include <utility>
#include <vector>

namespace cyella {
    namespace detail {
        // 콜백은 mutex를 잡은 상태에서 실행한다. 그래서 registration 소멸자가 콜백
        // 실행과 겹치더라도 콜백이 끝난 뒤에만 해제되며 dangling 참조가 생기지 않는다.
        class process_cancellation_state
        {
        public:
            void request()
            {
                const std::lock_guard<std::mutex> guard { mutex_ };
                if (cancelled_.exchange(true, std::memory_order_acq_rel))
                    return;

                for (const entry& value : callbacks_)
                    if (value.callback)
                        value.callback();
            }

            [[nodiscard]] bool cancelled() const noexcept
            {
                return cancelled_.load(std::memory_order_acquire);
            }

            [[nodiscard]] std::uint64_t add(std::function<void()> callback)
            {
                std::unique_lock<std::mutex> guard { mutex_ };
                if (cancelled_.load(std::memory_order_acquire))
                {
                    guard.unlock();
                    if (callback)
                        callback();
                    return 0;
                }

                const std::uint64_t id { next_id_++ };
                callbacks_.push_back(entry { id, std::move(callback) });
                return id;
            }

            void remove(const std::uint64_t id) noexcept
            {
                if (id == 0)
                    return;

                const std::lock_guard<std::mutex> guard { mutex_ };
                for (std::size_t index = 0; index < callbacks_.size(); ++index)
                    if (callbacks_[index].id == id)
                    {
                        callbacks_.erase(callbacks_.begin() + static_cast<std::ptrdiff_t>(index));
                        return;
                    }
            }

        private:
            struct entry
            {
                std::uint64_t id {};
                std::function<void()> callback {};
            };

            mutable std::mutex mutex_ {};
            std::atomic_bool cancelled_ { false };
            std::uint64_t next_id_ { 1 };
            std::vector<entry> callbacks_ {};
        };
    } // namespace detail

    namespace {
        // `weak_canceller`가 돌려주는 호출체다. 이름 있는 타입이라 `std::function`에
        // 담긴 뒤에도 취소를 걸지 않고 상태가 살아 있는지 물어볼 수 있다.
        struct weak_cancel_call
        {
            std::weak_ptr<detail::process_cancellation_state> state {};

            bool operator()() const
            {
                const std::shared_ptr<detail::process_cancellation_state> locked { state.lock() };
                if (locked == nullptr)
                    return false;
                locked->request();
                return true;
            }
        };
    } // namespace

    process_cancellation_registration::process_cancellation_registration(std::shared_ptr<detail::process_cancellation_state> state, const std::uint64_t id) noexcept
        : state_ { std::move(state) }
        , id_ { id }
    {}

    process_cancellation_registration::process_cancellation_registration(process_cancellation_registration&& other) noexcept
        : state_ { std::move(other.state_) }
        , id_ { other.id_ }
    {
        other.id_ = 0;
    }

    process_cancellation_registration& process_cancellation_registration::operator=(process_cancellation_registration&& other) noexcept
    {
        if (this == &other)
            return *this;

        release();
        state_ = std::move(other.state_);
        id_ = other.id_;
        other.id_ = 0;
        return *this;
    }

    process_cancellation_registration::~process_cancellation_registration()
    {
        release();
    }

    void process_cancellation_registration::release() noexcept
    {
        if (state_)
            state_->remove(id_);
        state_.reset();
        id_ = 0;
    }

    process_cancellation_token::process_cancellation_token(std::shared_ptr<detail::process_cancellation_state> state) noexcept
        : state_ { std::move(state) }
    {}

    bool process_cancellation_token::cancellable() const noexcept
    {
        return state_ != nullptr;
    }

    bool process_cancellation_token::cancelled() const noexcept
    {
        return state_ != nullptr && state_->cancelled();
    }

    process_cancellation_registration process_cancellation_token::register_callback(std::function<void()> callback) const
    {
        if (state_ == nullptr)
            return {};
        return process_cancellation_registration { state_, state_->add(std::move(callback)) };
    }

    process_cancellation_source::process_cancellation_source()
        : state_ { std::make_shared<detail::process_cancellation_state>() }
    {}

    void process_cancellation_source::request_cancellation()
    {
        if (state_)
            state_->request();
    }

    bool process_cancellation_source::cancellation_requested() const noexcept
    {
        return state_ != nullptr && state_->cancelled();
    }

    process_cancellation_token process_cancellation_source::token() const noexcept
    {
        return process_cancellation_token { state_ };
    }

    std::function<bool()> process_cancellation_source::weak_canceller() const
    {
        // 약한 참조라 손잡이가 source의 수명을 늘리지 않는다. source가 사라진 뒤에
        // 불러도 안전하다.
        return std::function<bool()> { weak_cancel_call { state_ } };
    }

    bool weak_canceller_alive(const std::function<bool()>& canceller) noexcept
    {
        const weak_cancel_call* const call { canceller.target<weak_cancel_call>() };
        if (call == nullptr)
            return true;
        return call->state.expired() == false;
    }
} // namespace cyella
