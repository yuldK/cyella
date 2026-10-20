#pragma once

#include "infrastructure/git_operation_context.h"

#include <optional>

// 커밋 서명의 operation과 refresh가 읽는 값이다. `git_operation_runner`의 구현 파일끼리만 쓰는 내부 헤더다.
namespace cyella {
    // 서명 요청이면 실행해서 끝맺음을 돌려주고, 아니면 값이 없다. `run_operation`이 lane 분기 전에 부른다.
    [[nodiscard]] std::optional<operation_outcome> run_signing_operation(git_operation_context& context);

    // refresh가 snapshot에 덧붙이는 서명 설정의 실효 값이다. 읽지 못하면 비워 두고 refresh를 멈추지 않는다.
    void read_signing_snapshot(git_operation_context& context, repository_snapshot& snapshot);
} // namespace cyella
