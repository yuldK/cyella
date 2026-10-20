#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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
        // 태그가 같은 이름·같은 커밋으로 올라가 있는 원격 이름 목록이다. 원격이 둘 이상일 수
        // 있어(origin, upstream) bool 하나로는 어느 원격에 push할지 정할 수 없다. fetch 뒤
        // `git ls-remote --tags <remote>`로 원격마다 갱신하며, 확인하기 전에는 비어 있다.
        std::vector<std::string> remotes {};
        // 로컬 브랜치가 **다른** worktree에 체크아웃되어 있으면 그 폴더다. 이 저장소의 체크아웃은 비어 있다.
        // git은 이런 브랜치로 switch하는 것을 거절하므로 사이드바·그래프가 표시를 단다.
        std::string worktree_path {};
    };

    // `git ls-remote --tags <remote>`의 한 줄이다.
    struct remote_tag
    {
        std::string name {};
        // annotated 태그는 `^{}`가 붙은 줄의 커밋 SHA다. 없으면 태그 객체(=커밋) SHA다.
        std::string sha {};
    };

    // 원격 하나(`remote`)의 태그 목록으로 태그 ref의 `remotes`를 고친다. 이름만 같고 가리키는
    // 커밋이 다른 태그는 그 원격에 "없는" 것으로 본다. push해도 거절되기 때문이다. 다른
    // 원격의 기록은 건드리지 않으므로 원격마다 따로 불러 합칠 수 있다.
    void apply_remote_tags(std::span<ref> refs, std::string_view remote, std::span<const remote_tag> remote_tags);
} // namespace cyella
