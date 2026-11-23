#pragma once

#include "domain/ref.h"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 그래프에 그릴 커밋의 시작점을 좁히는 값이다 (ADR-012). 이름은 전체 ref 이름이다
    // (`refs/heads/x`, `refs/remotes/origin/x`, `refs/tags/v1`) — 짧은 이름은 브랜치와 태그가 같을 수 있다.
    //  - `only`가 있으면 그 ref에서 닿는 커밋만 그린다. HEAD도 따로 더하지 않고, 그동안 `hidden`은 듣지 않는다.
    //  - `only`가 비었으면 `hidden`의 ref를 시작점에서 뺀다. 다른 ref에서도 닿는 커밋은 남는다.
    // 저장소 세션이 들고 있어 탭을 닫으면 사라진다. 설정 파일에 남기지 않는다.
    struct graph_ref_filter
    {
        std::vector<std::string> only {};
        std::vector<std::string> hidden {};

        [[nodiscard]] bool empty() const noexcept
        {
            return only.empty() && hidden.empty();
        }

        [[nodiscard]] bool operator==(const graph_ref_filter&) const noexcept = default;
    };

    // 사이드바 행이 보일 표시다. 지금 그래프에 듣는 것만 말한다: `only`가 있는 동안 숨긴 ref는 표시하지 않는다.
    enum class graph_ref_mark
    {
        none,
        only,
        hidden,
    };

    [[nodiscard]] graph_ref_mark graph_ref_mark_of(const graph_ref_filter& filter, std::string_view full_name) noexcept;

    // 지금 저장소에 맞춘 필터다. 없어진 ref를 빼고 (`git log <없는 ref>`는 그래프 전체를 실패시킨다), 현재 브랜치는 숨긴 목록에서 뺀다.
    // `only`가 다 없어지면 `only` 없는 필터가 된다. `current_branch`는 짧은 이름이고 detached면 비어 있다.
    //  - refresh의 실행기와 세션이 같은 ref 목록에 이 함수 하나를 쓴다. 둘이 한 ref라도 다르게 거르면 첫 쪽과 이어 읽는 쪽의
    //    ref 집합이 달라져 `--skip` 경계가 어긋난다.
    [[nodiscard]] graph_ref_filter settle_graph_ref_filter(const graph_ref_filter& filter, std::span<const ref> refs, std::string_view current_branch);

    // "이것만 보기"를 켜고 끈다. 켤 때는 그 ref를 숨긴 목록에서 뺀다 (보겠다고 고른 것이 `only`를 푼 뒤 다시 숨겨지지 않게).
    void toggle_graph_ref_only(graph_ref_filter& filter, std::string_view full_name);
    // "숨기기"를 켜고 끈다.
    void toggle_graph_ref_hidden(graph_ref_filter& filter, std::string_view full_name);
} // namespace cyella
