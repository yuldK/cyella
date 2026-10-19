#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>

#include <windows.h>

namespace cyella::test {
    // test마다 따로 쓰는 임시 폴더다. 소멸할 때 통째로 지운다. 실패해도 test를 깨지 않는다.
    class temporary_directory
    {
    public:
        explicit temporary_directory(const std::string_view label)
        {
            static std::atomic_uint64_t sequence { 0 };
            path_ = std::filesystem::temp_directory_path()
                / (std::string { "cyella-test-" } + std::string { label } + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(sequence.fetch_add(1)));
            std::error_code ignored {};
            std::filesystem::remove_all(path_, ignored);
            std::filesystem::create_directories(path_);
        }

        temporary_directory(const temporary_directory&) = delete;
        temporary_directory& operator=(const temporary_directory&) = delete;

        ~temporary_directory()
        {
            std::error_code ignored {};
            std::filesystem::remove_all(path_, ignored);
        }

        [[nodiscard]] const std::filesystem::path& path() const noexcept
        {
            return path_;
        }

        // 백슬래시 구분의 UTF-8 경로다. 앱이 쓰는 모양과 같다.
        [[nodiscard]] std::u8string u8path() const
        {
            return path_.u8string();
        }

    private:
        std::filesystem::path path_ {};
    };

    inline void write_file(const std::filesystem::path& path, const std::string_view content)
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream { path, std::ios::binary | std::ios::trunc };
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    inline std::string read_file(const std::filesystem::path& path)
    {
        std::ifstream stream { path, std::ios::binary };
        return std::string { std::istreambuf_iterator<char> { stream }, std::istreambuf_iterator<char> {} };
    }
} // namespace cyella::test
