#include "domain/user_profile.h"

#include "domain/app_settings.h"
#include "domain/repository_path.h"
#include "domain/text_catalog.h"

#include <algorithm>
#include <utility>

namespace cyella {
    namespace {
        char8_t ascii_lower(const char8_t value) noexcept
        {
            return value >= u8'A' && value <= u8'Z' ? static_cast<char8_t>(value - u8'A' + u8'a') : value;
        }

        bool same_name(const std::u8string_view left, const std::u8string_view right) noexcept
        {
            return std::ranges::equal(left, right, [](const char8_t a, const char8_t b) { return ascii_lower(a) == ascii_lower(b); });
        }

        bool has_line_break(const std::u8string_view text) noexcept
        {
            return text.find(u8'\n') != std::u8string_view::npos || text.find(u8'\r') != std::u8string_view::npos;
        }

        bool has_edge_space(const std::u8string_view text) noexcept
        {
            return text.empty() == false && (text.front() == u8' ' || text.back() == u8' ' || text.front() == u8'\t' || text.back() == u8'\t');
        }

        bool has_space(const std::u8string_view text) noexcept
        {
            return text.find_first_of(u8" \t\r\n") != std::u8string_view::npos;
        }

        // 접두 비교용 열쇠다. 끝 구분자는 떼고 대소문자·구분자는 맞춘다.
        std::u8string prefix_key(const std::u8string_view path)
        {
            return repository_path_key(path);
        }
    } // namespace

    std::optional<std::size_t> find_profile_index(const std::vector<user_profile>& profiles, const std::u8string_view name)
    {
        for (std::size_t index { 0 }; index < profiles.size(); ++index)
            if (same_name(profiles[index].name, name))
                return index;
        return std::nullopt;
    }

    std::u8string check_user_profile(const user_profile& profile, const std::vector<user_profile>& existing, const std::optional<std::size_t> editing)
    {
        if (profile.name.empty())
            return tr(u8"profile.name_required");
        if (profile.name.size() > maximum_profile_name_length || has_line_break(profile.name) || has_edge_space(profile.name))
            return tr(u8"profile.name_invalid");
        for (std::size_t index { 0 }; index < existing.size(); ++index)
            if (index != editing && same_name(existing[index].name, profile.name))
                return tr(u8"profile.name_duplicate");
        if (profile.user_name.empty() || has_line_break(profile.user_name) || has_edge_space(profile.user_name))
            return tr(u8"profile.user_name_invalid");
        if (profile.user_email.empty() || has_space(profile.user_email))
            return tr(u8"profile.user_email_spaces");
        const std::size_t at { profile.user_email.find(u8'@') };
        if (at == std::u8string::npos || at == 0 || at + 1 >= profile.user_email.size())
            return tr(u8"profile.user_email_format");
        if (has_line_break(profile.signing_key))
            return tr(u8"profile.signing_key_multiline");
        return {};
    }

    bool path_is_under_prefix(const std::u8string_view path, const std::u8string_view prefix)
    {
        const std::u8string key { prefix_key(path) };
        const std::u8string base { prefix_key(prefix) };
        if (base.empty() || key.size() < base.size() || key.compare(0, base.size(), base) != 0)
            return false;
        if (key.size() == base.size() || base.back() == u8'\\')
            return true;
        return key[base.size()] == u8'\\';
    }

    std::optional<std::size_t> match_profile_rule(const std::vector<profile_rule>& rules, const std::vector<user_profile>& profiles, const std::u8string_view repository_path)
    {
        std::optional<std::size_t> best {};
        std::size_t best_length { 0 };
        for (std::size_t index { 0 }; index < rules.size(); ++index)
        {
            const profile_rule& rule { rules[index] };
            if (rule.path_prefix.empty() || find_profile_index(profiles, rule.profile).has_value() == false || path_is_under_prefix(repository_path, rule.path_prefix) == false)
                continue;
            const std::size_t length { prefix_key(rule.path_prefix).size() };
            if (best.has_value() == false || length > best_length)
            {
                best = index;
                best_length = length;
            }
        }
        return best;
    }

    std::u8string check_profile_rule(const profile_rule& rule, const std::vector<user_profile>& profiles)
    {
        if (rule.path_prefix.empty() || has_line_break(rule.path_prefix))
            return tr(u8"profile.rule_path_required");
        if (rule.path_prefix.find(u8'\\') == std::u8string::npos && rule.path_prefix.find(u8'/') == std::u8string::npos)
            return tr(u8"profile.rule_path_not_full");
        if (find_profile_index(profiles, rule.profile).has_value() == false)
            return tr(u8"profile.rule_profile_required");
        return {};
    }

    void upsert_user_profile(app_settings& settings, const std::optional<std::size_t> index, user_profile profile)
    {
        if (index.has_value() && *index < settings.profiles.size())
        {
            const std::u8string old_name { settings.profiles[*index].name };
            settings.profiles[*index] = profile;
            if (old_name != profile.name)
            {
                for (profile_rule& rule : settings.profile_rules)
                    if (rule.profile == old_name)
                        rule.profile = profile.name;
                for (profile_assignment& assignment : settings.profile_assignments)
                    if (assignment.profile == old_name)
                        assignment.profile = profile.name;
            }
            return;
        }
        settings.profiles.push_back(std::move(profile));
    }

    bool remove_user_profile(app_settings& settings, const std::size_t index)
    {
        if (index >= settings.profiles.size())
            return false;
        const std::u8string name { settings.profiles[index].name };
        settings.profiles.erase(settings.profiles.begin() + static_cast<std::ptrdiff_t>(index));
        std::erase_if(settings.profile_rules, [&name](const profile_rule& rule) { return rule.profile == name; });
        std::erase_if(settings.profile_assignments, [&name](const profile_assignment& assignment) { return assignment.profile == name; });
        return true;
    }

    void upsert_profile_rule(app_settings& settings, profile_rule rule)
    {
        rule.path_prefix = normalize_repository_path(rule.path_prefix);
        const std::u8string key { prefix_key(rule.path_prefix) };
        for (profile_rule& existing : settings.profile_rules)
            if (prefix_key(existing.path_prefix) == key)
            {
                existing.profile = std::move(rule.profile);
                return;
            }
        settings.profile_rules.push_back(std::move(rule));
    }

    bool remove_profile_rule(app_settings& settings, const std::size_t index)
    {
        if (index >= settings.profile_rules.size())
            return false;
        settings.profile_rules.erase(settings.profile_rules.begin() + static_cast<std::ptrdiff_t>(index));
        return true;
    }

    void assign_profile(app_settings& settings, const std::u8string_view repository_path, std::u8string profile)
    {
        const std::u8string key { repository_path_key(repository_path) };
        for (profile_assignment& assignment : settings.profile_assignments)
            if (repository_path_key(assignment.path) == key)
            {
                assignment.profile = std::move(profile);
                return;
            }
        settings.profile_assignments.push_back(profile_assignment { .path = normalize_repository_path(repository_path), .profile = std::move(profile) });
    }

    std::optional<std::u8string> assigned_profile(const app_settings& settings, const std::u8string_view repository_path)
    {
        const std::u8string key { repository_path_key(repository_path) };
        for (const profile_assignment& assignment : settings.profile_assignments)
            if (repository_path_key(assignment.path) == key)
                return assignment.profile;
        return std::nullopt;
    }
} // namespace cyella
