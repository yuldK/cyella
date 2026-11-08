#include "domain/git_identity.h"

#include "domain/path_syntax.h"
#include "domain/text_catalog.h"

#include <algorithm>
#include <array>
#include <utility>

namespace cyella {
    namespace {
        constexpr std::u8string_view file_prefix { u8"file:" };
        constexpr std::u8string_view ssh_identities_only_suffix { u8" -o IdentitiesOnly=yes" };

        std::u8string forward_slashes(const std::u8string_view path)
        {
            std::u8string result { path };
            std::ranges::replace(result, u8'\\', u8'/');
            return result;
        }

        // sh의 작은따옴표 안은 아무것도 풀지 않는다. 프로필 검사가 경로의 작은따옴표를 막으므로 이스케이프는 두지 않는다.
        std::u8string quote_single(const std::u8string_view text)
        {
            return u8"'" + std::u8string { text } + u8"'";
        }

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
            else if (key == u8"credential.username")
                target = &identity.credential_username;
            // git은 `--get-regexp`의 키를 소문자로 낸다.
            else if (key == u8"core.sshcommand")
                target = &identity.ssh_command;
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

    std::u8string identity_origin_label(const identity_origin origin)
    {
        switch (origin)
        {
        case identity_origin::local:
            return tr(u8"identity.origin.local");
        case identity_origin::worktree:
            return u8"worktree";
        case identity_origin::global:
            return tr(u8"identity.origin.global");
        case identity_origin::include:
            return u8"includeIf";
        case identity_origin::system:
            return tr(u8"identity.origin.system");
        case identity_origin::command_line:
            return tr(u8"identity.origin.command_line");
        case identity_origin::unknown:
            break;
        }
        return tr(u8"identity.origin.unknown");
    }

    std::u8string describe_identity(const git_identity& identity)
    {
        const bool has_name { identity.name.has_value() && identity.name->value.empty() == false };
        const bool has_email { identity.email.has_value() && identity.email->value.empty() == false };
        if (has_name == false && has_email == false)
            return tr(u8"identity.not_set");
        std::u8string text { has_name ? identity.name->value : tr(u8"identity.no_name") };
        text += u8" <" + (has_email ? identity.email->value : tr(u8"identity.no_email")) + u8">";
        return text;
    }

    bool identity_matches_profile(const git_identity& identity, const user_profile& profile)
    {
        if (identity.name.has_value() == false || identity.email.has_value() == false || identity.name->value != profile.user_name || identity.email->value != profile.user_email)
            return false;
        if (profile.https_username.empty() == false && (identity.credential_username.has_value() == false || identity.credential_username->value != profile.https_username))
            return false;
        if (profile.ssh_key.empty() == false)
        {
            const std::optional<std::u8string> key { profile_ssh_key_of(identity) };
            if (key.has_value() == false || same_key_path(*key, profile.ssh_key) == false)
                return false;
        }
        return true;
    }

    std::u8string make_profile_ssh_command(const std::u8string_view ssh_executable, const std::u8string_view key_path)
    {
        return quote_single(forward_slashes(ssh_executable.empty() ? std::u8string_view { u8"ssh" } : ssh_executable)) + u8" -i " + quote_single(forward_slashes(key_path))
            + std::u8string { ssh_identities_only_suffix };
    }

    std::optional<std::u8string> profile_ssh_key_of(const std::u8string_view ssh_command)
    {
        // `<ssh> -i '<키>' -o IdentitiesOnly=yes`의 꼬리부터 거꾸로 읽는다. ssh 실행 파일 자리는 무엇이든 받는다.
        if (ssh_command.ends_with(ssh_identities_only_suffix) == false)
            return std::nullopt;
        std::u8string_view rest { ssh_command.substr(0, ssh_command.size() - ssh_identities_only_suffix.size()) };
        if (rest.size() < 2 || rest.back() != u8'\'')
            return std::nullopt;
        rest.remove_suffix(1);
        const std::size_t open { rest.rfind(u8'\'') };
        if (open == std::u8string_view::npos || open == 0)
            return std::nullopt;
        const std::u8string_view key { rest.substr(open + 1) };
        if (key.empty() || rest.substr(0, open).ends_with(u8" -i ") == false)
            return std::nullopt;
        return std::u8string { key };
    }

    std::optional<std::u8string> profile_ssh_key_of(const git_identity& identity)
    {
        if (identity.ssh_command.has_value() == false)
            return std::nullopt;
        return profile_ssh_key_of(identity.ssh_command->value);
    }

    bool same_key_path(const std::u8string_view left, const std::u8string_view right)
    {
        return normalize_path_key(left) == normalize_path_key(right);
    }

    std::u8string describe_access(const git_identity& identity)
    {
        std::u8string text {};
        if (identity.credential_username.has_value() && identity.credential_username->value.empty() == false)
            text = tr(u8"identity.access_user", { { u8"user", identity.credential_username->value } });
        if (const std::optional<std::u8string> key { profile_ssh_key_of(identity) }; key.has_value())
        {
            const std::size_t slash { key->find_last_of(u8"/\\") };
            const std::u8string name { slash == std::u8string::npos ? *key : key->substr(slash + 1) };
            text += (text.empty() ? std::u8string {} : std::u8string { u8" · " }) + tr(u8"identity.access_key", { { u8"key", name } });
        }
        return text;
    }

    std::u8string describe_identity_with_access(const git_identity& identity)
    {
        const std::u8string access { describe_access(identity) };
        return describe_identity_with_source(identity) + (access.empty() ? std::u8string {} : u8" · " + access);
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
        return text + tr(u8"identity.split_origin_suffix", { { u8"name_origin", name_origin }, { u8"email_origin", email_origin } });
    }
} // namespace cyella
