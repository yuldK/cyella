#include "platform/win32/utf8.h"

#include <windows.h>

#include <limits>

namespace cyella::win32 {
    namespace {
        template<typename value_type>
        utf_conversion_result<value_type> invalid_input_result() noexcept
        {
            return {
                std::nullopt,
                utf_conversion_error {
                    utf_conversion_error_kind::invalid_input,
                    ERROR_NO_UNICODE_TRANSLATION,
                },
            };
        }

        template<typename value_type>
        utf_conversion_result<value_type> system_error_result() noexcept
        {
            return {
                std::nullopt,
                utf_conversion_error {
                    utf_conversion_error_kind::system_error,
                    GetLastError(),
                },
            };
        }
    } // namespace

    utf_conversion_result<std::u8string> utf16_to_utf8(const std::wstring_view input) noexcept
    {
        if (input.empty())
            return { std::u8string {}, std::nullopt };
        if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return invalid_input_result<std::u8string>();

        const int input_length { static_cast<int>(input.size()) };
        const int output_length { WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), input_length, nullptr, 0, nullptr, nullptr) };

        if (output_length == 0)
            return GetLastError() == ERROR_NO_UNICODE_TRANSLATION ? invalid_input_result<std::u8string>() : system_error_result<std::u8string>();

        std::u8string output(static_cast<std::size_t>(output_length), u8'\0');
        const int converted { WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), input_length, reinterpret_cast<char*>(output.data()), output_length, nullptr, nullptr) };

        if (converted != output_length)
            return system_error_result<std::u8string>();
        return { std::move(output), std::nullopt };
    }

    utf_conversion_result<std::wstring> utf8_to_utf16(const std::u8string_view input) noexcept
    {
        if (input.empty())
            return { std::wstring {}, std::nullopt };
        if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return invalid_input_result<std::wstring>();

        const int input_length { static_cast<int>(input.size()) };
        const auto* input_data { reinterpret_cast<const char*>(input.data()) };
        const int output_length { MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input_data, input_length, nullptr, 0) };

        if (output_length == 0)
            return GetLastError() == ERROR_NO_UNICODE_TRANSLATION ? invalid_input_result<std::wstring>() : system_error_result<std::wstring>();

        std::wstring output(static_cast<std::size_t>(output_length), L'\0');
        const int converted { MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input_data, input_length, output.data(), output_length) };

        if (converted != output_length)
            return system_error_result<std::wstring>();
        return { std::move(output), std::nullopt };
    }
} // namespace cyella::win32
