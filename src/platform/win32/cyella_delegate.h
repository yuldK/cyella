#pragma once

#include "platform/win32/operation_result_relay.h"

#include "luil/luil.h"

namespace cyella::win32 {
    // luil UI thread에 꽂는 cyella 훅이다. 창 사건을 앱 메시지로 바꾸기만 하고
    // 상태는 직접 고치지 않는다.
    class cyella_delegate final : public luil::win32::window_delegate
    {
    public:
        // relay는 driver가 소유한다. host가 준비되면 묶어 worker 결과가 logic thread를 깨우게 한다.
        explicit cyella_delegate(operation_result_relay* relay = nullptr) noexcept
            : relay_ { relay }
        {}

        void on_started(luil::app_host& host) override;
        [[nodiscard]] luil::app_message make_window_metrics_message(float width, float height, float scale) override;

    private:
        operation_result_relay* relay_ { nullptr };
    };
} // namespace cyella::win32
