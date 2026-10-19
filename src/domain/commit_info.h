#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cyella {
    // `git log` 한 줄이 주는 커밋 메타데이터다. 그래프 레인은 `commit_graph_layout`이 이 값으로 계산한다.
    struct commit_info
    {
        std::string sha {};
        std::vector<std::string> parents {};
        std::string author_name {};
        std::string author_email {};
        // 유닉스 초다. 시간대는 표시에 필요하지 않아 담지 않는다.
        std::int64_t author_time { 0 };
        std::int64_t committer_time { 0 };
        std::string subject {};
        // 본문을 요청하지 않았으면 값이 없다. 본문이 비어 있으면 빈 문자열이다.
        std::optional<std::string> body {};
    };
} // namespace cyella
