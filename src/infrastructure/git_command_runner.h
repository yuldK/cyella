#pragma once

#include "application/process_cancellation.h"
#include "application/process_request.h"
#include "application/process_runner.h"
#include "domain/process_execution.h"

#include <string>
#include <vector>

namespace cyella {
    // `lines` 모드로 모은 실행 결과다. `raw` 스트림은 이 수집기가 받지 않으므로 해당
    // 스트림의 줄 목록은 비어 있다. 조각을 직접 소비하는 호출자는 `process_runner`를 쓴다.
    struct git_command_result
    {
        process_result process {};
        std::vector<std::u8string> standard_output_lines {};
        std::vector<std::u8string> standard_error_lines {};

        // 명령이 정상 종료하고 종료 코드가 0인 경우에만 참이다.
        [[nodiscard]] bool succeeded() const noexcept;
        [[nodiscard]] std::u8string standard_output_text() const;
        [[nodiscard]] std::u8string standard_error_text() const;
        // 값 하나만 출력하는 명령(`--version`, `rev-parse`)의 결과를 꺼낸다.
        [[nodiscard]] std::u8string first_output_line() const;
    };

    // 두 스트림을 분리해 모으면서 호출자가 준 sink에도 그대로 전달한다. 작업 로그가 이 sink
    // 자리에 들어간다.
    class git_output_collector final : public process_output_sink
    {
    public:
        git_output_collector() = default;
        explicit git_output_collector(process_output_sink* forward) noexcept;

        void on_record(const process_output_record& record) override;
        void on_chunk(const process_output_chunk& chunk) override;

        [[nodiscard]] std::vector<std::u8string> take_standard_output() noexcept;
        [[nodiscard]] std::vector<std::u8string> take_standard_error() noexcept;

    private:
        process_output_sink* forward_ { nullptr };
        std::vector<std::u8string> standard_output_ {};
        std::vector<std::u8string> standard_error_ {};
    };

    // 요청을 실행하고 출력을 스트림별로 모은다. 예외를 던지지 않는다.
    [[nodiscard]] git_command_result run_git_command(process_runner& runner, const process_request& request, const process_cancellation_token& token, process_output_sink* forward = nullptr) noexcept;
} // namespace cyella
