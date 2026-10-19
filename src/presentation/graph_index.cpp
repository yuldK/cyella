#include "presentation/graph_index.h"

#include <algorithm>

namespace cyella {
    graph_index::graph_index(const std::vector<commit_info>& commits)
    {
        shas_.reserve(commits.size());
        parents_begin_.reserve(commits.size() + 1);
        by_key_.reserve(commits.size());
        for (std::size_t index { 0 }; index < commits.size(); ++index)
        {
            shas_.push_back(commits[index].sha);
            by_key_.emplace(commits[index].sha.substr(0, key_length), static_cast<std::uint32_t>(index));
        }
        std::unordered_map<std::string_view, std::uint32_t> by_sha {};
        by_sha.reserve(shas_.size());
        for (std::size_t index { 0 }; index < shas_.size(); ++index)
            by_sha.emplace(shas_[index], static_cast<std::uint32_t>(index));
        for (const commit_info& commit : commits)
        {
            parents_begin_.push_back(static_cast<std::uint32_t>(parent_indices_.size()));
            for (const std::string& parent : commit.parents)
                if (const auto found { by_sha.find(parent) }; found != by_sha.end())
                    parent_indices_.push_back(found->second);
        }
        parents_begin_.push_back(static_cast<std::uint32_t>(parent_indices_.size()));
    }

    std::optional<std::size_t> graph_index::find(const std::string_view sha) const
    {
        if (sha.empty())
            return std::nullopt;
        const auto found { by_key_.find(std::string { sha.substr(0, key_length) }) };
        if (found == by_key_.end())
            return std::nullopt;
        const std::string& full { shas_[found->second] };
        // 키는 앞 15자뿐이라 더 긴 SHA가 오면 나머지도 맞는지 본다.
        if (sha.size() > key_length && full.compare(0, sha.size(), sha) != 0)
            return std::nullopt;
        return found->second;
    }

    std::optional<bool> graph_index::is_ancestor(const std::string_view ancestor, const std::string_view descendant) const
    {
        const std::optional<std::size_t> from { find(ancestor) };
        const std::optional<std::size_t> to { find(descendant) };
        if (from.has_value() == false || to.has_value() == false)
            return std::nullopt;
        if (*from == *to)
            return true;
        // 부모는 언제나 뒤에 있다. 조상이 후손보다 앞에 있으면 조상일 수 없다.
        if (*from < *to)
            return false;
        // 후손에서 부모로 내려가며 조상의 자리를 넘지 않는 것만 따라간다.
        std::vector<bool> seen(*from + 1, false);
        std::vector<std::uint32_t> stack { static_cast<std::uint32_t>(*to) };
        seen[*to] = true;
        while (stack.empty() == false)
        {
            const std::uint32_t node { stack.back() };
            stack.pop_back();
            for (std::uint32_t at { parents_begin_[node] }; at < parents_begin_[node + 1]; ++at)
            {
                const std::uint32_t parent { parent_indices_[at] };
                if (parent == *from)
                    return true;
                if (parent > *from || seen[parent])
                    continue;
                seen[parent] = true;
                stack.push_back(parent);
            }
        }
        return false;
    }

    std::optional<std::uint32_t> graph_index::count(const std::string_view from, const std::string_view to) const
    {
        const std::optional<std::size_t> excluded { find(from) };
        const std::optional<std::size_t> included { find(to) };
        if (excluded.has_value() == false || included.has_value() == false)
            return std::nullopt;
        // 낮은 색인부터 높은 색인으로 훑으며 부모에게 표지를 물려준다. 1은 `to`에서 닿고 2는 `from`에서 닿는다.
        const std::size_t first { std::min(*excluded, *included) };
        std::vector<std::uint8_t> marks(shas_.size() - first, 0);
        marks[*included - first] |= 1;
        marks[*excluded - first] |= 2;
        std::uint32_t counted { 0 };
        for (std::size_t node { first }; node < shas_.size(); ++node)
        {
            const std::uint8_t mark { marks[node - first] };
            if (mark == 0)
                continue;
            if (mark == 1)
                ++counted;
            for (std::uint32_t at { parents_begin_[node] }; at < parents_begin_[node + 1]; ++at)
                marks[parent_indices_[at] - first] |= mark;
        }
        return counted;
    }
} // namespace cyella
