#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace cyella {
    enum class ref_kind
    {
        local,
        remote,
        tag,
        stash,
        head,
    };

    struct ref
    {
        ref_kind kind { ref_kind::local };
        // 사이드바에 보일 짧은 이름이다(`main`, `origin/main`, `v1.0`).
        std::string name {};
        // `refs/heads/main` 같은 전체 이름이다. git에 되돌려 줄 때는 이것을 쓴다.
        std::string full_name {};
        // 커밋 SHA다. annotated 태그는 태그 객체가 아니라 가리키는 커밋이다.
        std::string target_sha {};
        // 로컬 브랜치의 upstream 짧은 이름이다(`origin/main`). 없으면 비어 있다.
        std::string upstream {};
        std::uint32_t ahead { 0 };
        std::uint32_t behind { 0 };
        // upstream을 설정했지만 원격 추적 ref가 사라졌다.
        bool upstream_gone { false };
        // 지금 체크아웃한 브랜치다.
        bool is_current { false };
        bool is_annotated_tag { false };
        // 태그가 원격에 있는지다. `git ls-remote --tags`로 확인하기 전에는 값이 없다.
        std::optional<bool> exists_on_remote {};
    };
} // namespace cyella
