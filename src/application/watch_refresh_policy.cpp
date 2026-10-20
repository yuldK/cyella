#include "application/watch_refresh_policy.h"

#include <utility>

namespace cyella {
    watch_refresh_policy::watch_refresh_policy(watch_refresh_options options) noexcept
        : options_ { options }
    {}

    std::optional<refresh_scope> watch_refresh_policy::on_change(const std::string& repository, const refresh_scope wanted, const repository_activity activity, const clock::time_point now)
    {
        entry& state { entries_[repository] };
        // 방금 낸 전체 새로 고침이 같은 묶음의 상태 범위 변경을 덮는다.
        if (wanted == refresh_scope::status && activity.write_pending == false && activity.refresh_pending && state.last_full.has_value() && now - *state.last_full < options_.same_batch_window)
            return std::nullopt;
        // 쓰기가 끝나면 workspace가 refresh를 내므로 그것이 이 변경을 덮는다. 보류해 둔 것도 함께 버린다.
        if (activity.write_pending)
        {
            state.pending.reset();
            return std::nullopt;
        }
        // 도는 새로 고침이 이 변경보다 앞서 읽었을 수 있다. 끝난 뒤 한 번 더 읽도록 표시만 남긴다. 둘 이상 모이면 넓은 쪽이 이긴다.
        if (activity.refresh_pending)
        {
            state.pending = state.pending.has_value() ? wider_scope(*state.pending, wanted) : wanted;
            return std::nullopt;
        }
        // 지금 내므로 앞서 보류한 것도 이 새로 고침이 덮는다. 범위만 넓힌다.
        const refresh_scope scope { state.pending.has_value() ? wider_scope(*state.pending, wanted) : wanted };
        state.pending.reset();
        if (scope == refresh_scope::full)
            state.last_full = now;
        return scope;
    }

    std::optional<refresh_scope> watch_refresh_policy::on_focus(const std::string& repository, const repository_activity activity, const clock::time_point now)
    {
        if (last_focus_.has_value() && now - *last_focus_ < options_.focus_debounce)
            return std::nullopt;
        last_focus_ = now;
        const bool lost { entries_[repository].lost };
        return on_change(repository, lost ? refresh_scope::full : refresh_scope::status, activity, now);
    }

    std::optional<refresh_scope> watch_refresh_policy::on_activity(const std::string& repository, const repository_activity activity)
    {
        const auto found { entries_.find(repository) };
        if (found == entries_.end() || found->second.pending.has_value() == false)
            return std::nullopt;
        if (activity.write_pending)
        {
            found->second.pending.reset();
            return std::nullopt;
        }
        if (activity.refresh_pending)
            return std::nullopt;
        const refresh_scope scope { *found->second.pending };
        found->second.pending.reset();
        // 보류했던 새로 고침을 지금 낸다. 시각은 모르므로 같은 묶음 검사에는 쓰지 않는다.
        found->second.last_full.reset();
        return scope;
    }

    void watch_refresh_policy::mark_watch_lost(const std::string& repository)
    {
        entries_[repository].lost = true;
    }

    bool watch_refresh_policy::watch_lost(const std::string& repository) const
    {
        const auto found { entries_.find(repository) };
        return found != entries_.end() && found->second.lost;
    }

    void watch_refresh_policy::mark_watch_started(const std::string& repository)
    {
        entries_[repository].lost = false;
    }

    void watch_refresh_policy::forget(const std::string& repository)
    {
        entries_.erase(repository);
    }

    std::optional<refresh_scope> watch_refresh_policy::pending(const std::string& repository) const
    {
        const auto found { entries_.find(repository) };
        if (found == entries_.end())
            return std::nullopt;
        return found->second.pending;
    }
} // namespace cyella
