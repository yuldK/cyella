#pragma once

#include "domain/commit_suggestion.h"
#include "domain/text_catalog.h"

#include <cstdint>
#include <string>

namespace cyella {
    // 커밋 메시지 제안은 git operation이 아니다 (ADR-009): 저장소의 읽기·쓰기 줄을 잡지 않아야 하는 5~30초짜리 외부 명령이라,
    // 호스팅 호출처럼 `operation_kind`를 건드리지 않고 아래 일감과 결과 메시지로 오간다. 화면(controller)이 일감을 내면 driver가
    // 워커에서 `commit_message_suggester`로 돌리고, 결과는 relay를 거쳐 logic thread의 `commit_suggestion_result_intent`로 돌아온다.
    // 늦은 결과는 `serial`로 가려 버린다.
    struct commit_suggestion_job
    {
        std::uint64_t serial { 0 };
        std::string repository_id {};
        std::u8string repository_path {};
        // 설정에서 온 명령이다. 일감을 낼 때의 값을 실어 보내 도는 동안 설정이 바뀌어도 흔들리지 않는다.
        std::u8string executable {};
        std::u8string arguments {};
        std::u8string instruction {};
        // 최근 커밋이 없을 때 메시지를 쓸 언어다 (화면 언어).
        ui_language language { ui_language::korean };
    };

    enum class commit_suggestion_state
    {
        succeeded,
        failed,
        // 사용자가 취소했거나 앱이 닫히는 중이다. 알릴 것이 없다.
        cancelled,
    };

    struct commit_suggestion_result_intent
    {
        std::uint64_t serial { 0 };
        std::string repository_id {};
        commit_suggestion_state state { commit_suggestion_state::failed };
        suggested_commit_message message {};
        // 실패했을 때 사용자에게 보일 이유다 (화면 언어).
        std::u8string error {};
    };
} // namespace cyella
