#pragma once

#include "application/app_messages.h"

#include "luil/luil.h"

#include <atomic>
#include <memory>

namespace cyella::win32 {
    // luil logic thread에 꽂는 cyella 구동기다. 앱 상태를 소유하고 메시지를 처리해
    // 불변 frame을 게시한다. `handle`·`make_frame`은 logic thread에서만 불리고,
    // `make_close_message`·`shutdown_completed`는 UI thread도 부르므로 atomic으로 지킨다.
    class cyella_driver final : public luil::win32::logic_driver
    {
    public:
        void handle(luil::app_message message) override;
        [[nodiscard]] std::shared_ptr<const luil::win32::ui_frame> make_frame() override;
        [[nodiscard]] luil::app_message make_close_message() override;
        [[nodiscard]] bool shutdown_completed() const override;

    private:
        // 창의 실제 크기·배율이다. 첫 frame(메시지가 오기 전)은 기본값으로 배치한다.
        window_metrics_intent metrics_ {};
        std::atomic<bool> closed_ { false };
    };
} // namespace cyella::win32
