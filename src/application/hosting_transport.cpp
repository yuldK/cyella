#include "application/hosting_transport.h"

#include <cctype>

namespace cyella {
    namespace {
        bool equal_ignoring_case(const std::string_view left, const std::string_view right) noexcept
        {
            if (left.size() != right.size())
                return false;
            for (std::size_t index { 0 }; index < left.size(); ++index)
                if (std::tolower(static_cast<unsigned char>(left[index])) != std::tolower(static_cast<unsigned char>(right[index])))
                    return false;
            return true;
        }
    } // namespace

    std::string_view hosting_http_response::header(const std::string_view name) const noexcept
    {
        for (const hosting_http_header& entry : headers)
            if (equal_ignoring_case(entry.name, name))
                return entry.value;
        return {};
    }
} // namespace cyella
