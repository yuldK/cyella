#pragma once

#include <cstdint>
#include <string>

namespace cyella {
    struct stash_entry
    {
        // `stash@{N}`의 N이다. 새 stash일수록 작다.
        std::uint32_t index { 0 };
        std::string name {};
        std::string sha {};
        // stash를 만든 시점의 HEAD 커밋이다.
        std::string base_sha {};
        // stash를 만든 브랜치 이름이다. 파악하지 못하면 비어 있다.
        std::string branch {};
        // 사용자가 준 메시지, 또는 git이 붙인 `<abbrev> <subject>`다.
        std::string message {};
        // `stash push -m`으로 메시지를 준 경우 false, git이 만든 `WIP on` 문구면 true.
        bool is_automatic_message { false };
        // `-u`로 untracked 파일도 담았으면 true다(부모가 셋).
        bool has_untracked { false };
        std::int64_t commit_time { 0 };
    };
} // namespace cyella
