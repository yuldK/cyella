#include "domain/git_identity.h"

#include "domain/path_syntax.h"

#include <algorithm>
#include <array>
#include <utility>

namespace cyella {
    namespace {
        constexpr std::u8string_view file_prefix { u8"file:" };

        std::vector<std::u8string_view> split_tabs(const std::u8string_view line, const std::size_t limit)
        {
            std::vector<std::u8string_view> parts {};
            std::u8string_view rest { line };
            while (parts.size() + 1 < limit)
            {
                const std::size_t tab { rest.find(u8'\t') };
                if (tab == std::u8string_view::npos)
                    break;
                parts.push_back(rest.substr(0, tab));
                rest.remove_prefix(tab + 1);
            }
            parts.push_back(rest);
            return parts;
        }

        // git은 경로에 역슬래시·따옴표·제어 문자가 있으면 C 방식으로 따옴표를 씌운다 (`file:"C:\\Users\\me\\.gitconfig"`).
        // 따옴표로 시작하면 풀어 쓴다. 닫는 따옴표가 없는 글은 그대로 둔다.
        std::u8string unquote_c_style(const std::u8string_view text)
        {
            if (text.size() < 2 || text.front() != u8'"' || text.back() != u8'"')
                return std::u8string { text };
            std::u8string result {};
            for (std::size_t index { 1 }; index + 1 < text.size(); ++index)
            {
                const char8_t value { text[index] };
                if (value != u8'\\' || index + 2 >= text.size())
                {
                    result.push_back(value);
                    continue;
                }
                const char8_t escaped { text[++index] };
                switch (escaped)
                {
                case u8'n':
                    result.push_back(u8'\n');
                    break;
                case u8't':
                    result.push_back(u8'\t');
                    break;
                case u8'0':
                case u8'1':
                case u8'2':
                case u8'3': {
                    // 세 자리 8진수 바이트다 (ASCII 밖의 글자).
                    unsigned byte { static_cast<unsigned>(escaped - u8'0') };
                    for (int digits { 1 }; digits < 3 && index + 2 < text.size() && text[index + 1] >= u8'0' && text[index + 1] <= u8'7'; ++digits)
                        byte = byte * 8 + static_cast<unsigned>(text[++index] - u8'0');
                    result.push_back(static_cast<char8_t>(byte));
                    break;
                }
                default:
                    result.push_back(escaped);
                    break;
                }
            }
            return result;
        }

        bool ends_with_key(const std::u8string& key, const std::u8string_view suffix)
        {
            return key.size() >= suffix.size() && key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        bool is_absolute(const std::u8string_view file) noexcept
        {
            return file.size() >= 3 && file[1] == u8':' && (file[2] == u8'/' || file[2] == u8'\\');
        }

        identity_origin classify(const std::u8string_view scope, const std::u8string_view origin, const identity_origin_hints& hints, std::u8string& file)
        {
            if (origin.starts_with(file_prefix) == false)
            {
                file = std::u8string { origin };
                return identity_origin::command_line;
            }
            file = unquote_c_style(origin.substr(file_prefix.size()));
            const std::u8string key { normalize_path_key(file) };
            if (scope == u8"command")
                return identity_origin::command_line;
            if (scope == u8"worktree" || ends_with_key(key, u8"/config.worktree"))
                return identity_origin::worktree;
            if (scope == u8"local")
                return identity_origin::local;
            if (scope == u8"system")
                return identity_origin::system;
            if (scope == u8"global")
            {
                if (hints.global_files.empty())
                    return identity_origin::global;
                for (const std::u8string& global_file : hints.global_files)
                    if (normalize_path_key(global_file) == key)
                        return identity_origin::global;
                return identity_origin::include;
            }
            // --show-scope를 쓰지 않은 출력이다. 경로로 짐작한다.
            if (is_absolute(file) == false || ends_with_key(key, u8"/.git/config"))
                return identity_origin::local;
            if (ends_with_key(key, u8"/etc/gitconfig"))
                return identity_origin::system;
            for (const std::u8string& global_file : hints.global_files)
                if (normalize_path_key(global_file) == key)
                    return identity_origin::global;
            return hints.global_files.empty() ? identity_origin::global : identity_origin::include;
        }
    } // namespace

