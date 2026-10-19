#pragma once

#include "luil/luil.h"

namespace cyella::win32 {
    // luil UI thread에 꽂는 cyella 훅이다. 창 사건을 앱 메시지로 바꾸기만 하고
    // 상태는 직접 고치지 않는다.
    class cyella_delegate final : public luil::win32::window_delegate
    {
    public:
        [[nodiscard]] luil::app_message make_window_metrics_message(float width, float height, float scale) override;
    };
} // namespace cyella::win32
