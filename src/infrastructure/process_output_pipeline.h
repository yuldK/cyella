#pragma once

#include "application/process_request.h"
#include "application/text_transcoder.h"
#include "domain/process_execution.h"

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace cyella {
    // 한 스트림의 raw byte를 줄 단위 레코드로 바꾼다. reader 스레드 하나가 단독으로
    // 소유하므로 내부에 잠금을 두지 않는다. sequence 부여와 sink 호출은 runner가 한다.
    class process_output_pipeline
    {
    public:
        using record_handler = std::function<void(process_output_record&)>;

        // `transcoder`는 `active_code_page_fallback`일 때만 사용하며 파이프라인보다 오래
        // 살아 있어야 한다. 값이 없으면 UTF-8 해석과 U+FFFD 대체만 수행한다.
        process_output_pipeline(process_stream stream, std::size_t maximum_record_bytes, std::size_t maximum_captured_bytes, process_text_encoding encoding = process_text_encoding::utf8,
            text_transcoder* transcoder = nullptr);
        process_output_pipeline(const process_output_pipeline&) = delete;
        process_output_pipeline(process_output_pipeline&&) = delete;
        process_output_pipeline& operator=(const process_output_pipeline&) = delete;
        process_output_pipeline& operator=(process_output_pipeline&&) = delete;
        ~process_output_pipeline() = default;

        // 읽은 byte 조각을 넣는다. 줄이 완성되면 그때마다 handler를 호출한다.
        void append(std::u8string_view bytes, const record_handler& handler);
        // 프로세스가 끝난 뒤 남은 미완결 tail을 마지막 레코드로 내보낸다.
        void flush(const record_handler& handler);

        [[nodiscard]] std::size_t captured_bytes() const noexcept;
        [[nodiscard]] bool truncated() const noexcept;

    private:
        void emit(bool progress, bool continued, const record_handler& handler);
        void consume(std::u8string_view bytes, const record_handler& handler);
        void fill_text(process_output_record& record);

        process_stream stream_ { process_stream::standard_output };
        process_text_encoding encoding_ { process_text_encoding::utf8 };
        text_transcoder* transcoder_ { nullptr };
        std::size_t maximum_record_bytes_ {};
        std::size_t maximum_captured_bytes_ {};
        std::size_t captured_bytes_ {};
        bool truncated_ { false };
        // 직전 byte가 `\r`이면 다음 byte를 봐야 `\r\n`과 진행 표시를 구분할 수 있다.
        bool pending_carriage_return_ { false };
        std::u8string pending_ {};
    };

    // 유효하지 않은 byte를 U+FFFD로 대체한 UTF-8 문자열을 만든다.
    [[nodiscard]] std::u8string normalize_utf8_text(std::u8string_view input, bool& replaced_invalid_bytes);
    // overlong, surrogate와 U+10FFFF 초과 표현까지 거부하는 UTF-8 유효성 검사다.
    [[nodiscard]] bool is_valid_utf8_text(std::u8string_view input) noexcept;
} // namespace cyella
