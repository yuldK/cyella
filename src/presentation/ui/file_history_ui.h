#pragma once

#include "presentation/view_snapshot.h"

#include "luil/ui/ui_element.h"

#include <memory>

namespace cyella::ui {
    struct feedback_sources;

    // diff 판의 이력 보기에서 왼쪽에 서는 커밋 목록이다. 읽는 중이거나 읽지 못했으면 그 글이 대신 선다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_file_history_list(const file_history_view& history, feedback_sources& sources);
} // namespace cyella::ui
