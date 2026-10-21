#pragma once

#include "presentation/shell_layout.h"

#include "luil/ui/caption_element.h"

namespace cyella::ui {
    // 창 caption의 겉모습이다. 두 곳이 같은 값을 읽는다: `window_config::caption`
    // (비클라이언트 hit test·창 제목)과 tree의 `luil::caption_element`(그리기·툴팁).
    // 값이 갈리면 버튼 자리가 어긋나므로 한곳에서만 만든다.
    [[nodiscard]] luil::caption_config cyella_caption();

    // 캡션 왼쪽에 앱 심볼(`app_symbol`)이 놓일 자리다 (논리 px). 글리프가 서던 아이콘 칸의 가운데다.
    // luil 캡션은 codicon 글리프만 그리므로, 심볼이 있으면 `cyella_caption`이 글리프를 끄고 tree가 이 자리에 심볼을 얹는다.
    // `titled`이면 타이틀 그림 곁에 서는 큰 심볼의 자리다.
    [[nodiscard]] shell_rect caption_icon_rect(const luil::caption_config& caption, bool titled = false) noexcept;

    // 캡션의 제목 자리에 놓는 타이틀 캘리그라피의 자리다 (논리 픽셀). 큰 심볼 바로 오른쪽에서 시작하고 캡션 높이의
    // 대부분을 쓰며, 폭은 그림의 비율(`aspect` = 폭 ÷ 높이)로 정한다.
    [[nodiscard]] shell_rect caption_title_rect(const luil::caption_config& caption, float aspect) noexcept;
} // namespace cyella::ui
