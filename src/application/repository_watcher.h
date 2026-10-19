#pragma once

#include "application/repository_change_classifier.h"
#include "domain/diagnostic.h"

#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace cyella {
    enum class repository_change_kind
    {
        // 작업 트리 파일이 바뀌었다.
        working_tree_changed,
        // HEAD·index·ref·진행 중 작업 상태가 바뀌었다.
        repository_state_changed,
    };

    // 감시 스레드에서 불린다. 오래 걸리거나 감시기를 멈추면 안 된다. 소비자는 보통 앱 메시지를
    // 올려 logic 스레드에서 일하게 한다. 변경이 두 종류 모였으면 `repository_state_changed`가
    // 먼저 온다.
    using repository_change_callback = std::function<void(repository_change_kind)>;

    struct repository_watch_options
    {
        debounce_options debounce {};
    };

    // 실행 중인 감시 하나다. 정책은 아래와 같다.
    //
    // - **git을 실행하지 않는다.** 알리기만 하고 `status`는 소비자가 돌린다.
    // - **.gitignore를 따르지 않는다.** 무시된 경로(`node_modules`, 빌드 산출물)의 변경도
    //   `working_tree_changed`가 된다. 무시 규칙을 풀려면 `git check-ignore`가 필요한데 감시
    //   스레드는 프로세스를 띄우지 않는다. 소비자가 `status`를 돌려 보면 어차피 걸러지고,
    //   디바운스가 호출 횟수를 줄인다. 산출물이 쏟아지는 저장소는 디바운스의 최대 대기
    //   시간(기본 2초)마다 한 번 `status`가 돈다.
    class repository_watcher
    {
    public:
        repository_watcher() = default;
        repository_watcher(const repository_watcher&) = delete;
        repository_watcher(repository_watcher&&) = delete;
        repository_watcher& operator=(const repository_watcher&) = delete;
        repository_watcher& operator=(repository_watcher&&) = delete;
        virtual ~repository_watcher() = default;

        // 감시를 멈춘다. 어느 스레드에서 몇 번 불러도 안전하며 100 ms 안에 돌아온다.
        // 돌아온 뒤에는 콜백이 더 불리지 않는다. 단, 콜백 안에서 부르면 스스로를 기다릴 수
        // 없으니 멈추라는 표시만 하고 돌아온다. 그 콜백이 끝나면 더는 불리지 않는다.
        virtual void stop() noexcept = 0;
    };

    struct repository_watch_start_result
    {
        // 감시를 시작하지 못했으면 null이다.
        std::unique_ptr<repository_watcher> watcher {};
        std::vector<diagnostic> diagnostics {};
    };

    // 감시기를 만드는 경계다. 소비자(탭 세션)가 이 인터페이스에만 의존해 가짜로 바꿀 수 있다.
    class repository_watcher_factory
    {
    public:
        repository_watcher_factory() = default;
        repository_watcher_factory(const repository_watcher_factory&) = delete;
        repository_watcher_factory(repository_watcher_factory&&) = delete;
        repository_watcher_factory& operator=(const repository_watcher_factory&) = delete;
        repository_watcher_factory& operator=(repository_watcher_factory&&) = delete;
        virtual ~repository_watcher_factory() = default;

        // `working_tree`는 작업 트리 절대 경로다. 시작하면 곧바로 감시한다.
        [[nodiscard]] virtual repository_watch_start_result start(std::u8string_view working_tree, repository_change_callback callback, const repository_watch_options& options) = 0;
    };
} // namespace cyella
