#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cyella::macos {
    // 닫기를 잊지 않도록 파일 기술자를 감싼다. 음수는 "없음"이다.
    class unique_fd
    {
    public:
        unique_fd() noexcept = default;

        explicit unique_fd(const int value) noexcept
            : value_ { value }
        {}

        unique_fd(const unique_fd&) = delete;
        unique_fd& operator=(const unique_fd&) = delete;

        unique_fd(unique_fd&& other) noexcept
            : value_ { other.release() }
        {}

        unique_fd& operator=(unique_fd&& other) noexcept
        {
            if (this != &other)
                reset(other.release());
            return *this;
        }

        ~unique_fd()
        {
            reset();
        }

        [[nodiscard]] int get() const noexcept
        {
            return value_;
        }

        [[nodiscard]] bool valid() const noexcept
        {
            return value_ >= 0;
        }

        [[nodiscard]] int release() noexcept
        {
            const int value { value_ };
            value_ = -1;
            return value;
        }

        void reset(int value = -1) noexcept;

    private:
        int value_ { -1 };
    };

    // 환경 변수의 값이다. 없으면 값이 없다 (빈 값은 빈 문자열이다).
    [[nodiscard]] std::optional<std::u8string> environment_value(std::u8string_view name);

    // 사용자의 홈 폴더다. `HOME`이 있으면 그것이고, 없으면 사용자 DB(`getpwuid_r`)다. 모르면 비어 있다.
    [[nodiscard]] std::u8string home_directory();

    // `~/Library/<relative>`다. 홈을 모르면 비어 있다. 폴더를 만들지 않는다.
    [[nodiscard]] std::u8string library_directory(std::u8string_view relative);

    // 실행 중인 프로세스의 실행 파일 절대 경로다 (심볼릭 링크를 푼다). 모르면 비어 있다.
    [[nodiscard]] std::u8string current_executable_path();

    // 실행 파일이 있는 폴더다. 앱 번들이면 `Cyella.app/Contents/MacOS`다. 모르면 비어 있다.
    [[nodiscard]] std::u8string current_executable_directory();

    // 실행 파일이 앱 번들(`…/Contents/MacOS`) 안에 있으면 그 번들의 `Contents/Resources`다. 아니면 실행 파일 폴더다
    // (test 실행 파일, 맨 실행 파일). 모르면 비어 있다.
    [[nodiscard]] std::u8string resources_directory();

    // 폴더가 없으면 상위부터 만든다 (권한 0700 — 사용자 자료 폴더다). 이미 있으면 성공이다. 실패하면 errno를 남긴다.
    [[nodiscard]] bool create_directory_tree(std::u8string_view path, int& error) noexcept;

    struct whole_file_read
    {
        // 읽었으면 내용이다.
        std::optional<std::string> bytes {};
        // 파일이 없다 (ENOENT·ENOTDIR).
        bool missing { false };
        bool too_large { false };
        int error { 0 };
    };
    // 파일 전체를 읽는다. `maximum_bytes`보다 크면 읽지 않는다. 예외를 던지지 않는다.
    [[nodiscard]] whole_file_read read_whole_file(std::u8string_view path, std::size_t maximum_bytes) noexcept;

    // 쓰는 도중 신호로 끊겨도(EINTR) 끝까지 쓴다. 다 쓰지 못하면 false이고 errno가 남는다.
    [[nodiscard]] bool write_all(int fd, const void* data, std::size_t size) noexcept;

    [[nodiscard]] inline std::string to_narrow(const std::u8string_view text)
    {
        return std::string { reinterpret_cast<const char*>(text.data()), text.size() };
    }

    [[nodiscard]] inline std::u8string to_u8(const std::string_view text)
    {
        return std::u8string { reinterpret_cast<const char8_t*>(text.data()), text.size() };
    }
} // namespace cyella::macos
