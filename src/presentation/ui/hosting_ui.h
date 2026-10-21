#pragma once

#include "presentation/hosting_view.h"

#include "luil/ui/ui_element.h"

#include <memory>

namespace cyella::ui {
    struct feedback_sources;
    // 사이드바에서 연 PR·MR의 상세 판이다. 제목·브랜치·상태·CI 요약·본문과 동작 단추([브라우저에서 열기], [로컬에 체크아웃], [CI 새로고침], 닫기)다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_pull_request_detail(const pull_request_detail_view& detail, bool drawer, float width, float scroll, feedback_sources& sources);

    // 커밋 상세의 CI 한 줄이다 (`✓ CI 통과 · GitHub`).
    [[nodiscard]] std::unique_ptr<luil::ui_element> build_commit_ci_line(const ci_mark_view& ci);
} // namespace cyella::ui
