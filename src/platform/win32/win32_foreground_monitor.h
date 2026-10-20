#pragma once

#include <functional>
#include <memory>

namespace cyella::win32 {
    // 이 프로세스의 창이 다른 프로세스의 창에서 앞(foreground)으로 돌아올 때마다 알린다.
    //
    // luil은 창 활성화를 앱에 알리지 않는다(`activation_changed`는 열린 popup을 닫는 계기일 뿐이다). 그래서
    // `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)`로 시스템 전체의 앞 창 전환을 듣고, 새 앞 창의 프로세스가 이 프로세스가 아니었다가
    // 이 프로세스가 되는 전환만 알린다. 이 프로세스 안의 다른 창(파일 대화 상자, 메뉴)으로의 이동은 알리지 않는다.
    //
    // 메시지 루프가 도는 스레드(UI thread)에서 만들고 없애야 한다. 콜백도 그 스레드에서 불린다. 한 프로세스에 하나만 만든다.
    class foreground_monitor
    {
    public:
        // 앞 창의 프로세스가 이 프로세스가 아니었다가 이 프로세스로 바뀔 때 부른다.
        using activation_callback = std::function<void()>;

        // 훅을 걸지 못하면 null이다 (알림이 오지 않을 뿐 앱은 정상이다).
        [[nodiscard]] static std::unique_ptr<foreground_monitor> start(activation_callback on_activated);

        foreground_monitor(const foreground_monitor&) = delete;
        foreground_monitor& operator=(const foreground_monitor&) = delete;
        ~foreground_monitor();

    private:
        foreground_monitor() = default;
    };
} // namespace cyella::win32
