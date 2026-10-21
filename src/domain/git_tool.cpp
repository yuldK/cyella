#include "domain/git_tool.h"

#include "domain/text_catalog.h"

#include <charconv>
#include <limits>

namespace cyella {
    namespace {
        void append_ascii_number(std::u8string& target, const std::uint32_t value)
        {
            std::array<char, std::numeric_limits<std::uint32_t>::digits10 + 2> buffer {};
            const auto result { std::to_chars(buffer.data(), buffer.data() + buffer.size(), value) };
            for (const char* current = buffer.data(); current != result.ptr; ++current)
                target.push_back(static_cast<char8_t>(*current));
        }
    } // namespace

    bool git_tool_info::usable() const noexcept
    {
        return availability == git_tool_availability::available && executable.empty() == false;
    }

    git_tool_version minimum_supported_git_version() noexcept
    {
        return { minimum_git_version[0], minimum_git_version[1], minimum_git_version[2] };
    }

    std::u8string_view git_tool_availability_name(const git_tool_availability availability) noexcept
    {
        switch (availability)
        {
        case git_tool_availability::unknown:
            return u8"unknown";
        case git_tool_availability::not_found:
            return u8"not_found";
        case git_tool_availability::path_invalid:
            return u8"path_invalid";
        case git_tool_availability::version_unreadable:
            return u8"version_unreadable";
        case git_tool_availability::too_old:
            return u8"too_old";
        case git_tool_availability::available:
            return u8"available";
        }
        return u8"unknown";
    }

    std::u8string git_tool_unavailable_message(const git_tool_availability availability)
    {
        switch (availability)
        {
        case git_tool_availability::unknown:
            return tr(u8"git_tool.unknown");
        case git_tool_availability::not_found:
            return tr(u8"git_tool.not_found");
        case git_tool_availability::path_invalid:
            return tr(u8"git_tool.path_invalid");
        case git_tool_availability::version_unreadable:
            return tr(u8"git_tool.version_unreadable");
        case git_tool_availability::too_old:
            return tr(u8"git_tool.too_old");
        case git_tool_availability::available:
            return tr(u8"git_tool.available");
        }
        return tr(u8"git_tool.unavailable");
    }

    std::u8string format_git_tool_version(const git_tool_version& version)
    {
        std::u8string result {};
        append_ascii_number(result, version.major);
        result.push_back(u8'.');
        append_ascii_number(result, version.minor);
        result.push_back(u8'.');
        append_ascii_number(result, version.patch);
        return result;
    }
} // namespace cyella
