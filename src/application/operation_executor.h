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
        // 이 작업이 index·ref·작업 트리를 바꾸는 저장소다(fetch 포함). 같은 키의 작업은 접수
        // 순서대로 하나씩 실행한다. 비어 있으면 저장소와 무관한 작업(설정 파일 입출력 등)이며
        // 하나의 공용 lane에서 직렬로 실행한다.
        std::string repository_key {};
        // 저장소를 바꾸지 않는 읽기 조회(status, for-each-ref, log, diff, show, ls-remote)다.
        // `--no-optional-locks`로 잠금을 잡지 않으므로 쓰기 lane과 분리된 전용 lane에서 병렬로
        // 실행하고, 오래 걸리는 fetch 뒤에 줄 서지 않는다. 이 값이 true면 `repository_key`는
        // lane 배정에 쓰이지 않는다.
        bool interactive { false };
        operation_job job {};
    };
} // namespace cyella
