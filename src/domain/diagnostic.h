#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cyella {
    enum class diagnostic_severity
    {
        information,
        warning,
        error,
    };

    // 앱이 내는 진단의 코드다.
    enum class diagnostic_code
    {
        unknown,
        operation_failed,
        invalid_process_request,
        process_start_failed,
        process_pipe_failed,
        process_timed_out,
        process_cancelled,
        process_output_truncated,
        git_tool_not_found,
        git_tool_too_old,
        git_tool_version_unreadable,
        git_tool_path_invalid,
        app_settings_invalid,
        app_settings_read_failed,
        app_settings_write_failed,
        app_settings_backup_failed,
        repository_watch_failed,
        // 인증 도우미(`cyella-askpass.exe`)를 앱 옆에서 찾지 못했다. 인증이 필요한 작업은 즉시 실패한다.
        askpass_helper_missing,
        // 도우미가 접속할 named pipe 서버를 열지 못했다. 인증이 필요한 작업은 즉시 실패한다.
        askpass_server_failed,
    };

    struct diagnostic
    {
        diagnostic_code code { diagnostic_code::unknown };
        diagnostic_severity severity { diagnostic_severity::error };
        std::u8string message {};
        std::optional<std::uint32_t> native_error {};
    };

    [[nodiscard]] std::u8string_view diagnostic_severity_name(diagnostic_severity severity) noexcept;
    [[nodiscard]] std::u8string_view diagnostic_code_name(diagnostic_code code) noexcept;
} // namespace cyella
