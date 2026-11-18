#include "infrastructure/diff_temp_directory.h"

#include <cstdio>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace cyella {
    namespace {
        constexpr std::string_view process_directory_prefix { "p" };
        constexpr std::string_view lock_file_name { "lock" };

        // 프로세스마다 겹치지 않는 이름이다. 난수라서 프로세스 번호가 재사용돼도 부딪히지 않는다.
        std::string unique_process_name()
        {
            std::random_device device {};
            const std::uint64_t value { (static_cast<std::uint64_t>(device()) << 32) | device() };
            char text[17] {};
            std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
            return std::string { process_directory_prefix } + text;
        }
    } // namespace

    diff_temp_directory::diff_temp_directory(std::filesystem::path root)
        : root_ { std::move(root) }
    {
        std::error_code error {};
        if (root_.empty())
            root_ = std::filesystem::temp_directory_path(error) / "cyella-diff";
        process_directory_ = root_ / unique_process_name();
        sweep_stale(root_, std::chrono::hours { 24 });
    }

    diff_temp_directory::~diff_temp_directory()
    {
        remove_all();
    }

    std::filesystem::path diff_temp_directory::create_operation_directory()
    {
        const std::lock_guard lock { mutex_ };
        const std::filesystem::path directory { process_directory_ / std::to_string(++counter_) };
        std::error_code error {};
        std::filesystem::create_directories(directory, error);
        if (error)
            return {};
        // MSVC의 파일 스트림은 지우기 공유 없이 열므로, 이 파일이 열려 있는 동안 다른 인스턴스의 정리가 이 폴더를
        // 알아본다. 열지 못해도 작업은 계속한다 (그때는 예전처럼 나이로만 가려진다).
        if (lock_.is_open() == false)
            lock_.open(process_directory_ / lock_file_name, std::ios::binary | std::ios::trunc);
        return directory;
    }

    void diff_temp_directory::remove_all() noexcept
    {
        const std::lock_guard lock { mutex_ };
        // 열어 둔 잠금 파일은 자기 자신도 지우지 못하므로 먼저 닫는다.
        lock_.close();
        std::error_code ignored {};
        std::filesystem::remove_all(process_directory_, ignored);
        // 다른 프로세스의 폴더가 없으면 root도 치운다. 비어 있을 때만 지워지므로 안전하다.
        std::filesystem::remove(root_, ignored);
    }

    void diff_temp_directory::remove_operation_directory(const std::filesystem::path& directory) noexcept
    {
        // 잘못 건넨 경로로 프로세스 폴더 자체나 그 밖, 다른 인스턴스가 알아보는 잠금 파일을 지우지 않는다.
        const std::filesystem::path normal { directory.lexically_normal() };
        if (normal.empty() || normal.parent_path() != process_directory_.lexically_normal() || normal.filename() == std::filesystem::path { lock_file_name })
            return;
        std::error_code ignored {};
        std::filesystem::remove_all(normal, ignored);
    }

    void diff_temp_directory::sweep_stale(const std::filesystem::path& root, const std::chrono::hours max_age) noexcept
    {
        std::error_code error {};
        if (std::filesystem::is_directory(root, error) == false)
            return;
        const auto now { std::filesystem::file_time_type::clock::now() };
        const std::filesystem::path prefix { process_directory_prefix };
        // noexcept 안이므로 던지는 길을 쓰지 않는다. 범위 for의 증가와 `string()`(ANSI 코드 페이지로 못 바꾸는 이름)은
        // 예외를 던져 앱을 끝낸다.
        const std::filesystem::directory_iterator end {};
        for (std::filesystem::directory_iterator it { root, error }; error == std::error_code {} && it != end; it.increment(error))
        {
            const std::filesystem::directory_entry& entry { *it };
            std::error_code entry_error {};
            if (entry.is_directory(entry_error) == false)
                continue;
            const std::filesystem::path name { entry.path().filename() };
            if (name.native().starts_with(prefix.native()) == false)
                continue;
            const auto modified { std::filesystem::last_write_time(entry.path(), entry_error) };
            if (entry_error || now - modified < max_age)
                continue;
            // 살아 있는 인스턴스가 잠금 파일을 열고 있으면 지우지 못한다. 그 폴더는 오래돼도 남긴다.
            const std::filesystem::path lock_file { entry.path() / lock_file_name };
            if (std::filesystem::remove(lock_file, entry_error) == false && entry_error)
                continue;
            std::filesystem::remove_all(entry.path(), entry_error);
        }
    }
} // namespace cyella
