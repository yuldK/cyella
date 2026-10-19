#pragma once

#include "application/askpass_service.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace cyella::win32 {
    struct askpass_server_start_result
    {
        // 시작하지 못하면 null이다.
        std::unique_ptr<askpass_service> server {};
        std::optional<std::uint32_t> native_error {};
    };

    // 앱 실행마다 하나 만든다.
    //
    // - 이름이 유일한 named pipe(`\\.\pipe\cyella-askpass-<pid>-<난수>`)를 연다. 원격 클라이언트는
    //   거부하고, 접근 권한(DACL)은 현재 사용자 SID 하나에만 주며, 첫 인스턴스 플래그로 같은 이름을
    //   먼저 차지한 다른 서버를 알아챈다.
    // - 토큰은 BCryptGenRandom으로 만든 128비트 난수다. 토큰이 틀린 요청은 핸들러를 부르지 않고
    //   바로 취소로 답한다.
    // - 질문마다 연결 스레드가 하나씩 생겨 여러 질문을 동시에 기다릴 수 있다.
    //
    // `handler`는 서버보다 오래 살아야 하는 것을 캡처하지 않거나, 서버를 먼저 `shutdown`해야 한다.
    [[nodiscard]] askpass_server_start_result start_askpass_server(askpass_prompt_handler handler);
} // namespace cyella::win32
