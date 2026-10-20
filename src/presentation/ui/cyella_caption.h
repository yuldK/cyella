#pragma once

#include "presentation/shell_layout.h"

#include "luil/ui/caption_element.h"
#include "luil/ui/image_element.h"

namespace cyella::ui {
    // 캡션 왼쪽 앱 아이콘 그림이다 (assets/cyella.svg에서 만든 PNG). luil 캡션은 codicon 글리프만 그리므로, 그림이
    // 있으면 글리프를 끄고 같은 자리에 image element를 얹는다. 창을 만들기 전에 한 번 정하고 그 뒤로는 읽기만 한다.
    // 정하지 않으면(그림 resource를 싣지 않은 test 등) 예전 codicon 글리프로 그린다.
    void set_caption_icon(luil::ui_image image);
    [[nodiscard]] const luil::ui_image& caption_icon() noexcept;

    // 창 caption의 겉모습이다. 두 곳이 같은 값을 읽는다: `window_config::caption`
    // (비클라이언트 hit test·창 제목)과 tree의 `luil::caption_element`(그리기·툴팁).
    // 값이 갈리면 버튼 자리가 어긋나므로 한곳에서만 만든다.
    [[nodiscard]] luil::caption_config cyella_caption();

    // 캡션 아이콘 그림이 놓일 자리다 (논리 px). 글리프가 서던 아이콘 칸의 가운데다.
    [[nodiscard]] shell_rect caption_icon_rect(const luil::caption_config& caption) noexcept;
} // namespace cyella::ui
