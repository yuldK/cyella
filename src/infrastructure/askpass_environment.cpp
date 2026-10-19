#include "infrastructure/askpass_environment.h"

#include <optional>
#include <utility>

namespace cyella {
    namespace {
        process_environment_override make_override(const std::u8string_view name, const std::u8string& value, const bool enabled)
        {
            process_environment_override entry {};
            entry.name = std::u8string { name };
            if (enabled && value.empty() == false)
                entry.value = value;
            return entry;
        }
    } // namespace

    std::vector<process_environment_override> askpass_session_overrides(const bool helper_present, const askpass_session& session)
    {
        std::vector<process_environment_override> overrides {};
        overrides.push_back(make_override(u8"CYELLA_ASKPASS_PIPE", session.pipe_name, helper_present));
        overrides.push_back(make_override(u8"CYELLA_ASKPASS_TOKEN", session.token, helper_present));
        overrides.push_back(make_override(u8"CYELLA_ASKPASS_OPERATION", session.operation_id, helper_present));
        return overrides;
    }
} // namespace cyella
