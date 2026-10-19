#include "infrastructure/diff_temp_directory.h"

#include <cstdio>
#include <random>
#include <string>
#include <string_view>
#include <system_error>

namespace cyella {
    namespace {
        constexpr std::string_view process_directory_prefix { "p" };

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
        return directory;
    }

    void diff_temp_directory::remove_all() noexcept
    {
        const std::lock_guard lock { mutex_ };
        std::error_code ignored {};
        std::filesystem::remove_all(process_directory_, ignored);
        // 다른 프로세스의 폴더가 없으면 root도 치운다. 비어 있을 때만 지워지므로 안전하다.
        std::filesystem::remove(root_, ignored);
    }

    void diff_temp_directory::sweep_stale(const std::filesystem::path& root, const std::chrono::hours max_age) noexcept
    {
        std::error_code error {};
        if (std::filesystem::is_directory(root, error) == false)
            return;
        const auto now { std::filesystem::file_time_type::clock::now() };
        for (const auto& entry : std::filesystem::directory_iterator { root, error })
        {
            std::error_code entry_error {};
            if (entry.is_directory(entry_error) == false)
                continue;
            const std::string name { entry.path().filename().string() };
            if (name.starts_with(process_directory_prefix) == false)
                continue;
            const auto modified { std::filesystem::last_write_time(entry.path(), entry_error) };
            if (entry_error || now - modified < max_age)
                continue;
            std::filesystem::remove_all(entry.path(), entry_error);
        }
    }
} // namespace cyella
