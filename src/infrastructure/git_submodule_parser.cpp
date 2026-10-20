#include "infrastructure/git_submodule_parser.h"

#include "infrastructure/git_text.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <utility>

namespace cyella {
    namespace {
        bool is_hex(const std::string_view text) noexcept
        {
            if (text.empty())
                return false;
            return std::ranges::all_of(text, [](const char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
        }

        std::string_view trim_carriage_return(std::string_view line) noexcept
        {
            if (line.empty() == false && line.back() == '\r')
                line.remove_suffix(1);
            return line;
        }

        bool is_zero(const std::string_view text) noexcept
        {
            return text.empty() == false && std::ranges::all_of(text, [](const char value) { return value == '0'; });
        }

        // `submodule.<name>.<variable>`을 이름과 변수로 가른다. 이름에 점이 들어갈 수 있어 마지막 점이 경계다.
        bool split_section_key(const std::string_view key, std::string_view& name, std::string_view& variable) noexcept
        {
            constexpr std::string_view prefix { "submodule." };
            if (git_text::starts_with(key, prefix) == false)
                return false;
            const std::string_view rest { key.substr(prefix.size()) };
            const std::size_t dot { rest.rfind('.') };
            if (dot == std::string_view::npos || dot == 0 || dot + 1 >= rest.size())
                return false;
            name = rest.substr(0, dot);
            variable = rest.substr(dot + 1);
            return true;
        }

        bool changed_content(const std::string& flags) noexcept
        {
            // `SCMU`: 셋째가 추적 파일 수정(M), 넷째가 추적 안 된 파일(U)이다.
            return flags.size() >= 4 && (flags[2] == 'M' || flags[3] == 'U');
        }
    } // namespace

    std::vector<submodule_status_entry> parse_git_submodule_status(const std::string_view bytes)
    {
        std::vector<submodule_status_entry> entries {};
        std::string_view rest { bytes };
        while (rest.empty() == false)
        {
            std::string_view line {};
            git_text::take_field(rest, '\n', line);
            line = trim_carriage_return(line);
            if (line.size() < 3)
                continue;

            submodule_status_entry entry {};
            const char prefix { line.front() };
            if (prefix != ' ' && prefix != '-' && prefix != '+' && prefix != 'U')
                continue;
            entry.prefix = prefix;
            line.remove_prefix(1);

            const std::size_t space { line.find(' ') };
            if (space == std::string_view::npos || is_hex(line.substr(0, space)) == false)
                continue;
            entry.sha = std::string { line.substr(0, space) };
            line.remove_prefix(space + 1);

            // 경로에는 공백이 들어갈 수 있다. 줄이 `)`로 끝나면 마지막 ` (`부터가 설명이다.
            if (line.empty() == false && line.back() == ')')
            {
                const std::size_t open { line.rfind(" (") };
                if (open != std::string_view::npos && open > 0)
                {
                    entry.description = std::string { line.substr(open + 2, line.size() - open - 3) };
                    line = line.substr(0, open);
                }
            }
            if (line.empty())
                continue;
            entry.path = std::string { line };
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    std::vector<gitmodules_entry> parse_gitmodules_config(const std::string_view bytes)
    {
        std::vector<gitmodules_entry> entries {};
        std::string_view rest { bytes };
        while (rest.empty() == false)
        {
            std::string_view record {};
            git_text::take_field(rest, '\0', record);
            if (record.empty())
                continue;
            // `-z`는 `key\nvalue`를 NUL로 끝낸다. 값이 없는 키는 개행이 없다.
            const std::size_t newline { record.find('\n') };
            const std::string_view key { record.substr(0, newline) };
            const std::string_view value { newline == std::string_view::npos ? std::string_view {} : record.substr(newline + 1) };

            std::string_view name {};
            std::string_view variable {};
            if (split_section_key(key, name, variable) == false)
                continue;
            auto found { std::ranges::find_if(entries, [name](const gitmodules_entry& entry) { return entry.name == name; }) };
            if (found == entries.end())
            {
                entries.push_back({ .name = std::string { name } });
                found = std::prev(entries.end());
            }
            if (variable == "path")
                found->path = std::string { value };
            else if (variable == "url")
                found->url = std::string { value };
        }
        return entries;
    }

    std::vector<gitlink_entry> parse_git_gitlinks(const std::string_view bytes)
    {
        std::vector<gitlink_entry> entries {};
        std::string_view rest { bytes };
        while (rest.empty() == false)
        {
            std::string_view record {};
            git_text::take_field(rest, '\0', record);
            // `<mode> <sha> <stage>\t<path>`
            const std::size_t tab { record.find('\t') };
            if (tab == std::string_view::npos)
                continue;
            std::string_view meta { record.substr(0, tab) };
            std::string_view mode {};
            std::string_view sha {};
            git_text::take_field(meta, ' ', mode);
            git_text::take_field(meta, ' ', sha);
            if (mode != "160000")
                continue;
            const auto stage { git_text::parse_integer<int>(meta) };
            entries.push_back({ .path = std::string { record.substr(tab + 1) }, .sha = std::string { sha }, .stage = stage.value_or(0) });
        }
        return entries;
    }

    std::vector<submodule_info> assemble_submodules(const submodule_sources& sources, const std::function<std::optional<bool>(const submodule_info&)>& descends)
    {
        std::vector<submodule_info> result {};
        result.reserve(sources.status.size());
        for (const submodule_status_entry& status : sources.status)
        {
            submodule_info info {};
            info.path = status.path;
            info.description = status.description;
            if (status.prefix == '-')
                info.recorded_sha = status.sha;
            else if (is_zero(status.sha) == false)
                info.checked_out_sha = status.sha;

            for (const gitmodules_entry& module : sources.modules)
            {
                if (module.path != info.path)
                    continue;
                info.name = module.name;
                info.url = module.url;
                break;
            }
            // 기록된 커밋은 상위 저장소 index에서만 알 수 있다. 병합 충돌 중이면 stage 1·2·3이 따로 있어 건너뛴다.
            if (info.recorded_sha.empty())
            {
                for (const gitlink_entry& link : sources.gitlinks)
                    if (link.path == info.path && link.stage == 0)
                    {
                        info.recorded_sha = link.sha;
                        break;
                    }
                // 중첩 submodule의 일치 상태는 기록이 곧 체크아웃이다.
                if (info.recorded_sha.empty() && status.prefix == ' ')
                    info.recorded_sha = info.checked_out_sha;
            }

            bool dirty { false };
            for (const file_change& change : sources.changes)
                if (change.is_submodule && change.path == info.path && changed_content(change.submodule_flags))
                    dirty = true;

            switch (status.prefix)
            {
            case '-':
                info.state = submodule_state::uninitialized;
                break;
            case 'U':
                info.state = submodule_state::conflict;
                break;
            case '+':
                info.state = descends != nullptr && descends(info).value_or(false) ? submodule_state::modified : submodule_state::out_of_date;
                break;
            default:
                info.state = dirty ? submodule_state::modified : submodule_state::initialized;
                break;
            }
            result.push_back(std::move(info));
        }

        // 다른 submodule 안에 있으면 중첩이다.
        for (submodule_info& info : result)
            for (const submodule_info& other : result)
                if (&other != &info && info.path.size() > other.path.size() && info.path.starts_with(other.path) && info.path[other.path.size()] == '/')
                    ++info.depth;
        return result;
    }
} // namespace cyella
