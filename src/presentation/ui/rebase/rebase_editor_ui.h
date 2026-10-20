#pragma once

#include "presentation/rebase/rebase_editor_view.h"

#include "luil/ui/ui_element.h"

#include <memory>

namespace cyella::ui {
    // 인터랙티브 rebase 편집기 대화 상자다. 화면 전체를 덮고 가운데에 놓는다. 크기는 논리 픽셀이다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_rebase_editor(const rebase_editor_view& view, float logical_width, float logical_height);
} // namespace cyella::ui
