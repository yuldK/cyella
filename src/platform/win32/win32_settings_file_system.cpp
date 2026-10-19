#include "platform/win32/win32_settings_file_system.h"

#include "platform/win32/win32_file_io.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace cyella::win32 {
    namespace {
        struct temporary_file
        {
            unique_handle handle {};
            std::wstring path {};
            std::uint32_t native_error { ERROR_CANNOT_MAKE };
        };

        std::atomic_uint64_t temporary_file_sequence { 0 };

        std::uint32_t last_error_or(const std::uint32_t fallback) noexcept
        {
            const DWORD error { GetLastError() };
            return error == ERROR_SUCCESS ? fallback : static_cast<std::uint32_t>(error);
        }

        std::wstring temporary_file_name(const std::wstring_view target_path, const std::uint64_t sequence)
        {
            const std::size_t separator { target_path.find_last_of(L'\\') };
            std::wstring value { target_path.substr(0, separator + 1) };
            value.append(L".cyella-");
            value.append(std::to_wstring(GetCurrentProcessId()));
            value.push_back(L'-');
            value.append(std::to_wstring(sequence));
            value.append(L".tmp");
            return value;
        }

        temporary_file create_temporary_file(const std::wstring_view target_path)
        {
            for (std::size_t attempt = 0; attempt < 100; ++attempt)
            {
                const std::uint64_t sequence { temporary_file_sequence.fetch_add(1, std::memory_order_relaxed) };
                std::wstring path { temporary_file_name(target_path, sequence) };
                SetLastError(ERROR_SUCCESS);
                unique_handle handle { CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED, nullptr) };
                if (handle.valid())
                    return { std::move(handle), std::move(path), ERROR_SUCCESS };

                const std::uint32_t error { last_error_or(ERROR_CANNOT_MAKE) };
                if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
                    return { {}, {}, error };
            }
            return { {}, {}, ERROR_FILE_EXISTS };
        }

        settings_file_commit_result write_bytes(const HANDLE file, const std::u8string_view bytes)
        {
            std::size_t offset { 0 };
            while (offset < bytes.size())
            {
                const std::size_t remaining { bytes.size() - offset };
                const DWORD requested { static_cast<DWORD>(std::min<std::size_t>(remaining, std::numeric_limits<DWORD>::max())) };
                DWORD written { 0 };
                SetLastError(ERROR_SUCCESS);
                if (WriteFile(file, bytes.data() + offset, requested, &written, nullptr) == FALSE)
                    return { settings_file_commit_failure::write, last_error_or(ERROR_WRITE_FAULT) };
                if (written == 0)
                    return { settings_file_commit_failure::write, ERROR_WRITE_FAULT };
                offset += static_cast<std::size_t>(written);
            }
            return {};
        }

        void delete_temporary_file(const std::wstring& path) noexcept
        {
            if (path.empty() == false)
                DeleteFileW(path.c_str());
        }

        settings_file_commit_result atomic_commit_impl(const std::u8string_view path, const std::u8string_view bytes)
        {
            prepared_path prepared { prepare_file_path(path) };
            if (prepared.value.has_value() == false)
                return { settings_file_commit_failure::write, prepared.native_error };

            // 폴더가 지워졌어도 저장이 되살려 준다. 첫 실행에서 `%APPDATA%\cyella`도 이렇게 생긴다.
            const std::size_t separator { prepared.value->find_last_of(L'\\') };
            if (separator != std::wstring::npos)
            {
                std::uint32_t directory_error { ERROR_SUCCESS };
                if (create_directory_tree(prepared.value->substr(0, separator), directory_error) == false)
                    return { settings_file_commit_failure::write, directory_error };
            }

            temporary_file temporary { create_temporary_file(*prepared.value) };
            if (temporary.handle.valid() == false)
                return { settings_file_commit_failure::write, temporary.native_error };

            const settings_file_commit_result write_result { write_bytes(temporary.handle.get(), bytes) };
            if (write_result.succeeded() == false)
            {
                temporary.handle.reset();
                delete_temporary_file(temporary.path);
                return write_result;
            }

            // 교체 전에 디스크에 내려 둔다. 그래야 전원이 나가도 빈 파일이 교체본이 되지 않는다.
            SetLastError(ERROR_SUCCESS);
            if (FlushFileBuffers(temporary.handle.get()) == FALSE)
            {
                const std::uint32_t error { last_error_or(ERROR_WRITE_FAULT) };
                temporary.handle.reset();
                delete_temporary_file(temporary.path);
                return { settings_file_commit_failure::flush, error };
            }
            temporary.handle.reset();

            // 임시 파일 속성(temporary·not indexed)이 교체 후 설정 파일에 남지 않게 한다.
            // 실패해도 내용은 온전하므로 교체를 계속한다.
            static_cast<void>(SetFileAttributesW(temporary.path.c_str(), FILE_ATTRIBUTE_NORMAL));

            SetLastError(ERROR_SUCCESS);
            if (MoveFileExW(temporary.path.c_str(), prepared.value->c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE)
                return {};

            const std::uint32_t replace_error { last_error_or(ERROR_WRITE_FAULT) };
            delete_temporary_file(temporary.path);
            return { settings_file_commit_failure::replace, replace_error };
        }

        settings_file_rename_result rename_impl(const std::u8string_view from, const std::u8string_view to)
        {
            const prepared_path source { prepare_file_path(from) };
            if (source.value.has_value() == false)
                return { false, source.native_error };
            const prepared_path target { prepare_file_path(to) };
            if (target.value.has_value() == false)
                return { false, target.native_error };

            // 덮어쓰기 플래그를 주지 않는다. 도착 이름이 있으면 실패해야 이전 백업이 남는다.
            SetLastError(ERROR_SUCCESS);
            if (MoveFileExW(source.value->c_str(), target.value->c_str(), MOVEFILE_WRITE_THROUGH) != FALSE)
                return { true, std::nullopt };
            return { false, last_error_or(ERROR_WRITE_FAULT) };
        }

        class win32_settings_file_system final : public settings_file_system
        {
        public:
            [[nodiscard]] settings_file_read_result read(const std::u8string_view path) noexcept override
            {
                return read_whole_file(path);
            }

            [[nodiscard]] settings_file_commit_result atomic_commit(const std::u8string_view path, const std::u8string_view bytes) noexcept override
            {
                try
                {
                    return atomic_commit_impl(path, bytes);
                }
                catch (...)
                {
                    return { settings_file_commit_failure::write, ERROR_NOT_ENOUGH_MEMORY };
                }
            }

            [[nodiscard]] settings_file_rename_result rename(const std::u8string_view from, const std::u8string_view to) noexcept override
            {
                try
                {
                    return rename_impl(from, to);
                }
                catch (...)
                {
                    return { false, ERROR_NOT_ENOUGH_MEMORY };
                }
            }
        };
    } // namespace

    std::unique_ptr<settings_file_system> make_settings_file_system()
    {
        return std::make_unique<win32_settings_file_system>();
    }
} // namespace cyella::win32
