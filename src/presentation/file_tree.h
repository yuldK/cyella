#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <vector>

namespace cyella {
    // 변경 파일 트리의 한 행이다. 폴더 행과 파일 행이 한 목록에 섞여 위에서 아래로 그려진다.
    struct file_tree_row
    {
        bool folder { false };
        // 폴더는 `d:` + 경로, 파일은 빈 값이다 (파일 행은 목록의 파일 키를 그대로 쓴다).
        std::u8string key {};
        // 폴더 행에 보일 이름이다. 하위 폴더 하나만 있는 사슬은 `src/presentation`처럼 한 행으로 접는다.
        std::u8string label {};
        // 폴더 경로(끝에 `/` 없음)다. 파일 행은 비어 있다.
        std::string path {};
        // 들여쓰기 단계다. 맨 위가 0이다.
        std::size_t depth { 0 };
        // 파일 행이 가리키는 입력의 번호다.
        std::size_t file_index { 0 };
        // 폴더 아래(하위 폴더 포함)의 파일 수다.
        std::size_t file_count { 0 };
        bool expanded { true };
    };

    // 경로 목록(`/`로 나뉜 저장소 상대 경로)을 폴더 트리 행으로 편다. 접힌 폴더(`collapsed`)의 아래는 내보내지 않는다.
    //  - 한 단계 안에서는 폴더가 먼저, 파일이 나중이고 각자 이름 순이다 (탐색기와 같은 순서).
    //  - 하위 폴더 하나만 있고 파일이 없는 폴더는 아래 폴더와 한 행으로 합친다. 깊은 소스 트리에서 들여쓰기만 늘지 않게 한다.
    //    합친 행의 경로는 가장 아래 폴더의 경로라, 접기는 그 경로로 기억한다.
    [[nodiscard]] std::vector<file_tree_row> build_file_tree(const std::vector<std::string>& paths, const std::set<std::string>& collapsed);
} // namespace cyella
