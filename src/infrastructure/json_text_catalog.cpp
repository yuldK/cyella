#include "infrastructure/json_text_catalog.h"

#include <nlohmann/json.hpp>

#include <set>
#include <utility>

namespace cyella {
    namespace {
        std::u8string as_u8string(const std::string_view value)
        {
            return std::u8string { reinterpret_cast<const char8_t*>(value.data()), value.size() };
        }
    } // namespace

    text_catalog_parse_result parse_text_catalog_json(const std::string_view source, const ui_language language)
    {
        text_catalog_parse_result result {};
        result.catalog.language = language;

        // nlohmann은 같은 키를 조용히 뒤의 값으로 덮으므로 최상위 키를 읽는 순간에 겹침을 센다.
        std::set<std::string, std::less<>> seen {};
        std::vector<std::u8string> duplicates {};
        const auto callback = [&seen, &duplicates](const int depth, const nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
            if (depth == 1 && event == nlohmann::json::parse_event_t::key && parsed.is_string())
            {
                const std::string& key { parsed.get_ref<const std::string&>() };
                if (seen.insert(key).second == false)
                    duplicates.push_back(as_u8string(key));
            }
            return true;
        };

        nlohmann::json root {};
        try
        {
            root = nlohmann::json::parse(source.begin(), source.end(), callback);
        }
        catch (const nlohmann::json::parse_error& error)
        {
            result.problems.push_back(u8"invalid JSON: " + as_u8string(error.what()));
            return result;
        }
        if (root.is_object() == false)
        {
            result.problems.push_back(u8"the root is not an object");
            return result;
        }

        for (const std::u8string& key : duplicates)
            result.problems.push_back(u8"duplicate key: " + key);
        for (const auto& [key, value] : root.items())
        {
            if (value.is_string() == false)
            {
                result.problems.push_back(u8"not a string: " + as_u8string(key));
                continue;
            }
            result.catalog.entries.emplace(as_u8string(key), as_u8string(value.get_ref<const std::string&>()));
        }
        return result;
    }
} // namespace cyella
