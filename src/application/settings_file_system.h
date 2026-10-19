#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cyella {
    enum class settings_file_read_state
    {
        available,
        not_found,
        failed,
    };

    struct settings_file_read_result
    {
        settings_file_read_state state { settings_file_read_state::failed };
        std::u8string bytes {};
        std::optional<std::uint32_t> native_error {};
    };

    enum class settings_file_commit_failure
    {
        none,
        write,
        flush,
        replace,
    };

    struct settings_file_commit_result
    {
        settings_file_commit_failure failure { settings_file_commit_failure::none };
        std::optional<std::uint32_t> native_error {};

        [[nodiscard]] bool succeeded() const noexcept
        {
            return failure == settings_file_commit_failure::none;
        }
    };

    struct settings_file_rename_result
    {
        bool succeeded { false };
        std::optional<std::uint32_t> native_error {};
    };

    // 설정 파일 하나를 읽고 쓰는 경계다. 계층 규칙을 지키려고 filesystem 접근을 계약으로
    // 분리하고 구현은 Win32 adapter에 둔다. 덕분에 저장·복구 규칙을 실제 디스크 없이
    // test할 수 있다. 모든 함수는 예외를 던지지 않는다.
    class settings_file_system
    {
    public:
        settings_file_system() = default;
        settings_file_system(const settings_file_system&) = delete;
        settings_file_system(settings_file_system&&) = delete;
        settings_file_system& operator=(const settings_file_system&) = delete;
        settings_file_system& operator=(settings_file_system&&) = delete;
        virtual ~settings_file_system() = default;

        [[nodiscard]] virtual settings_file_read_result read(std::u8string_view path) noexcept = 0;
        // 같은 폴더의 임시 파일에 쓰고 원자적으로 교체한다. 중간에 죽어도 옛 파일이나 새
        // 파일 중 하나가 온전히 남는다. 폴더가 없으면 만든다. 기존 파일이 없어도 성공한다.
        [[nodiscard]] virtual settings_file_commit_result atomic_commit(std::u8string_view path, std::u8string_view bytes) noexcept = 0;
        // 파일 이름을 바꾼다. 도착 이름이 이미 있으면 덮어쓰지 않고 실패한다. 깨진 설정을 백업
        // 이름으로 옮길 때 이전 백업을 지우지 않기 위해서다.
        [[nodiscard]] virtual settings_file_rename_result rename(std::u8string_view from, std::u8string_view to) noexcept = 0;
    };
} // namespace cyella
