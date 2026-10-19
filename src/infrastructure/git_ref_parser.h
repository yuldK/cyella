#pragma once

#include "domain/ref.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // `refs/remotes/<remote>/HEAD`가 가리키는 원격의 기본 브랜치다. 브랜치 목록에는
    // 넣지 않고 따로 알린다.
    struct remote_default_branch
    {
        std::string remote {};
        // `main`처럼 원격 이름을 뗀 브랜치 이름이다.
        std::string branch {};
    };

    struct git_ref_list
    {
        std::vector<ref> refs {};
        std::vector<remote_default_branch> remote_defaults {};
        // 모양이 맞지 않거나 다루지 않는 ref(`refs/notes/*` 등)라 건너뛴 수다.
        std::size_t skipped_count { 0 };
    };

    // `git for-each-ref`의 출력을 해석한다. 형식은 git_command_builder의
    // make_git_for_each_ref_args가 정한다. 필드는 0x1F로, 레코드는 줄바꿈 또는 NUL로
    // 나뉜다.
    //
    //   refname, objectname, *objectname, HEAD, upstream, upstream:short,
    //   upstream:track, symref
    //
    // upstream의 ahead/behind 개수는 `upstream:track` 문구(`[ahead 1, behind 2]`)에서
    // 읽는다. 이 문구는 지역화될 수 있어 best-effort다. LC_ALL=C로 git을 실행하길
    // 권한다. 지역화되어 읽지 못하면 개수는 0이다. 반면 upstream이 사라졌는지(gone)는
    // 문구가 아니라 목록 안에 추적 ref가 있는지로 판정하므로 지역화와 무관하다.
    [[nodiscard]] git_ref_list parse_git_refs(std::string_view bytes);

    // `git ls-remote --tags <remote>`의 한 줄이다.
    struct remote_tag
    {
        std::string name {};
        // annotated 태그는 `^{}`가 붙은 줄의 커밋 SHA다. 없으면 태그 객체(=커밋) SHA다.
        std::string sha {};
    };

    [[nodiscard]] std::vector<remote_tag> parse_ls_remote_tags(std::string_view bytes);

    // 태그 ref마다 원격에 같은 이름이 있는지(exists_on_remote)를 채운다. 이름만 같고
    // 가리키는 커밋이 다른 태그는 원격에 "없는" 것으로 본다. push해도 거절되기 때문이다.
    void apply_remote_tags(std::span<ref> refs, std::span<const remote_tag> remote_tags);
} // namespace cyella
