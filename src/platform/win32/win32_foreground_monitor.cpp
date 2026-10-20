#include "platform/win32/win32_foreground_monitor.h"

#include <windows.h>

#include <utility>

namespace cyella::win32 {
    namespace {
        // 훅 콜백은 인자로 사용자 값을 받지 못해 전역 하나를 쓴다. UI thread 하나에서만 만들고 없애고 부른다.
        struct monitor_state
        {
            HWINEVENTHOOK hook { nullptr };
            foreground_monitor::activation_callback on_activated {};
            // 직전 앞 창이 이 프로세스의 것이었는가. 처음에는 모르므로 이미 앞이라고 본다(시작할 때 알리지 않는다).
            bool was_ours { true };
        };

        monitor_state* active_state { nullptr };

        void CALLBACK on_foreground(HWINEVENTHOOK, const DWORD event, const HWND window, const LONG, const LONG, const DWORD, const DWORD)
        {
            monitor_state* const state { active_state };
            if (state == nullptr || event != EVENT_SYSTEM_FOREGROUND)
                return;
            DWORD process { 0 };
            if (window != nullptr)
                GetWindowThreadProcessId(window, &process);
            const bool ours { process != 0 && process == GetCurrentProcessId() };
            const bool became_active { ours && state->was_ours == false };
            state->was_ours = ours;
            if (became_active && state->on_activated != nullptr)
                state->on_activated();
        }
    } // namespace

    std::unique_ptr<foreground_monitor> foreground_monitor::start(activation_callback on_activated)
    {
        if (active_state != nullptr)
            return nullptr;
        auto state { std::make_unique<monitor_state>() };
        state->on_activated = std::move(on_activated);
        // 지금 앞 창이 다른 프로세스면 첫 전환부터 알린다.
        if (const HWND current { GetForegroundWindow() }; current != nullptr)
        {
            DWORD process { 0 };
            GetWindowThreadProcessId(current, &process);
            state->was_ours = process == GetCurrentProcessId();
        }
        // OUTOFCONTEXT: 콜백이 이 스레드의 메시지 루프에서 불린다. 다른 프로세스에 DLL을 넣지 않는다.
        state->hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &on_foreground, 0, 0, WINEVENT_OUTOFCONTEXT);
        if (state->hook == nullptr)
            return nullptr;
        active_state = state.release();
        return std::unique_ptr<foreground_monitor>(new foreground_monitor {});
    }

    foreground_monitor::~foreground_monitor()
    {
        monitor_state* const state { active_state };
        active_state = nullptr;
        if (state == nullptr)
            return;
        if (state->hook != nullptr)
            UnhookWinEvent(state->hook);
        delete state;
    }
} // namespace cyella::win32
