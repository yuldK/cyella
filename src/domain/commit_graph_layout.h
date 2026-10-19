#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cyella {
    // 커밋 그래프의 레인 배치다. 입력은 `git log --topo-order` 순서(자식이 부모보다
    // 먼저)의 {sha, 부모 sha 목록}이고, 출력은 행마다 노드 레인·색·그릴 선분이다.
    // 파서·git·UI 어느 것에도 의존하지 않는 순수 도메인 코드다.
    //
    // 배치는 **온라인**이다. 행 i는 0..i번 커밋만으로 정해지고 뒤 페이지에 영향받지
    // 않는다. 그래서 이미 내보낸 행은 절대 바뀌지 않으며(UI가 행을 캐시한다), 페이지를
    // 어떻게 나눠 append해도 행이 똑같다.

    // 색 팔레트의 크기다. 색 번호는 0..graph_palette_size-1이고 실제 색은 UI가 고른다.
    inline constexpr std::uint8_t graph_palette_size { 8 };

    // WIP 가짜 행에 쓰는 sha다. 실제 커밋 sha와 겹칠 수 없는 모양(40자 16진수가 아님)이다.
    inline constexpr std::string_view graph_wip_sha { "WIP" };

    struct graph_commit
    {
        std::string sha;
        std::vector<std::string> parent_shas;
    };

    enum class graph_edge_kind : std::uint8_t
    {
        // 행 위 경계에서 아래 경계까지 같은 레인으로 곧게 지나간다 (from_lane == to_lane).
        // 이 행의 노드와 무관한 선이다.
        pass_through,
        // 행 위 경계의 from_lane에서 노드(to_lane == node lane)로 들어온다. 다르면 합류
        // 곡선, 같으면 곧은 선이다. 자식 쪽 선이며 선의 색은 from_lane의 색이다.
        incoming,
        // 노드(from_lane == node lane)에서 행 아래 경계의 to_lane으로 나간다. 다르면 병합
        // 부모로 가는 곡선, 같으면 첫 부모로 가는 곧은 선이다. 색은 to_lane의 색이다.
        outgoing,
    };

    // 선분 하나다. 레인 번호는 0부터이고 왼쪽이 0이다. 위쪽 절반(incoming)과 아래쪽
    // 절반(outgoing)은 서로 다른 선분이라, 한 행의 같은 레인에 위·아래 색이 다를 수 있다.
    struct graph_edge
    {
        std::uint16_t from_lane { 0 };
        std::uint16_t to_lane { 0 };
        std::uint8_t color { 0 };
        graph_edge_kind kind { graph_edge_kind::pass_through };

        [[nodiscard]] bool operator==(const graph_edge&) const = default;
    };

    struct graph_row
    {
        std::uint16_t lane { 0 };
        std::uint8_t color { 0 };
        // 부모가 둘 이상이다 (중복 부모는 하나로 센다).
        bool is_merge { false };
        // 부모가 없다.
        bool is_root { false };
        // 이 커밋을 부모로 꼽은 커밋이 지금까지 append된 것 중에 없다. 그래서 위에서 노드로
        // 들어오는 incoming 선이 없다. ref 끝(tip)이면 정상이고, 페이지가 역사 중간에서
        // 시작했거나 로드하지 않은 ref에서만 닿는 커밋이면 UI가 위로 이어지는 짧은 선을
        // 그리는 단서로 쓴다. ref를 모르는 레이아웃은 둘을 구별하지 못한다.
        bool has_unloaded_child { false };
        // 위에서부터 레인 순서(pass_through·incoming), 그 뒤 부모 순서(outgoing)다.
        std::vector<graph_edge> edges;

        [[nodiscard]] bool operator==(const graph_row&) const = default;
    };

    // WIP 행은 HEAD를 부모로 하는 가짜 커밋 `{WIP, [HEAD]}`를 **첫 페이지 맨 앞에** 끼워
    // 만든다. 별도 조회 함수 대신 이 방식을 고른 이유는 WIP 행이 평범한 graph_row라
    // 렌더러가 특별 취급하지 않아도 되고, HEAD 행에는 incoming 선이 자연히 생기며,
    // 온라인 배치(내보낸 행은 불변)가 그대로 유지되기 때문이다. WIP 레인은 HEAD 행까지
    // 이어진다. head_sha가 비면(unborn HEAD) 부모 없는 WIP 행이 된다.
    [[nodiscard]] graph_commit make_wip_commit(std::string_view head_sha);

    class commit_graph_layout
    {
    public:
        // 커밋을 topo 순서(자식 먼저)로 이어 붙이고 새로 생긴 행을 입력 순서대로 돌려준다.
        // 여러 번 불러도 된다. 레이아웃은 행을 보관하지 않는다 (캐시는 호출자 몫이다).
        // 같은 sha를 두 번 넣거나 부모를 자식보다 먼저 넣으면 선이 열린 채 남을 뿐
        // 배치가 깨지지는 않는다.
        [[nodiscard]] std::vector<graph_row> append(std::span<const graph_commit> commits);

        // 지금까지 쓰인 레인 수의 최댓값이다. 줄지 않고 페이지가 이어지면 늘 수 있다.
        [[nodiscard]] std::uint16_t max_lane_count() const noexcept;
        // 지금까지 내보낸 행 수다.
        [[nodiscard]] std::size_t row_count() const noexcept;
        // 아직 부모가 오지 않아 아래로 열려 있는 레인 수다 (마지막 행 아래로 이어지는 선).
        [[nodiscard]] std::uint16_t open_lane_count() const noexcept;

    private:
        // 레인이 다음에 기다리는 커밋이다. expected_id가 0이면 빈 레인이다.
        struct lane_slot
        {
            std::uint32_t expected_id { 0 };
            std::uint8_t color { 0 };
        };

        static constexpr std::size_t no_lane { static_cast<std::size_t>(-1) };

        [[nodiscard]] graph_row place(const graph_commit& commit);
        [[nodiscard]] std::uint32_t intern(const std::string& sha);
        [[nodiscard]] std::size_t find_expecting(std::uint32_t id) const noexcept;
        [[nodiscard]] std::size_t take_free_lane();
        [[nodiscard]] std::uint8_t take_new_color();

        // 인덱스가 곧 레인이다. 끝쪽 빈 레인만 잘라내므로 이미 쓴 레인 번호는 안 바뀐다.
        std::vector<lane_slot> lanes_;
        // 아직 자리를 못 잡은 sha를 4바이트 id로 줄인다. 커밋이 자리를 잡으면 지운다.
        std::unordered_map<std::string, std::uint32_t> ids_;
        std::uint32_t next_id_ { 1 };
        std::uint8_t next_color_ { 0 };
        std::uint16_t max_lane_count_ { 0 };
        std::size_t row_count_ { 0 };
    };
} // namespace cyella
