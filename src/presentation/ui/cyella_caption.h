#pragma once

#include "luil/generated/codicons.h"
#include "luil/ui/caption_element.h"

namespace cyella::ui {
    // 창 caption의 겉모습이다. 두 곳이 같은 값을 읽는다: `window_config::caption`
    // (비클라이언트 hit test·창 제목)과 tree의 `luil::caption_element`(그리기·툴팁).
    // 값이 갈리면 버튼 자리가 어긋나므로 한곳에서만 만든다.
    [[nodiscard]] inline luil::caption_config cyella_caption()
    {
        luil::caption_config caption {};
        caption.title = u8"cyella";
        caption.application_icon = luil::codicons::icon_source_control;
        caption.minimize_tooltip = u8"최소화";
        caption.maximize_tooltip = u8"최대화 또는 이전 크기로 복원";
        caption.close_tooltip = u8"닫기";
        return caption;
    }
} // namespace cyella::ui
