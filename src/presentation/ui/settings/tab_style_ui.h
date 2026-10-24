#pragma once

#include "presentation/settings/settings_view.h"

#include "luil/ui/ui_element.h"

#include <cstddef>
#include <memory>

namespace cyella::ui {
    // 탭 줄 모양을 고르는 견본 카드 묶음이다 (이어진 탭·평평한 칸). 카드마다 그 모양의 견본 막대가 선다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> make_tab_shape_picker(const settings_view& view);
    // 탭 상태 표시 자리를 고르는 견본 카드 묶음이다 (이름 뒤 배지·아이콘 모서리·닫기 자리). 견본은 지금 고른 모양으로 그린다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> make_tab_marker_picker(const settings_view& view);
    // 카드 `count`장의 묶음 높이다 (논리 픽셀).
    [[nodiscard]] float tab_style_picker_height(std::size_t count) noexcept;

    inline constexpr std::size_t tab_shape_choices { 2 };
    inline constexpr std::size_t tab_marker_choices { 3 };
} // namespace cyella::ui
