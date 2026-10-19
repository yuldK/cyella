#include "platform/win32/cyella_delegate.h"

#include "application/app_messages.h"

namespace cyella::win32 {
    void cyella_delegate::on_started(luil::app_host& host)
    {
        // 묶이기 전에 도착한 결과가 있으면 bind가 한 번 깨운다.
        if (relay_ != nullptr)
            relay_->bind(host);
    }

    luil::app_message cyella_delegate::make_window_metrics_message(const float width, const float height, const float scale)
    {
        return luil::app_message { window_metrics_intent { width, height, scale } };
    }
} // namespace cyella::win32
