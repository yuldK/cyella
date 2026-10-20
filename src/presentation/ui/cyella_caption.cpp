#include "presentation/ui/cyella_caption.h"

#include "presentation/ui/app_symbol.h"

#include "luil/generated/codicons.h"

namespace cyella::ui {
    luil::caption_config cyella_caption()
    {
        luil::caption_config caption {};
        caption.title = u8"cyella";
        if (app_symbol().valid())
        {
            // 글리프를 끄면 luil은 제목을 왼쪽 여백에서 시작한다. 아이콘이 있을 때와 같은 자리에 오도록 여백을 칸 폭만큼 민다.
            // 아이콘 칸의 시스템 메뉴 판정은 칸 폭만 보므로 그대로다.
            caption.metrics.title_left_padding = caption.metrics.application_icon_slot_width + caption.metrics.title_icon_gap;
        }
        else
            caption.application_icon = luil::codicons::icon_source_control;
        caption.minimize_tooltip = u8"최소화";
        caption.maximize_tooltip = u8"최대화 또는 이전 크기로 복원";
        caption.close_tooltip = u8"닫기";
        return caption;
    }

    shell_rect caption_icon_rect(const luil::caption_config& caption) noexcept
    {
        const float size { static_cast<float>(caption.metrics.application_icon_size) };
        const float slot_width { static_cast<float>(caption.metrics.application_icon_slot_width) };
        const float height { static_cast<float>(caption.metrics.height) };
        return { (slot_width - size) * 0.5f, (height - size) * 0.5f, size, size };
    }
} // namespace cyella::ui
