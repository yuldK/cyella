#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cyella::win32 {
    // 도우미(`cyella-askpass.exe`)와 앱이 named pipe로 주고받는 메시지의 부호화다. 질문 하나에 요청
    // 하나와 답 하나가 오가고 연결은 닫힌다. 모든 정수는 little-endian이다.
    //
    //   요청: "CYA1" | u32 토큰 길이 | u32 operation 길이 | u32 질문 길이 | 토큰 | operation | 질문
    //   답  : "CYR1" | u8 상태(0 취소, 1 답함) | u32 답 길이 | 답
    //
    // JSON 대신 길이 접두어를 쓴다. 도우미가 JSON 라이브러리를 링크하지 않아도 되고, 답(비밀)이
    // 라이브러리 내부 복사본으로 흩어지지 않아 한 버퍼를 지울 수 있다. 상한은 비정상 길이로 메모리를
    // 먹이는 연결을 일찍 끊기 위한 것이다.
    inline constexpr std::size_t askpass_maximum_token_bytes { 256u };
    inline constexpr std::size_t askpass_maximum_operation_bytes { 1024u };
    inline constexpr std::size_t askpass_maximum_prompt_bytes { 64u * 1024u };
    inline constexpr std::size_t askpass_maximum_answer_bytes { 64u * 1024u };
    inline constexpr std::size_t askpass_request_header_bytes { 16u };
    inline constexpr std::size_t askpass_reply_header_bytes { 9u };

    struct askpass_request_message
    {
        std::string token {};
        std::string operation_id {};
        std::string prompt {};
    };

    struct askpass_request_lengths
    {
        std::size_t token {};
        std::size_t operation {};
        std::size_t prompt {};

        [[nodiscard]] std::size_t body_bytes() const noexcept
        {
            return token + operation + prompt;
        }
    };

    struct askpass_reply_header
    {
        bool answered { false };
        std::size_t answer_bytes {};
    };

    // 상한을 넘는 필드가 있으면 값이 없다.
    [[nodiscard]] std::optional<std::string> encode_askpass_request(const askpass_request_message& message);
    // 정확히 `askpass_request_header_bytes`를 받아 길이를 읽는다. 매직이나 상한이 어긋나면 값이 없다.
    [[nodiscard]] std::optional<askpass_request_lengths> decode_askpass_request_header(std::string_view header);
    // 본문은 `lengths.body_bytes()`와 정확히 같은 크기여야 한다.
    [[nodiscard]] std::optional<askpass_request_message> decode_askpass_request_body(const askpass_request_lengths& lengths, std::string_view body);

    [[nodiscard]] std::string encode_askpass_reply(bool answered, std::string_view answer);
    [[nodiscard]] std::optional<askpass_reply_header> decode_askpass_reply_header(std::string_view header);
} // namespace cyella::win32
