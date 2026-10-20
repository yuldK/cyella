#include "domain/lfs.h"

#include <array>
#include <charconv>
#include <cstddef>

namespace cyella {
    namespace {
        // 포인터 파일은 1 KiB보다 작다(스펙). 이보다 큰 텍스트는 포인터가 아니다.
        constexpr std::size_t maximum_pointer_bytes { 1024 };
        constexpr std::string_view version_prefix { "version https://git-lfs.github.com/spec/" };
        constexpr std::string_view oid_prefix { "oid sha256:" };
        constexpr std::string_view size_prefix { "size " };
        constexpr std::size_t sha256_hex_length { 64 };

        bool is_hex(const std::string_view text) noexcept
        {
            for (const char value : text)
                if ((value >= '0' && value <= '9') == false && (value >= 'a' && value <= 'f') == false)
                    return false;
            return text.empty() == false;
        }

        std::string_view next_line(std::string_view& rest) noexcept
        {
            const std::size_t end { rest.find('\n') };
            std::string_view line { rest.substr(0, end) };
            rest = end == std::string_view::npos ? std::string_view {} : rest.substr(end + 1);
            if (line.empty() == false && line.back() == '\r')
                line.remove_suffix(1);
            return line;
        }
    } // namespace

    std::optional<lfs_pointer> parse_lfs_pointer(const std::string_view text)
    {
        if (text.size() > maximum_pointer_bytes || text.starts_with(version_prefix) == false)
            return std::nullopt;

        lfs_pointer pointer {};
        bool has_oid { false };
        bool has_size { false };
        std::string_view rest { text };
        while (rest.empty() == false)
        {
            const std::string_view line { next_line(rest) };
            if (line.starts_with(oid_prefix))
            {
                const std::string_view hash { line.substr(oid_prefix.size()) };
                if (hash.size() != sha256_hex_length || is_hex(hash) == false)
                    return std::nullopt;
                pointer.oid = std::string { hash };
                has_oid = true;
            }
            else if (line.starts_with(size_prefix))
            {
                const std::string_view digits { line.substr(size_prefix.size()) };
                std::uint64_t value { 0 };
                const auto [end, error] { std::from_chars(digits.data(), digits.data() + digits.size(), value) };
                if (error != std::errc {} || end != digits.data() + digits.size())
                    return std::nullopt;
                pointer.size = value;
                has_size = true;
            }
        }
        if (has_oid == false || has_size == false)
            return std::nullopt;
        return pointer;
    }

    std::string format_byte_size(const std::uint64_t bytes)
    {
        constexpr std::array<const char*, 5> units { "B", "KB", "MB", "GB", "TB" };
        if (bytes < 1000)
            return std::to_string(bytes) + " B";
        double value { static_cast<double>(bytes) };
        std::size_t unit { 0 };
        while (value >= 1000.0 && unit + 1 < units.size())
        {
            value /= 1000.0;
            ++unit;
        }
        // 소수 한 자리로 반올림한다. 1000.0이 되면 한 단위 올린다.
        std::string number { std::to_string(static_cast<long long>(value * 10.0 + 0.5)) };
        if (number.size() < 2)
            number.insert(number.begin(), '0');
        std::string text { number.substr(0, number.size() - 1) + "." + number.back() };
        if (text == "1000.0" && unit + 1 < units.size())
        {
            text = "1.0";
            ++unit;
        }
        return text + " " + units[unit];
    }
} // namespace cyella
