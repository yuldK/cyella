#pragma once

#include "application/process_cancellation.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace cyella {
    // UI가 operation을 요청할 때 붙이는 번호다. 결과 메시지에 그대로 실려 돌아와, 늦게 온
    // 결과를 버리는 데 쓰인다.
    using operation_id = std::uint64_t;

    // worker가 job을 실행할 때 넘기는 값이다.
    struct operation_context
    {
        operation_id id {};
        std::string_view repository_key {};
        // scheduler가 종료를 시작하면 취소된다. job은 이 token을 `process_runner::run`에
        // 그대로 넘겨, 실행 중인 git 프로세스가 종료 때 정리되게 한다. 종료 시점에 큐에 남은
        // job도 취소된 token으로 호출되므로, 곧바로 끝내고 마지막 결과(취소)만 보내면 된다.
        process_cancellation_token shutdown {};
    };

    using operation_job = std::function<void(const operation_context&)>;

    // scheduler가 lane에 배정하는 단위다. scheduler는 git 작업의 종류를 모른다. 작업은
    // job 안에 담아 오므로 operation 종류가 늘어도 scheduler는 바뀌지 않는다.
    struct operation_request
    {
        operation_id id {};
        // 이 작업이 index나 ref를 바꾸거나 `status`처럼 index 잠금을 잡을 수 있는 저장소다.
        // 같은 키의 작업은 접수 순서대로 하나씩 실행한다. 비어 있으면 저장소와 무관한
        // 작업(설정 파일 입출력 등)이며 하나의 공용 lane에서 직렬로 실행한다.
        std::string repository_key {};
        // 저장소를 바꾸지 않는 대화형 조회(diff, show, log 한 쪽)다. 저장소 lane과 분리된
        // 전용 lane에서 실행하므로 오래 걸리는 fetch 뒤에 줄 서지 않는다. 이 값이 true면
        // `repository_key`는 lane 배정에 쓰이지 않는다.
        bool interactive { false };
        operation_job job {};
    };

    // worker thread가 operation 하나를 동기로 수행하는 경계의 모양이다. 구체적인 git
    // 작업을 아는 실행기가 이 인터페이스를 구현하고 job이 그것을 부른다. scheduler가 git
    // 작업을 몰라도 프로세스 없이 결정적으로 검증할 수 있게 둘을 분리한다.
    class operation_executor
    {
    public:
        operation_executor() = default;
        operation_executor(const operation_executor&) = delete;
        operation_executor(operation_executor&&) = delete;
        operation_executor& operator=(const operation_executor&) = delete;
        operation_executor& operator=(operation_executor&&) = delete;
        virtual ~operation_executor() = default;

        // 결과는 실행기가 가진 경로(relay)로 보낸다. 어떤 경로에서도 마지막 결과(성공,
        // 실패, 취소)는 반드시 나가야 UI의 busy 상태가 풀린다. 예외를 던지지 않는다.
        virtual void execute(const operation_context& context) noexcept = 0;
    };
} // namespace cyella
