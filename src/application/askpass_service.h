#pragma once

#include "domain/credential_prompt.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace cyella {
    // 도우미가 앱을 찾아가는 데 필요한 값이다. 실행 환경 변수 `CYELLA_ASKPASS_PIPE`와
    // `CYELLA_ASKPASS_TOKEN`으로 자식에게 건넨다. 토큰은 앱 실행마다 새로 만든 비밀이다.
    struct askpass_endpoint
    {
        std::u8string pipe_name {};
        std::u8string token {};
    };

    // 도우미가 앱에 보낸 질문 하나다.
    struct askpass_prompt_request
    {
        // 서버가 붙인 번호다. 대화 상자와 답을 짝짓는 데 쓴다.
        std::uint64_t prompt_id {};
        // 질문을 낸 operation이다. 자식의 `CYELLA_ASKPASS_OPERATION` 값이다.
        std::u8string operation_id {};
        credential_prompt prompt {};
    };

    // 사용자의 답이다. 빈 답도 유효한 답이다 (`ssh-keygen`의 "passphrase 없음"). 취소와는 `answered`로
    // 구분하므로 길이로 취소를 추측하지 않는다.
    struct askpass_reply
    {
        bool answered { false };
        std::u8string answer {};

        [[nodiscard]] static askpass_reply cancelled()
        {
            return {};
        }

        [[nodiscard]] static askpass_reply with_answer(std::u8string text)
        {
            return askpass_reply { true, std::move(text) };
        }
    };

    // 어느 스레드에서든 불러도 된다. 처음 한 번만 효과가 있고 이후 호출, 서버가 끝난 뒤의 호출,
    // 도우미가 이미 사라진 뒤의 호출은 조용히 무시된다. 핸들러가 이 함수를 보관하고 늦게 불러도
    // 안전하다.
    using askpass_reply_callback = std::function<void(askpass_reply)>;

    // 질문이 도착할 때 서버의 연결 스레드에서 불린다. 여러 질문이 동시에 올 수 있으므로 여러
    // 스레드에서 동시에 불릴 수 있다. 오래 걸리면 안 되며, 보통 logic thread에 게시하고 바로
    // 돌아와 나중에 `askpass_reply_callback`으로 답한다. 예외를 던지면 그 질문은 취소로 답한다.
    using askpass_prompt_handler = std::function<void(askpass_prompt_request, askpass_reply_callback)>;

    // 앱 안에서 도우미의 질문을 받는 서버의 계약이다. 구현은 OS 종속(Win32 named pipe)이다.
    class askpass_service
    {
    public:
        askpass_service() = default;
        askpass_service(const askpass_service&) = delete;
        askpass_service(askpass_service&&) = delete;
        askpass_service& operator=(const askpass_service&) = delete;
        askpass_service& operator=(askpass_service&&) = delete;
        virtual ~askpass_service() = default;

        [[nodiscard]] virtual askpass_endpoint endpoint() const = 0;

        // 해당 operation에서 대기 중인 질문 전부에 취소로 답하고, 이후 같은 operation의 질문도
        // 즉시 취소한다. operation이 취소되거나 끝날 때 부른다.
        virtual void cancel_operation(std::u8string_view operation_id) = 0;

        // 대기 중인 질문 전부를 취소로 답하고 listener와 연결 스레드를 멈춘다. 반복 호출해도
        // 안전하다. 소멸자도 부른다.
        virtual void shutdown() = 0;
    };
} // namespace cyella
