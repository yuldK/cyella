#pragma once

#include "domain/diff_file.h"
#include "domain/file_change.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // git의 C 스타일 경로 인용(`"a/\355\225\234.txt"`)을 푼다. 따옴표로 싸이지 않은
    // 입력은 그대로 돌려준다. `\n \t \" \\`와 3자리 8진 이스케이프를 이해한다.
    // `core.quotepath`가 켜져 있으면 ASCII 밖의 바이트도 8진으로 인용된다.
    [[nodiscard]] std::string unquote_git_path(std::string_view text);

    // `git diff` / `git diff --cached` / `git show --format=` 의 unified diff 출력을
    // 파일 목록으로 해석한다. `--no-color --no-ext-diff`로 만든 출력을 가정하며,
    // 경로 접두어는 `a/`와 `b/`다(make_git_*_args가 `--src-prefix`·`--dst-prefix`로
    // 고정한다). 출력이 비어 있으면 빈 목록이다.
    //
    // - hunk의 줄 수는 헤더의 개수로 센다. 그래서 내용이 `--- `나 `diff --git`로
    //   시작하는 줄도 헤더로 오인하지 않는다.
    // - 줄 끝의 '\r'은 내용에 남긴다. 줄 나눔은 '\n'만 본다.
    // - 충돌 병합의 `diff --cc`는 지원하지 않고 건너뛴다. 병합 커밋은 `-m
    //   --first-parent`로 첫 부모와의 일반 diff를 받는다.
    [[nodiscard]] std::vector<diff_file> parse_git_diff(std::string_view text);

    // `git diff --numstat -z`의 한 항목이다.
    struct numstat_entry
    {
        std::string path {};
        // 이름 변경·복사일 때의 원래 경로다.
        std::string original_path {};
        // 바이너리 파일은 `-`로 나와 값이 없다.
        std::optional<std::uint32_t> lines_added {};
        std::optional<std::uint32_t> lines_removed {};
        bool is_binary { false };
    };

    // `--numstat -z`의 출력을 해석한다. 일반 항목은 `<added>\t<removed>\t<path>\0`,
    // 이름 변경은 `<added>\t<removed>\t\0<old>\0<new>\0`다.
    [[nodiscard]] std::vector<numstat_entry> parse_git_numstat(std::string_view bytes);

    // numstat의 줄 수와 바이너리 여부를 file_change에 합친다. 경로가 같은 항목만
    // 맞추며, 같은 쪽(staged 또는 unstaged)의 numstat을 넘겨야 한다.
    void apply_numstat(std::span<file_change> changes, std::span<const numstat_entry> entries);
} // namespace cyella
