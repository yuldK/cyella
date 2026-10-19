#include "platform/win32/win32_file_probe.h"

#include "platform/win32/utf8.h"

#include <windows.h>

#include <cstddef>
#include <utility>
#include <vector>

namespace cyella::win32 {
    namespace {
        bool is_separator(const wchar_t value) noexcept
        {
            return value == L'\\' || value == L'/';
        }

        bool is_ascii_letter(const wchar_t value) noexcept
        {
            return (value >= L'A' && value <= L'Z') || (value >= L'a' && value <= L'z');
        }

        bool starts_with_extended_prefix(const std::wstring_view value) noexcept
        {
            return value.size() >= 4 && value[0] == L'\\' && value[1] == L'\\' && (value[2] == L'?' || value[2] == L'.') && value[3] == L'\\';
        }

        // MAX_PATH를 넘는 경로에서도 조회가 실패하지 않도록 확장 접두어를 붙인다.
        std::wstring path_for_file_api(std::wstring path)
        {
            for (wchar_t& value : path)
                if (value == L'/')
                    value = L'\\';

            if (starts_with_extended_prefix(path))
                return path;
            if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\')
            {
                std::wstring extended { L"\\\\?\\UNC\\" };
                extended.append(std::wstring_view { path }.substr(2));
                return extended;
            }
            if (path.size() >= 3 && is_ascii_letter(path[0]) && path[1] == L':' && is_separator(path[2]))
            {
                std::wstring extended { L"\\\\?\\" };
                extended.append(path);
                return extended;
            }
            return path;
        }

        void append_program_files_directory(std::vector<std::u8string>& directories, const std::u8string_view name)
        {
            std::u8string value { read_environment_variable(name) };
            if (value.empty())
                return;
            for (const std::u8string& existing : directories)
                if (existing == value)
                    return;
            directories.push_back(std::move(value));
        }

        class win32_file_probe final : public file_probe
        {
        public:
            [[nodiscard]] path_kind probe(const std::u8string_view absolute_path) const noexcept override
            {
                try
                {
                    if (absolute_path.empty())
                        return path_kind::missing;

                    const auto wide { utf8_to_utf16(absolute_path) };
                    if (wide.value.has_value() == false)
                        return path_kind::missing;

                    const std::wstring api_path { path_for_file_api(*wide.value) };
                    const DWORD attributes { GetFileAttributesW(api_path.c_str()) };
                    if (attributes == INVALID_FILE_ATTRIBUTES)
                        return path_kind::missing;
                    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? path_kind::directory : path_kind::file;
                }
                catch (...)
                {
                    return path_kind::missing;
                }
            }
        };
    } // namespace

    std::unique_ptr<file_probe> make_file_probe()
    {
        return std::make_unique<win32_file_probe>();
    }

    std::u8string read_environment_variable(const std::u8string_view name) noexcept
    {
        try
        {
            const auto wide_name { utf8_to_utf16(name) };
            if (wide_name.value.has_value() == false)
                return {};

            SetLastError(ERROR_SUCCESS);
            DWORD capacity { GetEnvironmentVariableW(wide_name.value->c_str(), nullptr, 0) };
            if (capacity == 0)
                return {};

            while (true)
            {
                std::wstring buffer(static_cast<std::size_t>(capacity), L'\0');
                const DWORD length { GetEnvironmentVariableW(wide_name.value->c_str(), buffer.data(), capacity) };
                if (length == 0)
                    return {};
                if (length < capacity)
                {
                    buffer.resize(static_cast<std::size_t>(length));
                    const auto value { utf16_to_utf8(buffer) };
                    return value.value.has_value() ? std::move(*value.value) : std::u8string {};
                }
                // 읽는 사이 값이 길어졌다. 새 길이로 다시 읽는다.
                capacity = length;
            }
        }
        catch (...)
        {
            return {};
        }
    }

    git_tool_environment current_git_tool_environment() noexcept
    {
        try
        {
            git_tool_environment environment {};
            environment.path_environment = read_environment_variable(u8"PATH");
            append_program_files_directory(environment.program_files_directories, u8"ProgramFiles");
            append_program_files_directory(environment.program_files_directories, u8"ProgramFiles(x86)");
            append_program_files_directory(environment.program_files_directories, u8"ProgramW6432");
            return environment;
        }
        catch (...)
        {
            return {};
        }
    }
} // namespace cyella::win32