    git_identity parse_git_identity(const std::u8string_view output, const identity_origin_hints& hints)
    {
        git_identity identity {};
        std::u8string_view rest { output };
        while (rest.empty() == false)
        {
            const std::size_t newline { rest.find(u8'\n') };
            std::u8string_view line { rest.substr(0, newline) };
            rest = newline == std::u8string_view::npos ? std::u8string_view {} : rest.substr(newline + 1);
            while (line.empty() == false && line.back() == u8'\r')
                line.remove_suffix(1);
            if (line.empty())
                continue;
            // `scope\torigin\tkey value`다. `--show-scope` 없이 부른 출력은 앞의 범위가 없다.
            std::vector<std::u8string_view> parts { split_tabs(line, 3) };
            std::u8string_view scope {};
            std::u8string_view origin {};
            std::u8string_view entry {};
            if (parts.size() == 3)
            {
                scope = parts[0];
                origin = parts[1];
                entry = parts[2];
            }
            else if (parts.size() == 2)
            {
                origin = parts[0];
                entry = parts[1];
            }
            else
                continue;
            const std::size_t space { entry.find(u8' ') };
            const std::u8string_view key { entry.substr(0, space) };
            // 값이 없는 키(`[user] name`)는 빈 값이다.
            const std::u8string_view value { space == std::u8string_view::npos ? std::u8string_view {} : entry.substr(space + 1) };
            std::optional<identity_value>* target { nullptr };
            if (key == u8"user.name")
                target = &identity.name;
            else if (key == u8"user.email")
                target = &identity.email;
            else if (key == u8"user.signingkey")
                target = &identity.signing_key;
            if (target == nullptr)
                continue;
            identity_value found {};
            found.value = std::u8string { value };
            found.origin = classify(scope, origin, hints, found.file);
            // git은 나중에 읽은 값을 쓴다 (시스템 → 전역 → 포함 파일 → 저장소 순서로 출력된다).
            *target = std::move(found);
        }
        return identity;
    }

    std::u8string_view identity_origin_label(const identity_origin origin) noexcept
    {
        switch (origin)
        {
        case identity_origin::local:
            return u8"로컬";
        case identity_origin::worktree:
            return u8"worktree";
        case identity_origin::global:
            return u8"전역";
        case identity_origin::include:
            return u8"includeIf";
        case identity_origin::system:
            return u8"시스템";
        case identity_origin::command_line:
            return u8"명령줄";
        case identity_origin::unknown:
            break;
        }
        return u8"알 수 없음";
    }

    std::u8string describe_identity(const git_identity& identity)
    {
        const bool has_name { identity.name.has_value() && identity.name->value.empty() == false };
        const bool has_email { identity.email.has_value() && identity.email->value.empty() == false };
        if (has_name == false && has_email == false)
            return u8"설정되지 않음";
        std::u8string text { has_name ? identity.name->value : std::u8string { u8"(이름 없음)" } };
        text += u8" <" + (has_email ? identity.email->value : std::u8string { u8"이메일 없음" }) + u8">";
        return text;
    }

    bool identity_matches_profile(const git_identity& identity, const user_profile& profile) noexcept
    {
        return identity.name.has_value() && identity.email.has_value() && identity.name->value == profile.user_name && identity.email->value == profile.user_email;
    }

    std::u8string describe_identity_with_source(const git_identity& identity)
    {
        std::u8string text { describe_identity(identity) };
        if (identity.name.has_value() == false && identity.email.has_value() == false)
            return text;
        const std::u8string name_origin { identity.name.has_value() ? std::u8string { identity_origin_label(identity.name->origin) } : std::u8string {} };
        const std::u8string email_origin { identity.email.has_value() ? std::u8string { identity_origin_label(identity.email->origin) } : std::u8string {} };
        if (name_origin == email_origin || name_origin.empty() || email_origin.empty())
            return text + u8" · " + (name_origin.empty() ? email_origin : name_origin);
        return text + u8" · 이름 " + name_origin + u8" · 이메일 " + email_origin;
    }
} // namespace cyella
