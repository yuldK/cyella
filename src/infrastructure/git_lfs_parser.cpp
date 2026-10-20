#include "infrastructure/git_lfs_parser.h"

#include "infrastructure/git_text.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace cyella {
    namespace {
        using json = nlohmann::json;

        constexpr std::size_t sha256_hex_length { 64 };

        bool is_oid(const std::string_view text) noexcept
        {
            return text.size() == sha256_hex_length && std::ranges::all_of(text, [](const char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
        }

        std::string_view trimmed(std::string_view text) noexcept
        {
            constexpr std::string_view whitespace { " \t\r\n" };
            const std::size_t begin { text.find_first_not_of(whitespace) };
            if (begin == std::string_view::npos)
                return {};
            text.remove_prefix(begin);
            text.remove_suffix(text.size() - 1 - text.find_last_not_of(whitespace));
            return text;
        }

        // 공백으로 나뉜 첫 토큰이 `filter=lfs`인 속성이 있는가.
        bool line_sets_lfs_filter(const std::string_view line) noexcept
        {
            std::string_view rest { line };
            std::string_view token {};
            // 첫 토큰은 경로 패턴이라 건너뛴다. 패턴에 공백이 들어가려면 따옴표를 써야 하므로 이 판정으로 충분하다.
            git_text::take_field(rest, ' ', token);
            while (rest.empty() == false)
            {
                git_text::take_field(rest, ' ', token);
                if (token == "filter=lfs")
                    return true;
            }
            return false;
        }
    } // namespace

    std::optional<std::string> parse_git_lfs_version(const std::string_view bytes)
    {
        const std::string_view text { trimmed(bytes) };
        if (git_text::starts_with(text, "git-lfs/") == false)
            return std::nullopt;
        return std::string { text.substr(0, text.find(' ')) };
    }

    std::optional<std::vector<lfs_file>> parse_git_lfs_files_json(const std::string_view bytes)
    {
        const json root { json::parse(bytes.begin(), bytes.end(), nullptr, false) };
        if (root.is_object() == false)
            return std::nullopt;
        const auto files { root.find("files") };
        // 추적하는 파일이 없으면 git-lfs가 `"files": null`을 줄 수 있다.
        if (files == root.end() || (files->is_array() == false && files->is_null() == false))
            return std::nullopt;

        std::vector<lfs_file> result {};
        if (files->is_null())
            return result;
        for (const json& item : *files)
        {
            if (item.is_object() == false || item.contains("name") == false || item["name"].is_string() == false)
                continue;
            lfs_file file {};
            file.path = item["name"].get<std::string>();
            if (item.contains("oid") && item["oid"].is_string())
                file.oid = item["oid"].get<std::string>();
            if (item.contains("size") && item["size"].is_number_unsigned())
                file.size = item["size"].get<std::uint64_t>();
            if (item.contains("downloaded") && item["downloaded"].is_boolean())
                file.downloaded = item["downloaded"].get<bool>();
            if (item.contains("checkout") && item["checkout"].is_boolean())
                file.checked_out = item["checkout"].get<bool>();
            result.push_back(std::move(file));
        }
        return result;
    }

    std::vector<lfs_file> parse_git_lfs_files_long(const std::string_view bytes)
    {
        std::vector<lfs_file> result {};
        std::string_view rest { bytes };
        while (rest.empty() == false)
        {
            std::string_view line {};
            git_text::take_field(rest, '\n', line);
            if (line.empty() == false && line.back() == '\r')
                line.remove_suffix(1);
            // `<oid> <표지> <경로>`
            if (line.size() < sha256_hex_length + 4 || is_oid(line.substr(0, sha256_hex_length)) == false || line[sha256_hex_length] != ' ' || line[sha256_hex_length + 2] != ' ')
                continue;
            const char marker { line[sha256_hex_length + 1] };
            if (marker != '*' && marker != '-')
                continue;
            lfs_file file {};
            file.oid = std::string { line.substr(0, sha256_hex_length) };
            file.path = std::string { line.substr(sha256_hex_length + 3) };
            file.checked_out = marker == '*';
            file.downloaded = file.checked_out;
            result.push_back(std::move(file));
        }
        return result;
    }

    bool gitattributes_uses_lfs(const std::string_view text)
    {
        std::string_view rest { text };
        while (rest.empty() == false)
        {
            std::string_view line {};
            git_text::take_field(rest, '\n', line);
            line = trimmed(line);
            if (line.empty() || line.front() == '#')
                continue;
            if (line_sets_lfs_filter(line))
                return true;
        }
        return false;
    }

    std::set<std::string> parse_git_check_attr_lfs_paths(const std::string_view bytes)
    {
        std::set<std::string> paths {};
        std::string_view rest { bytes };
        while (rest.empty() == false)
        {
            std::string_view path {};
            std::string_view attribute {};
            std::string_view value {};
            if (git_text::take_field(rest, '\0', path) == false)
                break;
            if (git_text::take_field(rest, '\0', attribute) == false)
                break;
            static_cast<void>(git_text::take_field(rest, '\0', value));
            if (attribute == "filter" && value == "lfs")
                paths.insert(std::string { path });
        }
        return paths;
    }

    std::string lfs_object_relative_path(const std::string_view oid)
    {
        if (is_oid(oid) == false)
            return {};
        std::string path { "lfs/objects/" };
        path.append(oid.substr(0, 2)).append("/").append(oid.substr(2, 2)).append("/").append(oid);
        return path;
    }
} // namespace cyella
