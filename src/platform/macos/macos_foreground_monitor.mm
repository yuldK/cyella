#include "platform/win32/win32_foreground_monitor.h"

#import <AppKit/AppKit.h>

#include <utility>

// `platform/win32/win32_foreground_monitor.h`의 macOS 구현이다. 앱이 활성 앱이 되는 순간(`NSApplicationDidBecomeActiveNotification`)을
// 듣는다. 다른 앱에서 돌아올 때만 오고, 앱 안의 창끼리(파일 패널, 보조 창) 오갈 때는 오지 않는다 — Win32 판과 같은 뜻이다.
// 알림은 메인 thread(luil의 UI thread)에서 온다.
namespace cyella::win32 {
    namespace {
        struct monitor_state
        {
            id observer { nil };
            foreground_monitor::activation_callback on_activated {};
        };

        monitor_state* active_state { nullptr };
    } // namespace

    std::unique_ptr<foreground_monitor> foreground_monitor::start(activation_callback on_activated)
    {
        if (active_state != nullptr)
            return nullptr;
        auto* const state { new monitor_state {} };
        state->on_activated = std::move(on_activated);
        state->observer = [[NSNotificationCenter defaultCenter] addObserverForName:NSApplicationDidBecomeActiveNotification
                                                                            object:nil
                                                                             queue:[NSOperationQueue mainQueue]
                                                                        usingBlock:^(NSNotification*) {
                                                                            if (active_state != nullptr && active_state->on_activated != nullptr)
                                                                                active_state->on_activated();
                                                                        }];
        if (state->observer == nil)
        {
            delete state;
            return nullptr;
        }
        active_state = state;
        return std::unique_ptr<foreground_monitor> { new foreground_monitor {} };
    }

    foreground_monitor::~foreground_monitor()
    {
        if (active_state == nullptr)
            return;
        [[NSNotificationCenter defaultCenter] removeObserver:active_state->observer];
        delete active_state;
        active_state = nullptr;
    }
} // namespace cyella::win32
