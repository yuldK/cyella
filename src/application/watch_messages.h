#pragma once

#include "application/repository_watcher.h"

#include <string>

namespace cyella {
    // 파일 감시와 창 초점이 logic thread에 올리는 앱 메시지다. 감시 스레드와 UI thread가 `operation_result_relay`로 보낸다.

    // 감시 스레드가 저장소의 변경을 알렸다. 저장소가 이미 닫혔거나 감시를 포기한 뒤에 도착한 것은 driver가 버린다.
    struct repository_changed_intent
    {
        std::string repository_id {};
        repository_change_kind kind { repository_change_kind::working_tree_changed };
    };

    // 다른 앱에서 cyella 창으로 돌아왔다 (이 프로세스의 창이 앞으로 나왔다). 선택된 탭을 다시 읽는 계기다.
    struct window_focus_intent
    {};
} // namespace cyella
