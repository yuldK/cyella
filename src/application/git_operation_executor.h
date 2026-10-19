#pragma once

#include "application/git_operation.h"
#include "application/operation_events.h"
#include "application/process_cancellation.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace cyella {
    // worker가 operation 하나를 실행하는 데 필요한 값이다. job을 만들 때 logic thread가 채워
    // 복사해 넘기므로 worker는 세션 상태를 읽지 않는다.
    struct git_operation_job
    {
        operation_id id { 0 };
        std::string repository_id {};
        // 저장소 작업 트리의 절대 경로다. clone·init·inspect_path는 비어 있다.
        std::u8string repository_path {};
        git_operation operation {};
        // refresh·load_more_log의 generation이다. 사건에 실려 돌아와 늦은 결과를 거른다.
        std::uint64_t generation { 0 };
        // load_more_log가 읽기 시작할 위치다(이미 받은 커밋 수).
        std::size_t log_skip { 0 };
        // load_more_log에서 HEAD도 log의 시작점으로 넣는다. 첫 커밋 전이면 `log HEAD`가 실패하므로
        // 마지막 snapshot이 HEAD를 알 때만 켠다.
        bool log_include_head { true };
        // 접수 시점의 현재 브랜치다. merge(into)·fast_forward(target)가 체크아웃 여부를 가른다.
        // 실행 중에 바뀌었을 수 있어 실행기는 status로 다시 확인한다.
        std::string current_branch_hint {};
        // 사용자의 취소와 앱 종료가 합쳐진 token이다. 실행기는 이것을 그대로 runner에 넘긴다.
        process_cancellation_token cancellation {};
    };

    // operation을 git 명령으로 바꿔 실행하는 경계다. 구현(infrastructure/git_operation_runner)은
    // process_runner와 해석기를 쓰고, worker thread에서 동기로 돈다.
    //
    // 약속:
    // - 사건은 `emit`으로만 내보낸다. 마지막 사건은 반드시 `operation_finished` 하나다.
    // - `needs_input`(체크아웃 정책, 충돌)은 기다리지 않고 끝내는 결과다. worker를 붙잡는 대기는
    //   인증(askpass)뿐이다.
    // - 예외를 던지지 않는다.
    class git_operation_executor
    {
    public:
        git_operation_executor() = default;
        git_operation_executor(const git_operation_executor&) = delete;
        git_operation_executor(git_operation_executor&&) = delete;
        git_operation_executor& operator=(const git_operation_executor&) = delete;
        git_operation_executor& operator=(git_operation_executor&&) = delete;
        virtual ~git_operation_executor() = default;

        virtual void execute(const git_operation_job& job, const operation_event_sink& emit) noexcept = 0;
    };
} // namespace cyella
