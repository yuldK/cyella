#pragma once

#include "domain/diagnostic.h"
#include "domain/path_syntax.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 줄 끝을 만나지 못한 레코드를 강제로 끊는 기본 크기다. 진행 표시가 없는 도구가
    // 한 줄로 대량 출력해도 로그 레코드 하나가 무한히 커지지 않게 한다.
    inline constexpr std::size_t default_process_record_byte_limit { 8u * 1024u };
    // 강제 분할이 UTF-8 sequence 하나를 쪼개지 않도록 최소 상한을 둔다.
    inline constexpr std::size_t minimum_process_record_byte_limit { 4u };
    // pipe에서 한 번에 읽는 크기다. 값이 너무 작으면 대용량 출력에서 호출이 늘고
    // 너무 크면 첫 레코드 지연이 커진다. `raw` 모드의 조각 상한이기도 하다.
    inline constexpr std::size_t process_read_block_byte_size { 64u * 1024u };
    // 캡처 상한이 필요 없는 스트리밍 소비자를 위한 값이다. 검증은 0만 거부한다.
    inline constexpr std::size_t unlimited_process_capture_bytes { static_cast<std::size_t>(-1) };

    enum class process_text_encoding
    {
        // 출력을 UTF-8로만 해석하고 잘못된 byte는 U+FFFD로 대체한다.
        utf8,
        // UTF-8로 해석되지 않는 레코드만 활성 code page에서 변환한다.
        active_code_page_fallback,
    };

    // 스트림 하나를 sink에 넘기는 방식이다.
    enum class process_output_mode
    {
        // 줄 단위 레코드로 나누고 마스킹·UTF-8 정규화를 거쳐 `on_record`로 전달한다.
        lines,
        // pipe의 byte를 그대로 `on_chunk`로 전달한다. 줄 나누기, `\r` 해석, 레코드 크기
        // 분할, 인코딩 변환과 마스킹을 모두 건너뛴다. `git log -z`처럼 NUL로 구분하거나
        // 바이너리가 섞이는 출력을 스트리밍 해석기가 점진적으로 소비하는 용도다.
        raw,
    };

    struct process_environment_override
    {
        std::u8string name {};
        // 값이 없으면 부모 환경에서 해당 변수를 삭제한다.
        std::optional<std::u8string> value {};
    };

    struct process_request
    {
        // 절대 경로만 허용한다. PATH 탐색과 확장자 추론을 하지 않으므로 실행 파일
        // 발견은 호출자(`git_tool_discovery`)가 담당한다.
        std::u8string executable {};
        // argv[1] 이후의 인자다. 셸을 사용하지 않으므로 metacharacter는 값의 일부다.
        std::vector<std::u8string> arguments {};
        std::u8string working_directory {};
        std::vector<process_environment_override> environment_overrides {};
        // 자식의 표준 입력으로 쓸 byte다. 쓰기를 마치면 pipe를 닫아 자식이 EOF를 본다.
        // 비어 있으면 NUL 장치를 연결해 자식이 입력을 읽으려 해도 즉시 EOF가 되게 한다.
        // 마스킹되지 않은 값이므로 명령줄과 달리 어떤 결과에도 되돌려 주지 않는다.
        std::string standard_input {};
        // 값이 없으면 무제한이며, 취소만으로 종료를 제어한다.
        std::optional<std::chrono::milliseconds> timeout {};
        // 스트림별 캡처 상한이다. 상한을 넘으면 이후 내용을 버리지만 pipe는 계속 비운다.
        // 스트리밍으로 소비하는 요청은 `unlimited_process_capture_bytes`를 쓴다.
        std::size_t maximum_captured_bytes_per_stream {};
        std::size_t maximum_record_bytes { default_process_record_byte_limit };
        process_text_encoding text_encoding { process_text_encoding::utf8 };
        // 스트림마다 따로 정한다. 흔한 조합은 stdout만 `raw`로 두고 진행 표시가 오는
        // stderr는 `lines`로 두는 것이다.
        process_output_mode standard_output_mode { process_output_mode::lines };
        process_output_mode standard_error_mode { process_output_mode::lines };
    };

    // 요청 값만으로 판정할 수 있는 오류를 반환한다. filesystem 조회는 하지 않는다.
    [[nodiscard]] std::vector<diagnostic> validate_process_request(const process_request& request);
    [[nodiscard]] std::u8string_view process_text_encoding_name(process_text_encoding encoding) noexcept;
    [[nodiscard]] std::u8string_view process_output_mode_name(process_output_mode mode) noexcept;
} // namespace cyella
