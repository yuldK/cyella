#include "domain/commit_graph_layout.h"

#include <algorithm>

namespace cyella {
    graph_commit make_wip_commit(std::string_view head_sha)
    {
        graph_commit commit { std::string { graph_wip_sha }, {} };
        if (!head_sha.empty())
            commit.parent_shas.emplace_back(head_sha);

        return commit;
    }

    std::vector<graph_row> commit_graph_layout::append(std::span<const graph_commit> commits)
    {
        std::vector<graph_row> rows;
        rows.reserve(commits.size());
        for (const graph_commit& commit : commits)
            rows.push_back(place(commit));

        row_count_ += rows.size();
        return rows;
    }

    std::uint16_t commit_graph_layout::max_lane_count() const noexcept
    {
        return max_lane_count_;
    }

    std::size_t commit_graph_layout::row_count() const noexcept
    {
        return row_count_;
    }

    std::uint16_t commit_graph_layout::open_lane_count() const noexcept
    {
        const auto open = std::count_if(lanes_.begin(), lanes_.end(), [](const lane_slot& lane) { return lane.expected_id != 0; });
        return static_cast<std::uint16_t>(open);
    }

    graph_row commit_graph_layout::place(const graph_commit& commit)
    {
        graph_row row {};

        // 이 커밋의 id는 자식이 부모로 꼽을 때 이미 만들어졌다. 없으면 아무도 기다리지
        // 않는 것이다. 자리를 잡은 sha는 다시 쓰이지 않으므로 표에서 지운다.
        std::uint32_t id { 0 };
        if (const auto found = ids_.find(commit.sha); found != ids_.end())
        {
            id = found->second;
            ids_.erase(found);
        }

        // 여러 자식이 기다리면 가장 왼쪽 레인이 노드 자리다. 나머지는 노드로 합류한다.
        std::size_t node_lane = id != 0 ? find_expecting(id) : no_lane;
        row.has_unloaded_child = node_lane == no_lane;
        if (row.has_unloaded_child)
        {
            // 새 선의 시작이다. 레인을 먼저 잡고 색은 그다음에 고른다 (레인은 아직 빈
            // 칸이라 색 고르기에서 사용 중으로 세지 않는다).
            node_lane = take_free_lane();
            lanes_[node_lane].color = take_new_color();
        }

        row.lane = static_cast<std::uint16_t>(node_lane);
        row.color = lanes_[node_lane].color;
        row.edges.reserve(lanes_.size() + commit.parent_shas.size());

        // 위쪽 절반이다. 이 커밋을 기다리던 레인은 노드로 모이고 나머지는 그대로 지나간다.
        for (std::size_t lane = 0; lane < lanes_.size(); ++lane)
        {
            const lane_slot& slot = lanes_[lane];
            if (slot.expected_id == 0)
                continue;

            const auto lane_index = static_cast<std::uint16_t>(lane);
            if (slot.expected_id == id)
                row.edges.push_back({ lane_index, row.lane, slot.color, graph_edge_kind::incoming });
            else
                row.edges.push_back({ lane_index, lane_index, slot.color, graph_edge_kind::pass_through });
        }

        // 아래쪽 절반이다. 합류하는 레인은 아래 반복이 끝날 때까지 id를 쥔 채로 두어야
        // 새 부모가 같은 행에서 그 칸을 재사용하지 않는다. 위로는 한 색, 아래로는 다른
        // 색이 같은 칸에 이어지면 선이 꺾여 보이는 대신 한 줄로 읽혀 헷갈리기 때문이다.
        std::size_t unique_parents { 0 };
        for (std::size_t index = 0; index < commit.parent_shas.size(); ++index)
        {
            const std::string& parent = commit.parent_shas[index];
            // 같은 부모를 두 번 적은 입력은 한 번으로 본다 (선이 겹쳐 그려지지 않게).
            const auto earlier_end = commit.parent_shas.begin() + static_cast<std::ptrdiff_t>(index);
            if (std::find(commit.parent_shas.begin(), earlier_end, parent) != earlier_end)
                continue;

            const std::uint32_t parent_id = intern(parent);
            if (unique_parents == 0)
            {
                // 첫 부모는 이 커밋의 레인과 색을 이어받는다. 이미 다른 레인이 같은 부모를
                // 기다려도 합치지 않고 부모 행에서 합류시킨다: 선이 부모까지 곧게 내려가
                // 브랜치가 갈라진 자리가 보인다.
                lanes_[node_lane] = { parent_id, row.color };
                row.edges.push_back({ row.lane, row.lane, row.color, graph_edge_kind::outgoing });
            }
            else
            {
                std::size_t parent_lane = find_expecting(parent_id);
                if (parent_lane == no_lane)
                {
                    parent_lane = take_free_lane();
                    lanes_[parent_lane] = { parent_id, take_new_color() };
                }

                row.edges.push_back({ row.lane, static_cast<std::uint16_t>(parent_lane), lanes_[parent_lane].color, graph_edge_kind::outgoing });
            }

            ++unique_parents;
        }

        row.is_root = unique_parents == 0;
        row.is_merge = unique_parents > 1;

        // 이 커밋에서 끝나는 레인(합류한 레인, 부모 없는 노드)을 비운다.
        if (id != 0)
        {
            for (lane_slot& slot : lanes_)
                if (slot.expected_id == id)
                    slot = {};
        }

        // 끝쪽 빈 레인만 자른다. 가운데 빈 칸은 이후 새 선이 재사용한다.
        while (!lanes_.empty() && lanes_.back().expected_id == 0)
            lanes_.pop_back();

        std::size_t lane_count = node_lane + 1;
        for (const graph_edge& edge : row.edges)
            lane_count = std::max<std::size_t>({ lane_count, edge.from_lane + std::size_t { 1 }, edge.to_lane + std::size_t { 1 } });

        max_lane_count_ = std::max(max_lane_count_, static_cast<std::uint16_t>(lane_count));
        return row;
    }

