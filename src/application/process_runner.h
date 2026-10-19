#pragma once

#include "application/process_cancellation.h"
#include "application/process_request.h"
#include "domain/process_execution.h"

namespace cyella {
    // runner가 내부 mutex로 호출을 직렬화하므로 sink 구현은 스스로 동기화하지 않아도
    // 된다. 두 스트림의 출력이 서로 다른 스레드에서 도착하더라도 한 번에 하나만 불린다.
    // 대신 sink 안에서 오래 기다리면 자식 프로세스가 backpressure를 받는다.
    //
    // 스트림이 `lines` 모드면 `on_record`, `raw` 모드면 `on_chunk`가 불린다. 둘 다
    // 기본 구현은 아무 일도 하지 않아, 한쪽만 쓰는 sink는 다른 쪽을 구현하지 않는다.
    class process_output_sink
    {
    public:
        process_output_sink() = default;
        process_output_sink(const process_output_sink&) = delete;
        process_output_sink(process_output_sink&&) = delete;
        process_output_sink& operator=(const process_output_sink&) = delete;
        process_output_sink& operator=(process_output_sink&&) = delete;
        virtual ~process_output_sink() = default;

        virtual void on_record(const process_output_record& record)
        {
            static_cast<void>(record);
        }

        // 조각의 크기와 경계는 pipe 읽기에 따라 정해지므로 의미 단위(NUL, 줄)와 맞지
        // 않는다. 해석기는 조각 사이에 걸친 토큰을 스스로 이어 붙여야 한다.
        virtual void on_chunk(const process_output_chunk& chunk)
        {
            static_cast<void>(chunk);
        }
    };

    // 실행 계약은 호출 스레드를 점유하는 동기 API 하나다. lane 배정, 동시 실행 상한과
    // 결과 전달 경로는 `task_scheduler`와 `operation_result_relay`가 맡는다.
    // 하나의 구현 instance는 여러 스레드에서 동시에 `run`을 호출해도 안전해야 한다.
    class process_runner
    {
    public:
        process_runner() = default;
        process_runner(const process_runner&) = delete;
        process_runner(process_runner&&) = delete;
        process_runner& operator=(const process_runner&) = delete;
        process_runner& operator=(process_runner&&) = delete;
        virtual ~process_runner() = default;

        // `sink`는 null일 수 있으며 그때도 출력은 계속 비우고 캡처 통계만 갱신한다.
        [[nodiscard]] virtual process_result run(const process_request& request, process_output_sink* sink, const process_cancellation_token& token) noexcept = 0;
    };
} // namespace cyella
