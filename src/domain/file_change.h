#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace cyella {
    // 변경이 index(staged)에 있는지 작업 트리(unstaged)에 있는지 가른다. 같은 파일이
    // 양쪽에 변경이 있으면 file_change도 양쪽에 하나씩 생긴다.
    enum class change_side
    {
        staged,
        unstaged,
    };

    // 파일 하나의 변경이다. 경로는 git이 준 바이트 그대로(UTF-8 가정)이며 저장소 루트
    // 기준 상대 경로다.
    struct file_change
    {
        std::string path {};
        // 이름이 바뀌거나 복사된 경우의 원래 경로다. 그 밖에는 비어 있다.
        std::string original_path {};
        // M 수정, A 추가, D 삭제, R 이름 변경, C 복사, T 형식 변경, U 충돌,
        // ? untracked, ! 무시됨.
        char status { '?' };
        change_side side { change_side::unstaged };
        bool is_binary { false };
        bool is_submodule { false };
        // 충돌일 때 git의 두 글자 코드다(UU, AA, DU 등). 그 밖에는 비어 있다.
        std::string conflict_code {};
        // numstat을 합치기 전에는 값이 없다. 바이너리는 줄 수가 없으므로 값이 없다.
        std::optional<std::uint32_t> lines_added {};
        std::optional<std::uint32_t> lines_removed {};
    };
} // namespace cyella
