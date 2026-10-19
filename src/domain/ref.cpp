#include "domain/ref.h"

#include <algorithm>
#include <unordered_map>

namespace cyella {
    void apply_remote_tags(const std::span<ref> refs, const std::string_view remote, const std::span<const remote_tag> remote_tags)
    {
        std::unordered_map<std::string_view, std::string_view> sha_by_name {};
        sha_by_name.reserve(remote_tags.size());
        for (const remote_tag& tag : remote_tags)
            sha_by_name.emplace(tag.name, tag.sha);

        for (ref& entry : refs)
        {
            if (entry.kind != ref_kind::tag)
                continue;
            const auto found { sha_by_name.find(entry.name) };
            const bool exists { found != sha_by_name.end() && found->second == entry.target_sha };
            const auto listed { std::find(entry.remotes.begin(), entry.remotes.end(), remote) };
            if (exists && listed == entry.remotes.end())
                entry.remotes.emplace_back(remote);
            else if (exists == false && listed != entry.remotes.end())
                entry.remotes.erase(listed);
        }
    }
} // namespace cyella
