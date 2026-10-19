#include "domain/process_execution.h"

#include <algorithm>

namespace cyella {
    bool process_result::succeeded() const noexcept
    {
        return completion == process_completion::exited && exit_code.has_value() && *exit_code == 0 && has_errors() == false;
    }

    bool process_result::has_errors() const noexcept
    {
        return std::ranges::any_of(diagnostics, [](const diagnostic& value) { return value.severity == diagnostic_severity::error; });
    }

    bool process_result::has_warnings() const noexcept
    {
        return std::ranges::any_of(diagnostics, [](const diagnostic& value) { return value.severity == diagnostic_severity::warning; });
    }

    std::u8string_view process_stream_name(const process_stream stream) noexcept
    {
        switch (stream)
        {
        case process_stream::standard_output:
            return u8"stdout";
        case process_stream::standard_error:
            return u8"stderr";
        }
        return u8"unknown";
    }

    std::u8string_view process_completion_name(const process_completion completion) noexcept
    {
        switch (completion)
        {
        case process_completion::exited:
            return u8"exited";
        case process_completion::start_failed:
            return u8"start_failed";
        case process_completion::timed_out:
            return u8"timed_out";
        case process_completion::cancelled:
            return u8"cancelled";
        case process_completion::invalid_request:
            return u8"invalid_request";
        case process_completion::internal_error:
            return u8"internal_error";
        }
        return u8"unknown";
    }
} // namespace cyella
