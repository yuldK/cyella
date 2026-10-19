#include "platform/win32/askpass_protocol.h"

namespace cyella::win32 {
    namespace {
        constexpr std::string_view request_magic { "CYA1" };
        constexpr std::string_view reply_magic { "CYR1" };

        void append_u32(std::string& output, const std::size_t value)
        {
            for (std::size_t shift { 0 }; shift < 32u; shift += 8u)
                output.push_back(static_cast<char>((value >> shift) & 0xffu));
        }

        std::size_t read_u32(const std::string_view bytes, const std::size_t offset) noexcept
        {
            std::size_t value { 0 };
            for (std::size_t index { 0 }; index < 4u; ++index)
                value |= static_cast<std::size_t>(static_cast<unsigned char>(bytes[offset + index])) << (index * 8u);
            return value;
        }
    } // namespace

    std::optional<std::string> encode_askpass_request(const askpass_request_message& message)
    {
        if (message.token.size() > askpass_maximum_token_bytes || message.operation_id.size() > askpass_maximum_operation_bytes || message.prompt.size() > askpass_maximum_prompt_bytes)
            return std::nullopt;

        std::string output {};
        output.reserve(askpass_request_header_bytes + message.token.size() + message.operation_id.size() + message.prompt.size());
        output.append(request_magic);
        append_u32(output, message.token.size());
        append_u32(output, message.operation_id.size());
        append_u32(output, message.prompt.size());
        output.append(message.token);
        output.append(message.operation_id);
        output.append(message.prompt);
        return output;
    }

    std::optional<askpass_request_lengths> decode_askpass_request_header(const std::string_view header)
    {
        if (header.size() != askpass_request_header_bytes || header.substr(0, request_magic.size()) != request_magic)
            return std::nullopt;

        askpass_request_lengths lengths {};
        lengths.token = read_u32(header, 4u);
        lengths.operation = read_u32(header, 8u);
        lengths.prompt = read_u32(header, 12u);
        if (lengths.token > askpass_maximum_token_bytes || lengths.operation > askpass_maximum_operation_bytes || lengths.prompt > askpass_maximum_prompt_bytes)
            return std::nullopt;
        return lengths;
    }

    std::optional<askpass_request_message> decode_askpass_request_body(const askpass_request_lengths& lengths, const std::string_view body)
    {
        if (body.size() != lengths.body_bytes())
            return std::nullopt;

        askpass_request_message message {};
        message.token = std::string { body.substr(0, lengths.token) };
        message.operation_id = std::string { body.substr(lengths.token, lengths.operation) };
        message.prompt = std::string { body.substr(lengths.token + lengths.operation, lengths.prompt) };
        return message;
    }

    std::string encode_askpass_reply(const bool answered, const std::string_view answer)
    {
        // 취소 답에 내용이 실리지 않도록 길이를 0으로 만든다.
        const std::string_view payload { answered ? answer : std::string_view {} };

        std::string output {};
        output.reserve(askpass_reply_header_bytes + payload.size());
        output.append(reply_magic);
        output.push_back(answered ? '\1' : '\0');
        append_u32(output, payload.size());
        output.append(payload);
        return output;
    }

    std::optional<askpass_reply_header> decode_askpass_reply_header(const std::string_view header)
    {
        if (header.size() != askpass_reply_header_bytes || header.substr(0, reply_magic.size()) != reply_magic)
            return std::nullopt;

        const char status { header[4] };
        if (status != '\0' && status != '\1')
            return std::nullopt;

        askpass_reply_header decoded {};
        decoded.answered = status == '\1';
        decoded.answer_bytes = read_u32(header, 5u);
        if (decoded.answer_bytes > askpass_maximum_answer_bytes || (decoded.answered == false && decoded.answer_bytes != 0))
            return std::nullopt;
        return decoded;
    }
} // namespace cyella::win32
