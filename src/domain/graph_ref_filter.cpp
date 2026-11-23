#include "domain/graph_ref_filter.h"

#include <algorithm>

namespace cyella {
    namespace {
        constexpr std::string_view local_prefix { "refs/heads/" };

        bool contains(const std::vector<std::string>& names, const std::string_view name) noexcept
        {
            return std::find(names.begin(), names.end(), name) != names.end();
        }

        // 있으면 빼고 없으면 더한다. 뺐으면 false다.
        bool toggle(std::vector<std::string>& names, const std::string_view name)
        {
            if (const auto found { std::find(names.begin(), names.end(), name) }; found != names.end())
            {
                names.erase(found);
                return false;
            }
            names.emplace_back(name);
            return true;
        }
    } // namespace

    graph_ref_mark graph_ref_mark_of(const graph_ref_filter& filter, const std::string_view full_name) noexcept
    {
        if (filter.only.empty() == false)
            return contains(filter.only, full_name) ? graph_ref_mark::only : graph_ref_mark::none;
        return contains(filter.hidden, full_name) ? graph_ref_mark::hidden : graph_ref_mark::none;
    }

    graph_ref_filter settle_graph_ref_filter(const graph_ref_filter& filter, const std::span<const ref> refs, const std::string_view current_branch)
    {
        const auto exists = [&refs](const std::string& name) { return std::any_of(refs.begin(), refs.end(), [&name](const ref& value) { return value.full_name == name; }); };
        const auto is_current = [current_branch](const std::string_view name) {
            return current_branch.empty() == false && name.starts_with(local_prefix) && name.substr(local_prefix.size()) == current_branch;
        };

        graph_ref_filter settled {};
        for (const std::string& name : filter.only)
        {
            if (exists(name))
                settled.only.push_back(name);
        }
        for (const std::string& name : filter.hidden)
        {
            if (exists(name) && is_current(name) == false)
                settled.hidden.push_back(name);
        }
        return settled;
    }

    void toggle_graph_ref_only(graph_ref_filter& filter, const std::string_view full_name)
    {
        if (toggle(filter.only, full_name))
            std::erase(filter.hidden, full_name);
    }

    void toggle_graph_ref_hidden(graph_ref_filter& filter, const std::string_view full_name)
    {
        static_cast<void>(toggle(filter.hidden, full_name));
    }
} // namespace cyella
