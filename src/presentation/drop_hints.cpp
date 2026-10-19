#include "presentation/drop_hints.h"

namespace cyella {
    namespace {
        std::optional<std::u8string> strip_prefix(const std::u8string& text, const std::u8string_view prefix)
        {
            if (text.size() < prefix.size() || std::u8string_view { text }.substr(0, prefix.size()) != prefix)
                return std::nullopt;
            return text.substr(prefix.size());
        }
    } // namespace

    drop_facts drop_hint_data::facts_for(const drop_party& source, const drop_party& destination) const
    {
        drop_facts facts {};
        if (index == nullptr || source.sha.empty() || destination.sha.empty())
            return facts;
        facts.destination_is_ancestor = index->is_ancestor(destination.sha, source.sha);
        facts.source_is_ancestor = index->is_ancestor(source.sha, destination.sha);
        facts.only_in_source = index->count(destination.sha, source.sha);
        facts.only_in_destination = index->count(source.sha, destination.sha);
        return facts;
    }

    std::optional<std::u8string> drop_hint_data::describe(const std::u8string& source_key, const std::u8string& target_owner) const
    {
        const auto source { branches.find(source_key) };
        if (source == branches.end())
            return std::nullopt;
        drop_party destination {};
        if (const auto key { strip_prefix(target_owner, sidebar_row_owner_prefix) }; key.has_value())
        {
            const auto found { branches.find(*key) };
            if (found == branches.end())
                return std::nullopt;
            destination = found->second;
        }
        else if (const auto label { strip_prefix(target_owner, ref_label_owner_prefix) }; label.has_value())
        {
            const auto found { branches.find(*label) };
            if (found == branches.end())
                return std::nullopt;
            destination = found->second;
        }
        else if (const auto row { strip_prefix(target_owner, graph_row_owner_prefix) }; row.has_value())
        {
            if (index == nullptr)
                return std::nullopt;
            const std::optional<std::size_t> position { index->find(std::string { row->begin(), row->end() }) };
            if (position.has_value() == false)
                return std::nullopt;
            destination.is_commit = true;
            destination.sha = index->sha_at(*position);
        }
        else
            return std::nullopt;
        return describe_drop(source->second, destination, current_branch, facts_for(source->second, destination));
    }
} // namespace cyella
