#pragma once

#include <string>

namespace cyella {
    // 환경 설정 "AI" 쪽의 메시지다 (ADR-009). 처리는 `settings_controller_suggestion.cpp`가 한다.

    // 커밋 상자의 제안 단추를 켜고 끈다. 바로 저장한다.
    struct set_commit_suggestion_enabled_intent
    {
        bool on { false };
    };

    // 본보기(`commit_suggestion_presets`)로 실행 파일·인자 칸을 채운다. 저장은 [저장]을 눌러야 한다.
    struct fill_commit_suggestion_preset_intent
    {
        std::u8string id {};
    };

    // 실행 파일·인자·추가 지시 칸의 초안을 저장한다.
    struct save_commit_suggestion_intent
    {};
} // namespace cyella
