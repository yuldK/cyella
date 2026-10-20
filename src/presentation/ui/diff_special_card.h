#pragma once

#include "presentation/view_snapshot.h"

#include "luil/ui/ui_element.h"

#include <memory>
#include <string>

namespace cyella::ui {
    // 텍스트 diff로 보일 수 없는 변경(LFS 포인터, submodule 포인터)의 본문 카드다. 웹뷰 자리에 서며 luil이 그리므로
    // UI Automation에 이름으로 보인다. `repository_id`는 카드의 단추가 내는 operation의 저장소다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_diff_special_card(const diff_special_view& special, const std::string& repository_id);
} // namespace cyella::ui
