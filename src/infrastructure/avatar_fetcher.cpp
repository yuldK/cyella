#include "infrastructure/avatar_fetcher.h"

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

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
            // `path.string()`은 코드 페이지 밖 글자(사용자 이름의 한글 등)에서 예외를 던진다. 넓은 경로 그대로 잇는다.
            std::filesystem::path temporary { path };
            temporary += ".part";
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

        constexpr std::size_t avatar_body_limit { std::size_t { 1 } * 1024 * 1024 };

        bool image_or_unspecified(std::string_view content_type)
        {
            while (content_type.empty() == false && (content_type.front() == ' ' || content_type.front() == '\t'))
                content_type.remove_prefix(1);
            if (content_type.empty())
                return true;
            constexpr std::string_view image { "image/" };
            if (content_type.size() < image.size())
                return false;
            return std::ranges::equal(content_type.substr(0, image.size()), image, [](const char left, const char right) { return (left >= 'A' && left <= 'Z' ? static_cast<char>(left - 'A' + 'a') : left) == right; });
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
        // 80픽셀 사진은 수 KB다. 엉뚱한 큰 답을 받아 보관하지 않는다.
        request.max_body_bytes = avatar_body_limit;
        const hosting_http_response response { transport_->execute(request, secret_text {}, cancellation) };
        // 형식을 밝힌 답이 그림이 아니면(프록시의 HTML 안내 등) 사진으로 보관하지 않고 받지 못한 것으로 본다. 형식을 밝히지 않은 답은 예전처럼 받는다.
        if (response.status_code == 200 && response.body.empty() == false && image_or_unspecified(response.header("content-type")))
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

    void avatar_fetcher::prune() const
    {
        if (options_.cache_directory.empty())
            return;
        std::error_code error {};
        std::filesystem::directory_iterator entries { options_.cache_directory, error };
        // 목록을 도는 동안 지우면 순회가 흔들리므로 모아 두었다가 지운다. 넘기기는 오류 코드 꼴로 해 던지지 않는다.
        std::vector<std::filesystem::path> expired {};
        for (; error == std::error_code {} && entries != std::filesystem::directory_iterator {}; entries.increment(error))
        {
            std::error_code kind_error {};
            if (entries->is_regular_file(kind_error) == false)
                continue;
            const std::filesystem::path& path { entries->path() };
            const std::filesystem::path extension { path.extension() };
            if (extension == ".part")
                expired.push_back(path);
            else if (extension == ".img" && fresh(path, options_.keep_image) == false)
                expired.push_back(path);
            else if (extension == ".none" && fresh(path, options_.keep_missing) == false)
                expired.push_back(path);
        }
        for (const std::filesystem::path& path : expired)
            remove_quietly(path);
    }
} // namespace cyella
