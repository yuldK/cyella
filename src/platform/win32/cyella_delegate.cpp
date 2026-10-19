#include "platform/win32/cyella_delegate.h"

#include "application/app_messages.h"

namespace cyella::win32 {
    luil::app_message cyella_delegate::make_window_metrics_message(const float width, const float height, const float scale)
    {
        return luil::app_message { window_metrics_intent { width, height, scale } };
    }
} // namespace cyella::win32
