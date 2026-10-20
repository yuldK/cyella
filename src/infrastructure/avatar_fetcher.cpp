#include "infrastructure/avatar_fetcher.h"

#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

namespace cyella {
    namespace {
        // 보관한 파일이 기한 안인가다. 파일이 없거나 시각을 읽지 못하면 거짓이다.
        bool fresh(const std::filesystem::path& path, const std::chrono::hours keep)
        {
            std::error_code error {};
            const std::filesystem::file_time_type written { std::filesystem::last_write_time(path, error) };
            if (error)
                return false;
            return std::filesystem::file_time_type::clock::now() - written < keep;
        }

        std::optional<std::string> read_bytes(const std::filesystem::path& path)
        {
            std::ifstream stream { path, std::ios::binary };
            if (stream.is_open() == false)
                return std::nullopt;
            std::string bytes { std::istreambuf_iterator<char> { stream }, std::istreambuf_iterator<char> {} };
            if (bytes.empty())
                return std::nullopt;
            return bytes;
        }

        // 보관은 덤이다. 쓰지 못해도 받은 사진은 그대로 쓴다.
        void write_bytes(const std::filesystem::path& path, const std::string_view bytes)
        {
            std::error_code error {};
            std::filesystem::create_directories(path.parent_path(), error);
            const std::filesystem::path temporary { path.string() + ".part" };
            {
                std::ofstream stream { temporary, std::ios::binary | std::ios::trunc };
                if (stream.is_open() == false)
                    return;
                stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                if (stream.good() == false)
                    return;
            }
            std::filesystem::rename(temporary, path, error);
            if (error)
                std::filesystem::remove(temporary, error);
        }

        void remove_quietly(const std::filesystem::path& path)
        {
            std::error_code error {};
            std::filesystem::remove(path, error);
        }
    } // namespace

    std::string normalize_avatar_email(const std::string_view email)
    {
        std::size_t begin { 0 };
        std::size_t end { email.size() };
        while (begin < end && (email[begin] == ' ' || email[begin] == '\t'))
            ++begin;
        while (end > begin && (email[end - 1] == ' ' || email[end - 1] == '\t'))
            --end;
        std::string normalized { email.substr(begin, end - begin) };
        for (char& value : normalized)
            if (value >= 'A' && value <= 'Z')
                value = static_cast<char>(value - 'A' + 'a');
        return normalized;
    }

    std::string gravatar_url(const std::string_view hash_hex, const int pixel_size)
    {
        return "https://gravatar.com/avatar/" + std::string { hash_hex } + "?s=" + std::to_string(pixel_size) + "&d=404";
    }

    avatar_fetcher::avatar_fetcher(hosting_transport& transport, avatar_fetch_options options)
        : transport_ { &transport }
        , options_ { std::move(options) }
    {}

    avatar_fetch_result avatar_fetcher::fetch(const std::string_view email, const process_cancellation_token& cancellation) const
    {
        avatar_fetch_result result {};
        const std::string normalized { normalize_avatar_email(email) };
        if (normalized.empty() || normalized.find('@') == std::string::npos || options_.sha256_hex == nullptr)
        {
            result.missing = true;
            return result;
        }
        const std::string hash { options_.sha256_hex(normalized) };
        if (hash.empty())
        {
            result.failed = true;
            return result;
        }
        // 보관 폴더를 모르면 보관 없이 받기만 한다 (빈 경로에 쓰면 작업 폴더에 파일이 생긴다).
        const bool cached { options_.cache_directory.empty() == false };
        const std::filesystem::path image { cached ? options_.cache_directory / (hash + ".img") : std::filesystem::path {} };
        const std::filesystem::path none { cached ? options_.cache_directory / (hash + ".none") : std::filesystem::path {} };
        if (fresh(image, options_.keep_image))
            if (std::optional<std::string> bytes { read_bytes(image) }; bytes.has_value())
            {
                result.bytes = std::move(*bytes);
                return result;
            }
        if (fresh(none, options_.keep_missing))
        {
            result.missing = true;
            return result;
        }

        hosting_http_request request {};
        request.url = gravatar_url(hash, options_.pixel_size);
        const hosting_http_response response { transport_->execute(request, secret_text {}, cancellation) };
        if (response.status_code == 200 && response.body.empty() == false)
        {
            if (cached)
                write_bytes(image, response.body);
            remove_quietly(none);
            result.bytes = response.body;
            return result;
        }
        if (response.status_code == 404)
        {
            if (cached)
                write_bytes(none, "404");
            remove_quietly(image);
            result.missing = true;
            return result;
        }
        // 받지 못했다. 기한이 지난 사진이라도 있으면 쓴다.
        if (std::optional<std::string> stale { read_bytes(image) }; stale.has_value())
        {
            result.bytes = std::move(*stale);
            return result;
        }
        result.failed = true;
        return result;
    }
} // namespace cyella
