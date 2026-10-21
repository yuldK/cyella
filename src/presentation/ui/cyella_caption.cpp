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

    namespace {
        // 타이틀 그림 곁의 심볼은 캡션 높이의 7할이다. 16px 글리프 크기로는 캘리그라피 곁에서 너무 작다.
        constexpr float titled_symbol_share { 0.7f };
        // 그림 위아래에 장식의 여백이 있어 캡션 높이의 8할을 주면 글자가 제목 글꼴과 비슷한 키로 선다.
        constexpr float title_height_share { 0.8f };
    } // namespace

    shell_rect caption_icon_rect(const luil::caption_config& caption, const bool titled) noexcept
    {
        const float height { static_cast<float>(caption.metrics.height) };
        const float size { titled ? height * titled_symbol_share : static_cast<float>(caption.metrics.application_icon_size) };
        const float slot_width { static_cast<float>(caption.metrics.application_icon_slot_width) };
        return { (slot_width - size) * 0.5f, (height - size) * 0.5f, size, size };
    }

    shell_rect caption_title_rect(const luil::caption_config& caption, const float aspect) noexcept
    {
        const float height { static_cast<float>(caption.metrics.height) };
        const float title_height { height * title_height_share };
        // 가로 로고처럼 심볼 바로 곁에 붙인다. 아이콘 칸 끝이 아니라 심볼 오른쪽 끝에서 제목 간격만큼 띈다.
        const shell_rect symbol { caption_icon_rect(caption, true) };
        const float left { symbol.x + symbol.width + static_cast<float>(caption.metrics.title_icon_gap) };
        return { left, (height - title_height) * 0.5f, title_height * aspect, title_height };
    }
} // namespace cyella::ui
