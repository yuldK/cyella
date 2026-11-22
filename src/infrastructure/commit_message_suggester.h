#pragma once

#include "application/commit_suggestion_messages.h"
#include "application/process_cancellation.h"
#include "application/process_request.h"
#include "application/process_runner.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 제안 명령의 시한이다. 로그인이 풀려 입력을 기다리거나 네트워크가 멈춘 명령이 worker를 붙잡지 않게 한다.
    inline constexpr std::chrono::seconds commit_suggestion_timeout { 120 };
    // 답으로 받는 표준 출력의 상한이다. 커밋 메시지는 길어야 몇 KiB다. 넘으면 답을 쓰지 않는다.
    inline constexpr std::size_t commit_suggestion_output_limit { std::size_t { 64 } * 1024 };

    struct commit_message_suggester_options
    {
        // 설정에 적힌 실행 파일을 절대 경로로 푼다 (PATH의 이름 → 경로). 못 찾으면 값이 없다. 플랫폼 계층이 준다.
        std::function<std::optional<std::u8string>(std::u8string_view)> resolve_executable {};
        // 명령을 띄울 폴더다. 저장소가 아닌 중립 폴더여야 한다 — 저장소에서 띄우면 그 저장소의 에이전트 설정 파일이 명령에 끼어든다.
        std::u8string working_directory {};
        std::chrono::milliseconds timeout { commit_suggestion_timeout };
        // git 조회에만 더하는 환경 변수다. test가 사용자 git 설정과 떼어 놓는다. 제안 명령의 환경은 건드리지 않는다.
        std::vector<process_environment_override> git_environment {};
    };

    // staged 변경을 읽어 프롬프트를 만들고, 사용자가 고른 외부 명령에 표준 입력으로 넘겨 답을 메시지로 받는다 (ADR-009).
    // worker에서 부르며 명령이 끝날 때까지 막힌다. 저장소는 읽기만 한다 (`--no-optional-locks`의 로컬 조회).
    class commit_message_suggester
    {
    public:
        commit_message_suggester(process_runner& runner, std::u8string git_executable, commit_message_suggester_options options);

        [[nodiscard]] commit_suggestion_result_intent suggest(const commit_suggestion_job& job, const process_cancellation_token& token) const;

    private:
        process_runner* runner_ { nullptr };
        std::u8string git_executable_ {};
        commit_message_suggester_options options_ {};
    };
} // namespace cyella
