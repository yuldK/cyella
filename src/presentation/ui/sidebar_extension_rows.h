#pragma once

#include "presentation/view_snapshot.h"

#include "luil/ui/draw_primitives.h"

namespace cyella::ui {
    // 사이드바의 SUBMODULES·WORKTREES·GIT LFS 행을 그린다. 브랜치·태그 행이 아닌 `sidebar_item_kind` 행이다.
    // 머리 줄은 브랜치 구역과 같은 모양이라 `sidebar_row_element`가 그린다.
    void draw_extension_sidebar_row(luil::draw_context& context, const sidebar_row_view& row, const luil::rect_f& box);

    // 구역 머리의 아이콘이다 (`section_glyph`가 새 구역을 여기에 묻는다).
    [[nodiscard]] char32_t extension_section_glyph(sidebar_section section) noexcept;

    // 로컬 브랜치 행의 "다른 worktree에서 사용 중" 표지를 오른쪽 끝에 그린다. 그린 폭(논리 픽셀 아님, 물리 픽셀)을 돌려준다.
    float draw_worktree_marker(luil::draw_context& context, const luil::rect_f& box, float right);
} // namespace cyella::ui
