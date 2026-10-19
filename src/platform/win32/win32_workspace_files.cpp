#include "platform/win32/win32_workspace_files.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace cyella::win32 {
    namespace {
        // std::filesystem은 char8_t 경로를 UTF-8로 받아 넓은 문자 API로 바꾼다. 한글 경로가
        // 활성 code page를 거치지 않는다.
        std::filesystem::path to_path(const std::u8string_view path)
        {
            return std::filesystem::path { path };
        }

        class filesystem_workspace_files final : public workspace_files
        {
        public:
            [[nodiscard]] path_kind probe(const std::u8string_view absolute_path) const noexcept override
            {
                try
                {
                    std::error_code error {};
                    const std::filesystem::file_status status { std::filesystem::status(to_path(absolute_path), error) };
                    if (error)
                        return path_kind::missing;
                    if (std::filesystem::is_directory(status))
                        return path_kind::directory;
                    if (std::filesystem::exists(status))
                        return path_kind::file;
                    return path_kind::missing;
                }
                catch (...)
                {
                    return path_kind::missing;
                }
            }

            [[nodiscard]] std::optional<std::string> read_text(const std::u8string_view absolute_path, const std::size_t maximum_bytes) const noexcept override
            {
                try
                {
                    std::error_code error {};
                    const std::filesystem::path path { to_path(absolute_path) };
                    const std::uintmax_t size { std::filesystem::file_size(path, error) };
                    if (error || size > maximum_bytes)
                        return std::nullopt;
                    std::ifstream stream { path, std::ios::binary };
                    if (stream.is_open() == false)
                        return std::nullopt;
                    return std::string { std::istreambuf_iterator<char> { stream }, std::istreambuf_iterator<char> {} };
                }
                catch (...)
                {
                    return std::nullopt;
                }
            }

            [[nodiscard]] bool append_text(const std::u8string_view absolute_path, const std::string_view text) noexcept override
            {
                try
                {
                    std::ofstream stream { to_path(absolute_path), std::ios::binary | std::ios::app };
                    if (stream.is_open() == false)
                        return false;
                    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
                    return static_cast<bool>(stream);
                }
                catch (...)
                {
                    return false;
                }
            }

            [[nodiscard]] bool remove_file(const std::u8string_view absolute_path) noexcept override
            {
                try
                {
                    std::error_code error {};
                    return std::filesystem::remove(to_path(absolute_path), error) && error == std::error_code {};
                }
                catch (...)
                {
                    return false;
                }
            }
        };
    } // namespace

    std::unique_ptr<workspace_files> make_workspace_files()
    {
        return std::make_unique<filesystem_workspace_files>();
    }
} // namespace cyella::win32
