#include "domain/ssh_remote.h"

#include <algorithm>
#include <cctype>

namespace cyella {
    namespace {
        bool is_space(const char value) noexcept
        {
            return value == ' ' || value == '\t' || value == '\r' || value == '\n';
        }

        std::string_view trim(std::string_view text) noexcept
        {
            while (text.empty() == false && is_space(text.back()))
                text.remove_suffix(1);
            while (text.empty() == false && is_space(text.front()))
                text.remove_prefix(1);
            return text;
        }

        std::string lower(const std::string_view text)
        {
            std::string lowered { text };
            std::ranges::transform(lowered, lowered.begin(), [](const char value) { return static_cast<char>(std::tolower(static_cast<unsigned char>(value))); });
            return lowered;
        }

        // 계정·호스트는 ssh 명령줄에 그대로 서므로 옵션 주입(`-oProxyCommand=...`)과 공백이 섞이지 않는 글자만 받는다.
        bool safe_name(const std::string_view value, const std::string_view extra_characters) noexcept
        {
            if (value.empty() || value.front() == '-')
                return false;
            for (const char character : value)
                if (std::isalnum(static_cast<unsigned char>(character)) == 0 && extra_characters.find(character) == std::string_view::npos)
                    return false;
            return true;
        }

        std::optional<std::uint16_t> parse_port(const std::string_view text) noexcept
        {
            if (text.empty() || text.size() > 5 || std::ranges::all_of(text, [](const char value) { return value >= '0' && value <= '9'; }) == false)
                return std::nullopt;
            unsigned value { 0 };
            for (const char digit : text)
                value = value * 10u + static_cast<unsigned>(digit - '0');
            if (value == 0u || value > 65535u)
                return std::nullopt;
            return static_cast<std::uint16_t>(value);
        }

        // 경로는 셸 따옴표 안에 들어가므로 글자는 넓게 받되, 제어 문자와 옵션처럼 보이는 앞 `-`는 거절한다.
        // 홈 접두어(`~`, `~계정`)는 셸이 펼치도록 따옴표 밖에 두므로 계정 부분을 좁게 본다.
        bool valid_path(const std::string_view path) noexcept
        {
            if (path.empty() || path.front() == '-' || path == "/")
                return false;
            if (std::ranges::any_of(path, [](const char value) { return static_cast<unsigned char>(value) < 0x20u || value == 0x7f; }))
                return false;
            if (path.front() != '~')
                return true;
            const std::size_t slash { path.find('/') };
            const std::string_view home_user { path.substr(1, slash == std::string_view::npos ? std::string_view::npos : slash - 1) };
            if (home_user.empty() == false && safe_name(home_user, "._-") == false)
                return false;
            // 홈 폴더 자체(`~`, `~/`)에는 저장소를 만들지 않는다.
            return slash != std::string_view::npos && slash + 1 < path.size();
        }

        bool split_user(std::string_view& authority, std::string& user)
        {
            const std::size_t at { authority.rfind('@') };
            if (at == std::string_view::npos)
                return true;
            user = std::string { authority.substr(0, at) };
            authority.remove_prefix(at + 1);
            return safe_name(user, "._-");
        }
    } // namespace

    std::string ssh_remote_target::destination() const
    {
        return user.empty() ? host : user + "@" + host;
    }

    std::optional<ssh_remote_target> parse_ssh_remote_target(const std::string_view raw_url)
    {
        const std::string_view url { trim(raw_url) };
        if (url.empty())
            return std::nullopt;
        ssh_remote_target target {};
        if (const std::size_t separator { url.find("://") }; separator != std::string_view::npos)
        {
            const std::string scheme { lower(url.substr(0, separator)) };
            if (scheme != "ssh" && scheme != "git+ssh" && scheme != "ssh+git")
                return std::nullopt;
            const std::string_view rest { url.substr(separator + 3) };
            const std::size_t slash { rest.find('/') };
            if (slash == std::string_view::npos)
                return std::nullopt;
            std::string_view authority { rest.substr(0, slash) };
            if (split_user(authority, target.user) == false || authority.empty())
                return std::nullopt;
            std::string_view host { authority };
            std::string_view port_text {};
            if (authority.front() == '[')
            {
                const std::size_t close { authority.find(']') };
                if (close == std::string_view::npos)
                    return std::nullopt;
                host = authority.substr(1, close - 1);
                const std::string_view after { authority.substr(close + 1) };
                if (after.empty() == false && after.front() != ':')
                    return std::nullopt;
                if (after.empty() == false)
                    port_text = after.substr(1);
                if (safe_name(host, ":.") == false)
                    return std::nullopt;
            }
            else
            {
                if (const std::size_t colon { authority.find(':') }; colon != std::string_view::npos)
                {
                    host = authority.substr(0, colon);
                    port_text = authority.substr(colon + 1);
                }
                if (safe_name(host, "._-") == false)
                    return std::nullopt;
            }
            if (port_text.empty() == false)
            {
                target.port = parse_port(port_text);
                if (target.port.has_value() == false)
                    return std::nullopt;
            }
            target.host = lower(host);
            // git은 `/~`로 시작하는 경로에서 앞 `/`를 떼어 홈 기준으로 넘긴다.
            std::string_view path { rest.substr(slash) };
            if (path.size() > 1 && path[1] == '~')
                path.remove_prefix(1);
            if (valid_path(path) == false)
                return std::nullopt;
            target.path = std::string { path };
            return target;
        }

        // scp 꼴: [user@]host:path. 첫 `:` 앞에 `/`·`\`가 있으면 로컬 경로다.
        const std::size_t colon { url.find(':') };
        if (colon == std::string_view::npos || colon == 0)
            return std::nullopt;
        std::string_view authority { url.substr(0, colon) };
        if (authority.find('/') != std::string_view::npos || authority.find('\\') != std::string_view::npos)
            return std::nullopt;
        if (split_user(authority, target.user) == false)
            return std::nullopt;
        // `C:\repo`·`C:/repo`는 드라이브 문자다.
        if (authority.size() == 1 && std::isalpha(static_cast<unsigned char>(authority.front())) != 0)
            return std::nullopt;
        if (safe_name(authority, "._-") == false)
            return std::nullopt;
        const std::string_view path { url.substr(colon + 1) };
        if (valid_path(path) == false)
            return std::nullopt;
        target.host = lower(authority);
        target.path = std::string { path };
        return target;
    }
} // namespace cyella
