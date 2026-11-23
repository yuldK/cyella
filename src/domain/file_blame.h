#pragma once

#include <cstdint>
#include <string>

namespace cyella {
    // 줄을 마지막으로 바꾼 커밋과 그 커밋에서의 파일 경로다. (sha, path) 쌍마다 하나다. 한 커밋이 여러 경로에 걸쳐 줄을 남겼으면
    // (합쳐진 파일) 같은 sha의 항목이 경로마다 따로 있다.
    struct blame_origin
    {
        std::string sha {};
        std::string path {};
        std::string author_name {};
        std::string author_email {};
        // 유닉스 초다.
        std::int64_t author_time { 0 };
        std::string summary {};
        // 이 커밋의 부모와 거기서의 파일 경로다 (git blame의 `previous`). "이 줄의 그 전 모습"을 blame할 때 넘길 리비전과 경로다.
        // 파일을 그 전에 마지막으로 바꾼 커밋이 아니라 바로 앞 커밋이다. 파일을 처음 만든 커밋이면 비어 있다.
        std::string previous_sha {};
        std::string previous_path {};
        // 커밋하지 않은 작업 트리의 줄이다. git은 이것을 0으로만 된 해시로 낸다.
        bool uncommitted { false };
    };

    struct blame_line
    {
        // `blame_origin` 목록에서의 위치다.
        std::uint32_t origin { 0 };
        // 그 커밋의 파일에서 몇째 줄이었는지와 blame한 파일에서 몇째 줄인지다. 둘 다 1부터 센다.
        std::uint32_t original_line { 0 };
        std::uint32_t final_line { 0 };
        // 줄바꿈을 뺀 원문 byte다. `\r`과 올바르지 않은 UTF-8도 그대로 둔다.
        std::string text {};
    };
} // namespace cyella
