#include "platform/macos/posix_file.h"

#include <mach-o/dyld.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

namespace cyella::macos {
    void unique_fd::reset(const int value) noexcept
    {
        if (value_ >= 0)
            static_cast<void>(::close(value_));
        value_ = value;
    }

    std::optional<std::u8string> environment_value(const std::u8string_view name)
    {
        const std::string key { to_narrow(name) };
        const char* const value { std::getenv(key.c_str()) };
        if (value == nullptr)
            return std::nullopt;
        return to_u8(value);
    }

    std::u8string home_directory()
    {
        if (const std::optional<std::u8string> home { environment_value(u8"HOME") }; home.has_value() && home->empty() == false)
            return *home;
        long size { ::sysconf(_SC_GETPW_R_SIZE_MAX) };
        if (size <= 0)
            size = 16384;
        std::vector<char> buffer(static_cast<std::size_t>(size));
        passwd entry {};
        passwd* found { nullptr };
        if (::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &found) != 0 || found == nullptr || found->pw_dir == nullptr)
            return {};
        return to_u8(found->pw_dir);
    }

    std::u8string library_directory(const std::u8string_view relative)
    {
        std::u8string home { home_directory() };
        if (home.empty())
            return {};
        while (home.size() > 1 && home.back() == u8'/')
            home.pop_back();
        std::u8string path { home + u8"/Library" };
        if (relative.empty() == false)
        {
            path.push_back(u8'/');
            path.append(relative);
        }
        return path;
    }

    std::u8string current_executable_path()
    {
        std::uint32_t size { 0 };
        static_cast<void>(::_NSGetExecutablePath(nullptr, &size));
        std::vector<char> raw(static_cast<std::size_t>(size) + 1u, '\0');
        if (::_NSGetExecutablePath(raw.data(), &size) != 0)
            return {};
        char resolved[PATH_MAX] {};
        if (::realpath(raw.data(), resolved) == nullptr)
            return to_u8(raw.data());
        return to_u8(resolved);
    }

    std::u8string current_executable_directory()
    {
        const std::u8string executable { current_executable_path() };
        const std::size_t slash { executable.find_last_of(u8'/') };
        if (slash == std::u8string::npos)
            return {};
        if (slash == 0)
            return u8"/";
        return executable.substr(0, slash);
    }

    std::u8string resources_directory()
    {
        const std::u8string directory { current_executable_directory() };
        constexpr std::u8string_view bundle_suffix { u8"/Contents/MacOS" };
        if (directory.ends_with(bundle_suffix))
            return directory.substr(0, directory.size() - bundle_suffix.size()) + u8"/Contents/Resources";
        return directory;
    }

    bool create_directory_tree(const std::u8string_view path, int& error) noexcept
    {
        try
        {
            error = 0;
            if (path.empty())
            {
                error = EINVAL;
                return false;
            }
            std::string text { to_narrow(path) };
            while (text.size() > 1 && text.back() == '/')
                text.pop_back();
            // 앞에서부터 한 단계씩 만든다. 이미 있는 단계(EEXIST)는 폴더인지 확인한다.
            for (std::size_t position { 1 }; position <= text.size(); ++position)
            {
                if (position != text.size() && text[position] != '/')
                    continue;
                const std::string prefix { text.substr(0, position) };
                if (::mkdir(prefix.c_str(), 0700) == 0)
                    continue;
                if (errno != EEXIST)
                {
                    error = errno;
                    return false;
                }
                struct stat info {};
                if (::stat(prefix.c_str(), &info) != 0 || S_ISDIR(info.st_mode) == false)
                {
                    error = ENOTDIR;
                    return false;
                }
            }
            return true;
        }
        catch (...)
        {
            error = ENOMEM;
            return false;
        }
    }

    whole_file_read read_whole_file(const std::u8string_view path, const std::size_t maximum_bytes) noexcept
    {
        whole_file_read result {};
        try
        {
            const std::string text { to_narrow(path) };
            const unique_fd file { ::open(text.c_str(), O_RDONLY | O_CLOEXEC) };
            if (file.valid() == false)
            {
                result.error = errno;
                result.missing = errno == ENOENT || errno == ENOTDIR;
                return result;
            }
            struct stat info {};
            if (::fstat(file.get(), &info) != 0)
            {
                result.error = errno;
                return result;
            }
            if (S_ISREG(info.st_mode) == false)
            {
                result.error = EISDIR;
                return result;
            }
            if (static_cast<std::uint64_t>(info.st_size) > maximum_bytes)
            {
                result.too_large = true;
                return result;
            }
            std::string bytes {};
            bytes.reserve(static_cast<std::size_t>(info.st_size));
            char buffer[64 * 1024];
            while (true)
            {
                const ssize_t read_count { ::read(file.get(), buffer, sizeof(buffer)) };
                if (read_count < 0)
                {
                    if (errno == EINTR)
                        continue;
                    result.error = errno;
                    return result;
                }
                if (read_count == 0)
                    break;
                // 읽는 사이에 파일이 자랐다. 상한을 넘으면 읽지 않은 것으로 본다.
                if (bytes.size() + static_cast<std::size_t>(read_count) > maximum_bytes)
                {
                    result.too_large = true;
                    return result;
                }
                bytes.append(buffer, static_cast<std::size_t>(read_count));
            }
            result.bytes = std::move(bytes);
            return result;
        }
        catch (...)
        {
            result.bytes.reset();
            result.error = ENOMEM;
            return result;
        }
    }

    bool write_all(const int fd, const void* const data, const std::size_t size) noexcept
    {
        const char* cursor { static_cast<const char*>(data) };
        std::size_t remaining { size };
        while (remaining != 0)
        {
            const ssize_t written { ::write(fd, cursor, remaining) };
            if (written < 0)
            {
                if (errno == EINTR)
                    continue;
                return false;
            }
            cursor += written;
            remaining -= static_cast<std::size_t>(written);
        }
        return true;
    }
} // namespace cyella::macos
