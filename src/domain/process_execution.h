#pragma once

#include "domain/diagnostic.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    enum class process_stream
    {
        standard_output,
        standard_error,
    };

    enum class process_completion
    {
        exited,
        start_failed,
        timed_out,
        cancelled,
        invalid_request,
        // 프로세스는 시작했지만 대기, 출력 수집 또는 종료 코드 확인이 실패해 결과를
        // 신뢰할 수 없는 경우다. 이때 자식은 정리된다.
        internal_error,
    };

    // `lines` 모드의 출력 레코드는 줄 단위이며 작업 로그가 그대로 표시할 수 있는 UTF-8 및
    // 마스킹 완료 값이다. 이 모드에서는 원문 byte와 마스킹되지 않은 텍스트를 어떤 public
    // 값에도 보관하지 않는다.
    struct process_output_record
    {
        std::uint64_t sequence {};
        process_stream stream { process_stream::standard_output };
        std::u8string text {};
        // Git의 진행 표시처럼 단독 `\r`로 끝난 줄이다. 작업 로그 뷰가 접을 수 있다.
        bool progress { false };
        // 줄 끝을 만나지 못하고 레코드 크기 상한 때문에 끊긴 줄이다.
        bool continued { false };
        bool replaced_invalid_bytes { false };
        bool transcoded_from_active_code_page { false };
    };

    // `raw` 모드의 출력 조각이다. pipe에서 읽은 byte를 가공 없이 전달하므로 NUL, `\r`, 비UTF-8
    // byte가 그대로 들어 있고 마스킹도 하지 않는다. `bytes`는 sink 호출이 끝나면 무효가
    // 되므로 해석기는 호출 안에서 소비하거나 복사해 둬야 한다.
    struct process_output_chunk
    {
        std::uint64_t sequence {};
        process_stream stream { process_stream::standard_output };
        std::string_view bytes {};
    };

    struct process_result
    {
        process_completion completion { process_completion::invalid_request };
        // `exited`일 때만 값이 있다. timeout과 취소는 자식의 종료 코드를 신뢰하지 않는다.
        std::optional<std::int32_t> exit_code {};
        std::chrono::system_clock::time_point started_at {};
        std::chrono::system_clock::time_point finished_at {};
        std::chrono::milliseconds duration {};
        // `lines` 모드의 레코드와 `raw` 모드의 조각을 함께 센 sink 전달 횟수다.
        std::uint64_t record_count {};
        std::size_t captured_bytes {};
        bool output_truncated { false };
        std::optional<std::uint32_t> native_error {};
        // 로그와 재현에 사용하는 마스킹된 명령줄이다.
        std::u8string masked_command_line {};
        std::vector<diagnostic> diagnostics {};

        [[nodiscard]] bool succeeded() const noexcept;
        [[nodiscard]] bool has_errors() const noexcept;
        [[nodiscard]] bool has_warnings() const noexcept;
    };

    [[nodiscard]] std::u8string_view process_stream_name(process_stream stream) noexcept;
    [[nodiscard]] std::u8string_view process_completion_name(process_completion completion) noexcept;
} // namespace cyella
