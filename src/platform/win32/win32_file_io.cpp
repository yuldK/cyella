#include "platform/win32/win32_file_io.h"

#include "platform/win32/utf8.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace cyella::win32 {
    namespace {
        std::uint32_t last_error_or(const std::uint32_t fallback) noexcept
        {
            const DWORD error { GetLastError() };
            return error == ERROR_SUCCESS ? fallback : static_cast<std::uint32_t>(error);
        }

        bool starts_with(const std::wstring_view value, const std::wstring_view prefix) noexcept
        {
            return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
        }

        bool is_ascii_drive_path(const std::wstring_view value) noexcept
        {
            if (value.size() < 3 || value[1] != L':' || value[2] != L'\\')
                return false;
            return (value[0] >= L'A' && value[0] <= L'Z') || (value[0] >= L'a' && value[0] <= L'z');
        }

        prepared_path full_path(std::wstring source)
        {
            for (wchar_t& value : source)
                if (value == L'/')
                    value = L'\\';

            SetLastError(ERROR_SUCCESS);
            DWORD capacity { GetFullPathNameW(source.c_str(), 0, nullptr, nullptr) };
            if (capacity == 0)
                return { std::nullopt, last_error_or(ERROR_INVALID_NAME) };

            while (true)
            {
                std::wstring output(static_cast<std::size_t>(capacity), L'\0');
                SetLastError(ERROR_SUCCESS);
                const DWORD length { GetFullPathNameW(source.c_str(), capacity, output.data(), nullptr) };
                if (length == 0)
                    return { std::nullopt, last_error_or(ERROR_INVALID_NAME) };
                if (length < capacity)
                {
                    output.resize(static_cast<std::size_t>(length));
                    return { std::move(output), ERROR_SUCCESS };
                }
                capacity = length;
            }
        }

        bool is_not_found_error(const std::uint32_t error) noexcept
        {
            switch (error)
            {
            case ERROR_FILE_NOT_FOUND:
            case ERROR_PATH_NOT_FOUND:
            case ERROR_INVALID_DRIVE:
            case ERROR_BAD_NETPATH:
            case ERROR_BAD_NET_NAME:
                return true;
            default:
                return false;
            }
        }

        settings_file_read_result read_impl(const std::u8string_view path)
        {
            prepared_path prepared { prepare_file_path(path) };
            if (prepared.value.has_value() == false)
                return { settings_file_read_state::failed, {}, prepared.native_error };

            SetLastError(ERROR_SUCCESS);
            unique_handle file {
                CreateFileW(
                    prepared.value->c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr),
            };
            if (file.valid() == false)
            {
                const std::uint32_t error { last_error_or(ERROR_READ_FAULT) };
                return {
                    is_not_found_error(error) ? settings_file_read_state::not_found : settings_file_read_state::failed,
                    {},
                    error,
                };
            }

            LARGE_INTEGER size {};
            SetLastError(ERROR_SUCCESS);
            if (GetFileSizeEx(file.get(), &size) == FALSE)
                return { settings_file_read_state::failed, {}, last_error_or(ERROR_READ_FAULT) };
            if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > static_cast<unsigned long long>(std::numeric_limits<std::size_t>::max()))
                return { settings_file_read_state::failed, {}, ERROR_FILE_TOO_LARGE };

            std::u8string bytes(static_cast<std::size_t>(size.QuadPart), u8'\0');
            std::size_t offset { 0 };
            while (offset < bytes.size())
            {
                const std::size_t remaining { bytes.size() - offset };
                const DWORD requested { static_cast<DWORD>(std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max())) };
                DWORD read_count { 0 };
                SetLastError(ERROR_SUCCESS);
                if (ReadFile(file.get(), bytes.data() + offset, requested, &read_count, nullptr) == FALSE)
                    return { settings_file_read_state::failed, {}, last_error_or(ERROR_READ_FAULT) };
                // 읽는 사이 파일이 줄었다. 일부만 믿지 않는다.
                if (read_count == 0)
                    return { settings_file_read_state::failed, {}, ERROR_HANDLE_EOF };
                offset += static_cast<std::size_t>(read_count);
            }
            return { settings_file_read_state::available, std::move(bytes), std::nullopt };
        }
    } // namespace

    prepared_path prepare_file_path(const std::u8string_view path)
    {
        const auto converted { utf8_to_utf16(path) };
        if (converted.value.has_value() == false)
            return { std::nullopt, static_cast<std::uint32_t>(converted.error->native_error) };
        if (converted.value->empty() || converted.value->find(L'\0') != std::wstring::npos)
            return { std::nullopt, ERROR_INVALID_NAME };

        prepared_path absolute { full_path(*converted.value) };
        if (absolute.value.has_value() == false)
            return absolute;

        std::wstring& value { *absolute.value };
        if (starts_with(value, L"\\\\.\\"))
            return { std::nullopt, ERROR_INVALID_NAME };
        if (starts_with(value, L"\\\\?\\UNC\\"))
            return absolute;
        if (starts_with(value, L"\\\\?\\"))
            return is_ascii_drive_path(std::wstring_view { value }.substr(4)) ? std::move(absolute) : prepared_path { std::nullopt, ERROR_INVALID_NAME };
        if (starts_with(value, L"\\\\"))
        {
            value.erase(0, 2);
            value.insert(0, L"\\\\?\\UNC\\");
            return absolute;
        }
        if (is_ascii_drive_path(value))
        {
            value.insert(0, L"\\\\?\\");
            return absolute;
        }
        return { std::nullopt, ERROR_INVALID_NAME };
    }

    bool create_directory_tree(const std::wstring& path, std::uint32_t& native_error) noexcept
    {
        try
        {
            SetLastError(ERROR_SUCCESS);
            if (CreateDirectoryW(path.c_str(), nullptr) != FALSE)
                return true;

            const std::uint32_t error { last_error_or(ERROR_CANNOT_MAKE) };
            if (error == ERROR_ALREADY_EXISTS)
            {
                // 같은 이름의 파일이면 폴더가 아니다.
                const DWORD attributes { GetFileAttributesW(path.c_str()) };
                if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                    return true;
                native_error = ERROR_DIRECTORY;
                return false;
            }
            if (error != ERROR_PATH_NOT_FOUND)
            {
                native_error = error;
                return false;
            }

            // 상위가 없다. 한 단계 위를 먼저 만들고 다시 시도한다.
            const std::size_t separator { path.find_last_of(L'\\') };
            if (separator == std::wstring::npos || separator == 0)
            {
                native_error = error;
                return false;
            }
            if (create_directory_tree(path.substr(0, separator), native_error) == false)
                return false;

            SetLastError(ERROR_SUCCESS);
            if (CreateDirectoryW(path.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS)
                return true;
            native_error = last_error_or(ERROR_CANNOT_MAKE);
            return false;
        }
        catch (...)
        {
            native_error = ERROR_NOT_ENOUGH_MEMORY;
            return false;
        }
    }

    settings_file_read_result read_whole_file(const std::u8string_view path) noexcept
    {
        try
        {
            return read_impl(path);
        }
        catch (...)
        {
            return { settings_file_read_state::failed, {}, ERROR_NOT_ENOUGH_MEMORY };
        }
    }
} // namespace cyella::win32
