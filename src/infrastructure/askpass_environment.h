#pragma once

#include "application/process_request.h"

#include <string>
#include <vector>

namespace cyella {
    // askpass 도우미가 앱에 접속하는 데 쓰는 세 값이다. 자식 프로세스의 환경 변수로 건넨다.
    struct askpass_session
    {
        // `CYELLA_ASKPASS_PIPE`: 앱이 연 named pipe의 전체 이름이다.
        std::u8string pipe_name {};
        // `CYELLA_ASKPASS_TOKEN`: 앱 실행마다 새로 만든 비밀이다. 로그에 남기지 않는다.
        std::u8string token {};
        // `CYELLA_ASKPASS_OPERATION`: 질문을 낸 operation의 id다. 취소할 때 대기 질문을 찾는 키다.
        std::u8string operation_id {};
    };

    // `CYELLA_ASKPASS_*` 세 변수의 override다. 도우미가 있으면 값이 있는 변수만 설정하고 나머지는
    // 지운다. 도우미가 없으면 모두 지워 사용자 환경의 값이 새지 않게 한다.
    [[nodiscard]] std::vector<process_environment_override> askpass_session_overrides(bool helper_present, const askpass_session& session);
} // namespace cyella
