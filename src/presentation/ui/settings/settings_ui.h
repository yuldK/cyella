#pragma once

#include "presentation/settings/settings_view.h"
#include "presentation/ui/ui_parts.h"

#include "luil/ui/ui_element.h"

#include <memory>

namespace cyella::ui {
    // 환경 설정 대화 상자다. 창 폭·높이(논리 픽셀)에 맞춰 크기를 정하고, 일반·diff 도구·SSH 쪽을 왼쪽 목록으로 고른다.
    // 오른쪽 내용은 흘리는 창이라 `sources.settings`로 최대 스크롤을 되돌려 준다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_settings_dialog(const settings_view& view, float window_width, float window_height, feedback_sources& sources);
} // namespace cyella::ui
