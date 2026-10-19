#include "presentation/graph_geometry.h"

#include <algorithm>
#include <map>
#include <utility>

namespace cyella {
    namespace {
        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        constexpr std::size_t graph_key_length { 15 };
    } // namespace

    float graph_lane_x(const std::uint16_t lane) noexcept
    {
        return static_cast<float>(lane) * graph_lane_width + graph_lane_width / 2.0f + graph_lane_padding;
    }

    float graph_column_width(const std::uint16_t lane_count) noexcept
    {
        const std::uint16_t lanes { std::max<std::uint16_t>(lane_count, 1) };
        return static_cast<float>(lanes) * graph_lane_width + graph_lane_padding * 2.0f;
    }

    graph_curve graph_edge_curve(const graph_edge& edge, const float row_height) noexcept
    {
        const float middle { row_height / 2.0f };
        float start_y { 0.0f };
        float end_y { row_height };
        switch (edge.kind)
        {
        case graph_edge_kind::pass_through:
            break;
        case graph_edge_kind::incoming:
            end_y = middle;
            break;
        case graph_edge_kind::outgoing:
            start_y = middle;
            break;
        }

        graph_curve curve {};
        const float start_x { graph_lane_x(edge.from_lane) };
        // pass_through는 레인 배치가 같은 레인으로 주지만, 어긋난 값이 와도 세로선으로 그린다.
        const float end_x { edge.kind == graph_edge_kind::pass_through ? start_x : graph_lane_x(edge.to_lane) };
        curve.start = { start_x, start_y };
        curve.end = { end_x, end_y };
        curve.straight = start_x == end_x;
        const float control_y { (start_y + end_y) / 2.0f };
        curve.control1 = { start_x, control_y };
        curve.control2 = { end_x, control_y };
        return curve;
    }

    bool graph_edge_is_wip_chain(const graph_edge& edge, const std::size_t row_index, const std::uint16_t wip_lane, const std::size_t head_row_index) noexcept
    {
        if (row_index == 0)
            return edge.kind == graph_edge_kind::outgoing;
        if (row_index > head_row_index || edge.from_lane != wip_lane)
            return false;
        if (row_index == head_row_index)
            return edge.kind == graph_edge_kind::incoming;
        return edge.kind == graph_edge_kind::pass_through;
    }

    bool graph_row_needs_upward_stub(const graph_row& row, const bool is_wip, const bool has_labels) noexcept
    {
        return row.has_unloaded_child && is_wip == false && has_labels == false;
    }

    ref_label_map build_ref_label_map(const std::vector<ref>& refs, const repository_summary& summary)
    {
        ref_label_map labels {};
        // 원격 브랜치 이름 → 그 ref다. 로컬의 upstream 짝을 찾는다.
        std::map<std::string, const ref*> remotes {};
        for (const ref& value : refs)
            if (value.kind == ref_kind::remote)
                remotes.emplace(value.name, &value);

        std::unordered_map<std::string, bool> paired {};
        if (summary.is_detached && summary.head_sha.empty() == false)
            labels[summary.head_sha].push_back({ .kind = ref_label_kind::head, .text = u8"HEAD", .ref_name = summary.head_sha });

        const auto add_locals = [&](const bool current_pass) {
            for (const ref& value : refs)
            {
                if (value.kind != ref_kind::local || value.is_current != current_pass)
                    continue;
                ref_label label { .kind = ref_label_kind::local, .text = to_u8(value.name), .ref_name = value.name, .current = value.is_current };
                // 짝은 upstream이 먼저고, upstream이 없으면 같은 이름의 원격 브랜치다.
                const ref* pair { nullptr };
                if (const auto found { remotes.find(value.upstream) }; value.upstream.empty() == false && found != remotes.end())
                    pair = found->second;
                else if (value.upstream.empty())
                    for (const auto& [name, remote] : remotes)
                        if (paired.contains(name) == false && name.ends_with("/" + value.name) && remote->target_sha == value.target_sha)
                        {
                            pair = remote;
                            break;
                        }
                if (pair != nullptr && pair->target_sha == value.target_sha && paired.contains(pair->name) == false)
                {
                    label.also_remote = true;
                    label.remote_name = to_u8(pair->name);
                    paired[pair->name] = true;
                }
                labels[value.target_sha].push_back(std::move(label));
            }
        };
        add_locals(true);
        add_locals(false);

        const bool has_remotes { summary.remotes.empty() == false };
        for (const ref& value : refs)
        {
            if (value.kind == ref_kind::remote)
            {
                // `origin/HEAD`는 다른 원격 브랜치를 가리키는 별칭이라 라벨로 세우지 않는다.
                if (paired.contains(value.name) || value.name.ends_with("/HEAD"))
                    continue;
                labels[value.target_sha].push_back({ .kind = ref_label_kind::remote, .text = to_u8(value.name), .ref_name = value.name });
            }
        }
        for (const ref& value : refs)
            if (value.kind == ref_kind::tag)
                labels[value.target_sha].push_back({ .kind = ref_label_kind::tag, .text = to_u8(value.name), .ref_name = value.name, .local_only = has_remotes && value.remotes.empty() });
        for (const ref& value : refs)
            if (value.kind == ref_kind::stash)
                labels[value.target_sha].push_back({ .kind = ref_label_kind::stash, .text = to_u8(value.name), .ref_name = value.name });
        return labels;
    }

    std::u8string graph_item_key(const std::string_view sha)
    {
        return to_u8(sha.substr(0, std::min(sha.size(), graph_key_length)));
    }
} // namespace cyella
