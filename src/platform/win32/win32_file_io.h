#pragma once

#include "application/settings_file_system.h"

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cyella::win32 {
    // 닫기를 잊지 않도록 핸들을 감싼다. `INVALID_HANDLE_VALUE`와 null을 모두 "없음"으로 본다.
    class unique_handle
    {
    public:
        unique_handle() noexcept = default;

        explicit unique_handle(HANDLE value) noexcept
            : value_ { value }
        {}

        unique_handle(const unique_handle&) = delete;
        unique_handle& operator=(const unique_handle&) = delete;

        unique_handle(unique_handle&& other) noexcept
            : value_ { other.release() }
        {}

        unique_handle& operator=(unique_handle&& other) noexcept
        {
            if (this != &other)
            {
                reset();
                value_ = other.release();
            }
            return *this;
        }

        ~unique_handle()
        {
            reset();
        }

        [[nodiscard]] HANDLE get() const noexcept
        {
            return value_;
        }

        [[nodiscard]] bool valid() const noexcept
        {
            return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
        }

        [[nodiscard]] HANDLE release() noexcept
        {
            const HANDLE value { value_ };
            value_ = INVALID_HANDLE_VALUE;
            return value;
        }

        void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept
        {
            if (valid())
                CloseHandle(value_);
            value_ = value;
        }

    private:
        HANDLE value_ { INVALID_HANDLE_VALUE };
    };

    struct prepared_path
    {
        std::optional<std::wstring> value {};
        std::uint32_t native_error { ERROR_INVALID_NAME };
    };

    // UTF-8 경로를 파일 API에 넘길 수 있는 절대 경로로 만든다. `..`를 풀고 `\\?\` 접두어를
    // 붙여 MAX_PATH를 넘는 경로에서도 실패하지 않게 한다. device 경로(`\\.\`)는 거부한다.
    [[nodiscard]] prepared_path prepare_file_path(std::u8string_view path);

    // 폴더가 없으면 상위부터 만든다. 이미 있으면 성공이다. `path`는 `prepare_file_path` 결과다.
    [[nodiscard]] bool create_directory_tree(const std::wstring& path, std::uint32_t& native_error) noexcept;

    // 파일 전체를 읽는다. 없으면 `not_found`다. 예외를 던지지 않는다.
    [[nodiscard]] settings_file_read_result read_whole_file(std::u8string_view path) noexcept;
} // namespace cyella::win32
