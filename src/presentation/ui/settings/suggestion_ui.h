#pragma once

#include "presentation/settings/suggestion_page.h"
#include "presentation/ui/settings/settings_page_parts.h"

namespace cyella::ui {
    // 환경 설정의 "AI" 쪽을 `page`에 쌓는다 (ADR-009). 커밋 메시지 제안을 켜는 토글과 무엇이 밖으로 나가는지의 설명, 제안 명령의
    // 본보기 단추·실행 파일·인자·추가 지시 입력이다. 모든 값은 `view`에서 오고 단추는 `presentation/settings/suggestion_intents.h`의 메시지를 낸다.
    void build_suggestion_page(pages::page_column& page, const suggestion_page_view& view, float content_width);
} // namespace cyella::ui
