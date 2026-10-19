#pragma once

#include "domain/git_tool.h"

#include <optional>
#include <string_view>

namespace cyella {
    // `--version` 첫 줄에서 버전을 뽑는다. 형식은 다음과 같다.
    //
    //     git version 2.52.0.windows.1
    //
    // 네 번째 이후 구성 요소와 플랫폼 접미사는 비교에 쓰지 않는다. Git for Windows의
    // `.windows.N`처럼 배포판마다 다른 값이라 최소 버전 판정 기준이 될 수 없다.
    [[nodiscard]] std::optional<git_tool_version> parse_git_tool_version(std::u8string_view first_line) noexcept;

    [[nodiscard]] bool meets_minimum_git_version(const git_tool_version& version) noexcept;
} // namespace cyella