    std::uint32_t commit_graph_layout::intern(const std::string& sha)
    {
        const auto [position, inserted] = ids_.try_emplace(sha, next_id_);
        if (inserted)
            ++next_id_;

        return position->second;
    }

    std::size_t commit_graph_layout::find_expecting(std::uint32_t id) const noexcept
    {
        for (std::size_t lane = 0; lane < lanes_.size(); ++lane)
            if (lanes_[lane].expected_id == id)
                return lane;

        return no_lane;
    }

    std::size_t commit_graph_layout::take_free_lane()
    {
        for (std::size_t lane = 0; lane < lanes_.size(); ++lane)
            if (lanes_[lane].expected_id == 0)
                return lane;

        lanes_.emplace_back();
        return lanes_.size() - 1;
    }

    std::uint8_t commit_graph_layout::take_new_color()
    {
        // 지금 살아 있는 선과 겹치지 않는 다음 색을 고른다. 이웃 선이 같은 색이면 구별이
        // 안 되기 때문이다. 팔레트가 다 쓰였을 때만 겹치는 것을 받아들인다.
        std::uint32_t in_use { 0 };
        for (const lane_slot& lane : lanes_)
            if (lane.expected_id != 0)
                in_use |= std::uint32_t { 1 } << lane.color;

        std::uint8_t color { next_color_ };
        for (std::uint8_t attempt = 0; attempt < graph_palette_size; ++attempt)
        {
            const auto candidate = static_cast<std::uint8_t>((next_color_ + attempt) % graph_palette_size);
            if ((in_use & (std::uint32_t { 1 } << candidate)) == 0)
            {
                color = candidate;
                break;
            }
        }

        next_color_ = static_cast<std::uint8_t>((color + 1) % graph_palette_size);
        return color;
    }
} // namespace cyella
