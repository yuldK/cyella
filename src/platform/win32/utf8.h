#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cyella::win32 {
    enum class utf_conversion_error_kind
    {
        invalid_input,
        system_error,
    };

    struct utf_conversion_error
    {
        utf_conversion_error_kind kind { utf_conversion_error_kind::system_error };
        unsigned long native_error { 0 };
    };

    template<typename value_type>
    struct utf_conversion_result
    {
        std::optional<value_type> value {};
        std::optional<utf_conversion_error> error {};
    };

    [[nodiscard]] utf_conversion_result<std::u8string> utf16_to_utf8(std::wstring_view input) noexcept;
    [[nodiscard]] utf_conversion_result<std::wstring> utf8_to_utf16(std::u8string_view input) noexcept;
} // namespace cyella::win32
