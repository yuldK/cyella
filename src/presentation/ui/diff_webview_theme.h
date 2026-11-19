#pragma once

#include "application/diff_view_protocol.h"

#include "luil/theme/ui_theme.h"

#include <string>

namespace cyella::ui {
    // `#rrggbb`, 알파가 불투명이 아니면 `#rrggbbaa`다 (CSS 색). luil 색은 `0xAARRGGBB`다.
    [[nodiscard]] std::string css_color(luil::ui_color color);

    // luil 팔레트를 diff 뷰어의 CSS 변수(`assets/diff-viewer/viewer.css`의 `:root` 토큰 이름)로 옮긴다. 뷰어는 이 값으로
    // 내장 색을 덮어쓰므로 앱 테마가 바뀌면 `diff_theme`만 다시 보내면 페이지가 같은 색으로 다시 칠해진다. luil 팔레트에 없는
    // 성공 색, 레인 색, diff 줄 바탕은 앱 색(`ui_helpers`)을 쓴다.
    [[nodiscard]] diff_theme make_diff_theme(const luil::ui_color_palette& palette, std::u8string_view ui_family = {}, std::u8string_view code_family = {});

    // luil 표면이 지금 칠하는 팔레트다. 웹뷰가 창과 같은 색이려면 luil이 푸는 순서(`frame_palette`)를 그대로 따라야 한다.
    //  - 테마는 설정의 선호와 OS의 고대비·밝은 모드로 고른다. 고대비이면 키 컬러를 버리고 OS가 준 대비 색으로 합성한다.
    //  - cyella는 frame에 스타일(`ui_frame::style`)을 싣지 않으므로 나머지 테마는 내장 스타일 위에 키 컬러를 얹은 것이다.
    [[nodiscard]] luil::ui_color_palette surface_palette_for(luil::theme_preference preference, const luil::system_appearance& system, std::u8string_view accent_id);
} // namespace cyella::ui
