#include "domain/diagnostic.h"

namespace cyella {
    std::u8string_view diagnostic_severity_name(const diagnostic_severity severity) noexcept
    {
        switch (severity)
        {
        case diagnostic_severity::information:
            return u8"information";
        case diagnostic_severity::warning:
            return u8"warning";
        case diagnostic_severity::error:
            return u8"error";
        }
        return u8"unknown";
    }

    std::u8string_view diagnostic_code_name(const diagnostic_code code) noexcept
    {
        switch (code)
        {
        case diagnostic_code::unknown:
            return u8"unknown";
        case diagnostic_code::operation_failed:
            return u8"operation_failed";
        case diagnostic_code::invalid_process_request:
            return u8"invalid_process_request";
        case diagnostic_code::process_start_failed:
            return u8"process_start_failed";
        case diagnostic_code::process_pipe_failed:
            return u8"process_pipe_failed";
        case diagnostic_code::process_timed_out:
            return u8"process_timed_out";
        case diagnostic_code::process_cancelled:
            return u8"process_cancelled";
        case diagnostic_code::process_output_truncated:
            return u8"process_output_truncated";
        case diagnostic_code::git_tool_not_found:
            return u8"git_tool_not_found";
        case diagnostic_code::git_tool_too_old:
            return u8"git_tool_too_old";
        case diagnostic_code::git_tool_version_unreadable:
            return u8"git_tool_version_unreadable";
        case diagnostic_code::git_tool_path_invalid:
            return u8"git_tool_path_invalid";
        case diagnostic_code::app_settings_invalid:
            return u8"app_settings_invalid";
        case diagnostic_code::app_settings_read_failed:
            return u8"app_settings_read_failed";
        case diagnostic_code::app_settings_write_failed:
            return u8"app_settings_write_failed";
        case diagnostic_code::app_settings_backup_failed:
            return u8"app_settings_backup_failed";
        case diagnostic_code::repository_watch_failed:
            return u8"repository_watch_failed";
        case diagnostic_code::askpass_helper_missing:
            return u8"askpass_helper_missing";
        case diagnostic_code::askpass_server_failed:
            return u8"askpass_server_failed";
        }
        return u8"unknown";
    }
} // namespace cyella
