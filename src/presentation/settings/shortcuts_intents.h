#pragma once

#include "presentation/commands/key_chord.h"

#include <string>

namespace cyella {
    // 환경 설정 "단축키" 쪽의 화면 메시지다. 저장하는 값(`app_settings::shortcuts`)은 `app_settings_service`를 거친다.

    // 명령 하나의 단축키를 고치기 시작한다.
    struct edit_shortcut_intent
    {
        std::string command {};
    };

    // 고치는 중인 단축키를 적용한다. 읽을 수 없거나 예약된 키이거나 다른 명령이 쓰는 키면 적용하지 않는다.
    struct apply_shortcut_intent
    {};

    struct cancel_shortcut_edit_intent
    {};

    // 명령의 단축키를 기본값으로 되돌린다.
    struct reset_shortcut_intent
    {
        std::string command {};
    };

    struct reset_all_shortcuts_intent
    {};

    // 입력 칸에 초점이 있을 때 눌린 키다. 칸의 글이 그 단축키로 바뀐다 (입력 thread의 정책이 키 이벤트를 이 메시지로 바꾼다).
    struct capture_shortcut_intent
    {
        key_chord chord {};
    };
} // namespace cyella
