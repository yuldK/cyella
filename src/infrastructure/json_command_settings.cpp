#include "infrastructure/json_command_settings.h"

#include "domain/diagnostic.h"
#include "domain/repository_path.h"
#include "domain/text_catalog.h"
#include "domain/user_profile.h"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace cyella {
    namespace {
        using json = nlohmann::ordered_json;

        std::string as_string(const std::u8string_view value)
        {
            return std::string { reinterpret_cast<const char*>(value.data()), value.size() };
        }

        std::u8string as_u8string(const std::string_view value)
        {
            return std::u8string { reinterpret_cast<const char8_t*>(value.data()), value.size() };
        }

        void warn(app_settings_load_result& result, std::u8string message)
        {
            diagnostic value {};
            value.code = diagnostic_code::app_settings_invalid;
            value.severity = diagnostic_severity::warning;
            value.message = std::move(message);
            result.diagnostics.push_back(std::move(value));
        }

        std::u8string read_string(const json& value, const char* const key)
        {
            const auto found { value.find(key) };
            if (found == value.end() || found->is_string() == false)
                return {};
            return as_u8string(found->get_ref<const std::string&>());
        }

        // 배열이어야 하는 키다. 배열이 아니면 경고하고 값이 없다.
        const json* array_of(const json& root, const char* const key, app_settings_load_result& result)
        {
            const auto found { root.find(key) };
            if (found == root.end() || found->is_null())
                return nullptr;
            if (found->is_array() == false)
            {
                warn(result, tr(u8"app_settings.key_not_array", { { u8"key", as_u8string(key) } }));
                return nullptr;
            }
            return &*found;
        }

        void parse_profiles(const json& root, app_settings_load_result& result)
        {
            const json* const source { array_of(root, "profiles", result) };
            if (source == nullptr)
                return;
            std::size_t dropped { 0 };
            for (const json& entry : *source)
            {
                if (entry.is_object() == false)
                {
                    ++dropped;
                    continue;
                }
                user_profile profile {};
                profile.name = read_string(entry, "name");
                profile.user_name = read_string(entry, "user_name");
                profile.user_email = read_string(entry, "user_email");
                profile.signing_key = read_string(entry, "signing_key");
                // 이름이 없거나 같은 이름이 앞에 있으면 규칙과 적용 기록이 어느 프로필을 가리키는지 알 수 없다.
                if (profile.name.empty() || find_profile_index(result.settings.profiles, profile.name).has_value())
                {
                    ++dropped;
                    continue;
                }
                result.settings.profiles.push_back(std::move(profile));
            }
            if (dropped > 0)
                warn(result, tr(u8"app_settings.profiles_invalid"));
        }

        void parse_profile_rules(const json& root, app_settings_load_result& result)
        {
            const json* const source { array_of(root, "profile_rules", result) };
            if (source == nullptr)
                return;
            std::size_t dropped { 0 };
            for (const json& entry : *source)
            {
                profile_rule rule {};
                if (entry.is_object())
                {
                    rule.path_prefix = normalize_repository_path(read_string(entry, "path_prefix"));
                    rule.profile = read_string(entry, "profile");
                }
                if (rule.path_prefix.empty() || find_profile_index(result.settings.profiles, rule.profile).has_value() == false)
                {
                    ++dropped;
                    continue;
                }
                result.settings.profile_rules.push_back(std::move(rule));
            }
            if (dropped > 0)
                warn(result, tr(u8"app_settings.profile_rules_invalid"));
        }

        void parse_profile_assignments(const json& root, app_settings_load_result& result)
        {
            const json* const source { array_of(root, "profile_assignments", result) };
            if (source == nullptr)
                return;
            for (const json& entry : *source)
            {
                if (entry.is_object() == false)
                    continue;
                profile_assignment assignment {};
                assignment.path = normalize_repository_path(read_string(entry, "path"));
                assignment.profile = read_string(entry, "profile");
                // 지워진 프로필을 가리키는 기록은 조용히 버린다 (프로필을 지울 때 같이 지우지만 파일을 손으로 고쳤을 수 있다).
                if (assignment.path.empty() || find_profile_index(result.settings.profiles, assignment.profile).has_value() == false)
                    continue;
                assign_profile(result.settings, assignment.path, std::move(assignment.profile));
            }
        }

        void parse_shortcuts(const json& root, app_settings_load_result& result)
        {
            const auto found { root.find("shortcuts") };
            if (found == root.end() || found->is_null())
                return;
            if (found->is_object() == false)
            {
                warn(result, tr(u8"app_settings.shortcuts_not_object"));
                return;
            }
            std::size_t dropped { 0 };
            for (const auto& [id, chords] : found->items())
            {
                if (chords.is_array() == false)
                {
                    ++dropped;
                    continue;
                }
                std::vector<std::u8string> list {};
                for (const json& chord : chords)
                {
                    if (chord.is_string() == false)
                    {
                        ++dropped;
                        continue;
                    }
                    list.push_back(as_u8string(chord.get_ref<const std::string&>()));
                }
                result.settings.shortcuts[id] = std::move(list);
            }
            if (dropped > 0)
                warn(result, tr(u8"app_settings.shortcuts_invalid"));
        }

        void parse_command_history(const json& root, app_settings_load_result& result)
        {
            const json* const source { array_of(root, "command_history", result) };
            if (source == nullptr)
                return;
            for (const json& entry : *source)
                if (entry.is_string() && entry.get_ref<const std::string&>().empty() == false && result.settings.command_history.size() < command_history_capacity)
                    result.settings.command_history.push_back(entry.get<std::string>());
        }

        // 값이 있으면 쓰고 없으면 키를 지운다.
        void write_or_erase(json& root, const char* const key, json value, const bool present)
        {
            if (present)
                root[key] = std::move(value);
            else
                root.erase(key);
        }
    } // namespace

    void parse_command_settings(const json& root, app_settings_load_result& result)
    {
        // 규칙과 적용 기록이 프로필을 가리키므로 프로필을 먼저 읽는다.
        parse_profiles(root, result);
        parse_profile_rules(root, result);
        parse_profile_assignments(root, result);
        parse_shortcuts(root, result);
        parse_command_history(root, result);
    }

    void serialize_command_settings(const app_settings& settings, json& root)
    {
        json profiles { json::array() };
        for (const user_profile& profile : settings.profiles)
        {
            json entry { json::object() };
            entry["name"] = as_string(profile.name);
            entry["user_name"] = as_string(profile.user_name);
            entry["user_email"] = as_string(profile.user_email);
            // 서명 키는 커밋 서명 쪽의 값이다. 비어 있으면 키를 쓰지 않는다.
            if (profile.signing_key.empty() == false)
                entry["signing_key"] = as_string(profile.signing_key);
            profiles.push_back(std::move(entry));
        }
        write_or_erase(root, "profiles", std::move(profiles), settings.profiles.empty() == false);

        json rules { json::array() };
        for (const profile_rule& rule : settings.profile_rules)
        {
            json entry { json::object() };
            entry["path_prefix"] = as_string(rule.path_prefix);
            entry["profile"] = as_string(rule.profile);
            rules.push_back(std::move(entry));
        }
        write_or_erase(root, "profile_rules", std::move(rules), settings.profile_rules.empty() == false);

        json assignments { json::array() };
        for (const profile_assignment& assignment : settings.profile_assignments)
        {
            json entry { json::object() };
            entry["path"] = as_string(assignment.path);
            entry["profile"] = as_string(assignment.profile);
            assignments.push_back(std::move(entry));
        }
        write_or_erase(root, "profile_assignments", std::move(assignments), settings.profile_assignments.empty() == false);

        json shortcuts { json::object() };
        for (const auto& [id, chords] : settings.shortcuts)
        {
            json list { json::array() };
            for (const std::u8string& chord : chords)
                list.push_back(as_string(chord));
            shortcuts[id] = std::move(list);
        }
        write_or_erase(root, "shortcuts", std::move(shortcuts), settings.shortcuts.empty() == false);

        json history { json::array() };
        for (const std::string& command : settings.command_history)
            history.push_back(command);
        write_or_erase(root, "command_history", std::move(history), settings.command_history.empty() == false);
    }
} // namespace cyella
