#include "platform/win32/win32_settings_file_system.h"

#include "platform/macos/posix_file.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <limits>
#include <stdio.h>
#include <unistd.h>

#include <atomic>
#include <string>

// `platform/win32/win32_settings_file_system.h`의 macOS 구현이다.
//  - 쓰기는 같은 폴더의 임시 파일에 쓰고 `fsync`한 뒤 `rename`으로 바꾼다. 같은 볼륨 안의 rename은 원자적이다.
//  - 백업 이름으로 옮기기(`rename`)는 덮어쓰지 않는다 (`renamex_np`의 `RENAME_EXCL`). 이전 백업을 지우지 않으려는 것이다.
namespace cyella::win32 {
    namespace {
        std::atomic_uint64_t temporary_file_sequence { 0 };

        std::uint32_t error_code(const int value) noexcept
        {
            return static_cast<std::uint32_t>(value == 0 ? EIO : value);
        }

        std::string parent_of(const std::string& path)
        {
            const std::size_t slash { path.find_last_of('/') };
            if (slash == std::string::npos)
                return {};
            if (slash == 0)
                return "/";
            return path.substr(0, slash);
        }

        settings_file_commit_result commit(const std::u8string_view target_path, const std::u8string_view bytes)
        {
            const std::string target { macos::to_narrow(target_path) };
            if (target.empty() || target.front() != '/' || target.find('\0') != std::string::npos)
                return { settings_file_commit_failure::write, static_cast<std::uint32_t>(EINVAL) };
            const std::string directory { parent_of(target) };
            int create_error { 0 };
            if (directory.empty() || macos::create_directory_tree(macos::to_u8(directory), create_error) == false)
                return { settings_file_commit_failure::write, error_code(create_error) };

            // 이름이 겹치지 않게 프로세스 번호와 순번을 붙인다. `O_EXCL`이 남이 먼저 만든 파일을 덮지 않는다.
            std::string temporary {};
            macos::unique_fd file {};
            for (int attempt { 0 }; attempt < 8 && file.valid() == false; ++attempt)
            {
                temporary = directory + (directory == "/" ? "" : "/") + ".cyella-" + std::to_string(::getpid()) + "-"
                    + std::to_string(temporary_file_sequence.fetch_add(1, std::memory_order_relaxed)) + ".tmp";
                file.reset(::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
                if (file.valid() == false && errno != EEXIST)
                    return { settings_file_commit_failure::write, error_code(errno) };
            }
            if (file.valid() == false)
                return { settings_file_commit_failure::write, static_cast<std::uint32_t>(EEXIST) };

            if (macos::write_all(file.get(), bytes.data(), bytes.size()) == false)
            {
                const int error { errno };
                file.reset();
                static_cast<void>(::unlink(temporary.c_str()));
                return { settings_file_commit_failure::write, error_code(error) };
            }
            // `fsync`는 장치의 캐시까지 내리지 않는다. 전원이 끊겨도 옛 파일이나 새 파일이 남도록 `F_FULLFSYNC`를 먼저 쓴다.
            if (::fcntl(file.get(), F_FULLFSYNC) != 0 && ::fsync(file.get()) != 0)
            {
                const int error { errno };
                file.reset();
                static_cast<void>(::unlink(temporary.c_str()));
                return { settings_file_commit_failure::flush, error_code(error) };
            }
            file.reset();
            if (::rename(temporary.c_str(), target.c_str()) != 0)
            {
                const int error { errno };
                static_cast<void>(::unlink(temporary.c_str()));
                return { settings_file_commit_failure::replace, error_code(error) };
            }
            return {};
        }

        class posix_settings_file_system final : public settings_file_system
        {
        public:
            [[nodiscard]] settings_file_read_result read(const std::u8string_view path) noexcept override
            {
                try
                {
                    const macos::whole_file_read read { macos::read_whole_file(path, std::numeric_limits<std::size_t>::max()) };
                    settings_file_read_result result {};
                    if (read.bytes.has_value())
                    {
                        result.state = settings_file_read_state::available;
                        result.bytes = macos::to_u8(*read.bytes);
                        return result;
                    }
                    result.state = read.missing ? settings_file_read_state::not_found : settings_file_read_state::failed;
                    if (read.missing == false)
                        result.native_error = error_code(read.error);
                    return result;
                }
                catch (...)
                {
                    return { settings_file_read_state::failed, {}, static_cast<std::uint32_t>(ENOMEM) };
                }
            }

            [[nodiscard]] settings_file_commit_result atomic_commit(const std::u8string_view path, const std::u8string_view bytes) noexcept override
            {
                try
                {
                    return commit(path, bytes);
                }
                catch (...)
                {
                    return { settings_file_commit_failure::write, static_cast<std::uint32_t>(ENOMEM) };
                }
            }

            [[nodiscard]] settings_file_rename_result rename(const std::u8string_view from, const std::u8string_view to) noexcept override
            {
                try
                {
                    const std::string source { macos::to_narrow(from) };
                    const std::string target { macos::to_narrow(to) };
                    if (::renamex_np(source.c_str(), target.c_str(), RENAME_EXCL) == 0)
                        return { true, std::nullopt };
                    return { false, error_code(errno) };
                }
                catch (...)
                {
                    return { false, static_cast<std::uint32_t>(ENOMEM) };
                }
            }
        };
    } // namespace

    std::unique_ptr<settings_file_system> make_settings_file_system()
    {
        return std::make_unique<posix_settings_file_system>();
    }
} // namespace cyella::win32
