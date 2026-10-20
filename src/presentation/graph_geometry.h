#pragma once

#include "domain/commit_graph_layout.h"
#include "domain/ref.h"
#include "domain/repository_summary.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cyella {
    // 그래프 칸의 치수다 (논리 픽셀).
    inline constexpr float graph_row_height { 28.0f };
    inline constexpr float graph_lane_width { 16.0f };
    inline constexpr float graph_lane_padding { 6.0f };
    inline constexpr float graph_node_radius { 5.0f };
    inline constexpr float graph_merge_node_radius { 4.0f };
    inline constexpr float graph_head_ring_radius { 8.5f };
    inline constexpr float graph_line_width { 2.0f };

    // 레인 `lane`의 가운데 x다 (그래프 칸 왼쪽 기준).
    [[nodiscard]] float graph_lane_x(std::uint16_t lane) noexcept;
    // 레인 `lane_count`개를 담는 그래프 칸의 폭이다. 레인이 없어도 한 레인 폭은 남긴다.
    [[nodiscard]] float graph_column_width(std::uint16_t lane_count) noexcept;

    struct graph_point
    {
        float x { 0.0f };
        float y { 0.0f };

        [[nodiscard]] bool operator==(const graph_point&) const noexcept = default;
    };

    // 선분 하나의 그림이다. `straight`면 start→end 직선이고, 아니면 세로 접선을 가진 3차 베지어다.
    struct graph_curve
    {
        graph_point start {};
        graph_point control1 {};
        graph_point control2 {};
        graph_point end {};
        bool straight { true };

        [[nodiscard]] bool operator==(const graph_curve&) const noexcept = default;
    };

    // 레인 배치가 낸 선분의 그림이다 (행 왼쪽 위 기준, 행 높이 `row_height`).
    //  - `incoming`: `from_lane`의 위 가장자리 가운데에서 노드(`to_lane`, 행 가운데)로 들어온다.
    //  - `outgoing`: 노드(`from_lane`, 행 가운데)에서 `to_lane`의 아래 가장자리 가운데로 나간다.
    //  - `pass_through`: 위 가장자리에서 아래 가장자리까지 세로 직선이다.
    // 레인이 다르면 두 끝의 가운데 높이에 제어점을 둔다 — 행 경계에서 접선이 세로라 행 사이 이음매가 맞는다.
    [[nodiscard]] graph_curve graph_edge_curve(const graph_edge& edge, float row_height = graph_row_height) noexcept;

    // WIP 행에서 HEAD 행까지 WIP 레인의 선은 점선이다. 이 선분이 그 사슬에 드는가.
    //  - `row_index`는 그래프 행 번호(WIP가 0), `head_row_index`는 HEAD 커밋의 행 번호다.
    //  - WIP 행의 outgoing, 그 사이 행의 WIP 레인 pass_through, HEAD 행의 WIP 레인 incoming이 든다.
    [[nodiscard]] bool graph_edge_is_wip_chain(const graph_edge& edge, std::size_t row_index, std::uint16_t wip_lane, std::size_t head_row_index) noexcept;

    // 노드 위로 짧은 선을 그려 "위에 읽지 않은 자식이 있다"를 보이는가.
    // ref 끝(라벨이 있는 커밋)은 자식이 없는 것이 정상이고, WIP 행은 위가 없다.
    [[nodiscard]] bool graph_row_needs_upward_stub(const graph_row& row, bool is_wip, bool has_labels) noexcept;

    enum class ref_label_kind
    {
        // detached HEAD다.
        head,
        local,
        remote,
        tag,
        stash,
    };

    // 그래프 행의 라벨 하나다. 같은 커밋의 로컬·원격 짝은 라벨 하나로 합친다(`also_remote`).
    struct ref_label
    {
        ref_label_kind kind { ref_label_kind::local };
        std::u8string text {};
        // 체크아웃할 때 넘기는 짧은 이름이다(`main`, `origin/main`).
        std::string ref_name {};
        bool current { false };
        // 로컬 브랜치의 upstream이 같은 커밋을 가리킨다. 라벨 끝에 구름 아이콘을 단다.
        bool also_remote { false };
        std::u8string remote_name {};
        // 원격이 있는데 어느 원격에도 없는 태그다. 점선 테두리로 그린다.
        bool local_only { false };
        // 로컬 브랜치가 **다른** worktree에 체크아웃되어 있으면 그 폴더다. 라벨 끝에 worktree 표지를 단다.
        std::string worktree_path {};

        [[nodiscard]] bool operator==(const ref_label&) const noexcept = default;
    };

    // 커밋 sha마다 그릴 라벨이다. 순서는 HEAD → 로컬(현재 브랜치 먼저) → 남은 원격 → 태그 → stash다.
    using ref_label_map = std::unordered_map<std::string, std::vector<ref_label>>;
    [[nodiscard]] ref_label_map build_ref_label_map(const std::vector<ref>& refs, const repository_summary& summary);

    // 그래프 목록의 항목 키다. 40자 SHA는 MSVC 짧은 문자열 최적화(15바이트)를 넘어 10만 행이면
    // frame마다 할당이 10만 번이 된다. 앞 15자(60비트)로 줄여 모델 복사를 할당 없이 끝낸다.
    [[nodiscard]] std::u8string graph_item_key(std::string_view sha);
} // namespace cyella
