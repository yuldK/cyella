#pragma once

#include "presentation/settings/signing_page_view.h"
#include "presentation/ui/settings/settings_page_parts.h"

namespace cyella::ui {
    // 환경 설정의 "서명" 쪽을 `page`에 쌓는다. 바꿀 범위(이 저장소·전역), 지금 적용되는 설정 표, 서명 켜기·형식·키·프로그램 경로 입력과
    // 전역 변경의 확인 상자다. 모든 값은 `view`에서 오고 단추는 `presentation/settings/signing_intents.h`의 메시지를 낸다.
    void build_signing_page(pages::page_column& page, const signing_page_view& view);
} // namespace cyella::ui
