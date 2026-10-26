#include "domain/hosting_remote.h"

#include "domain/text_catalog.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace cyella {
    namespace {
        std::string lower(const std::string_view text)
        {
            std::string result { text };
            std::ranges::transform(result, result.begin(), [](const char value) { return static_cast<char>(std::tolower(static_cast<unsigned char>(value))); });
            return result;
        }

        std::string_view trim(std::string_view text)
        {
            while (text.empty() == false && std::isspace(static_cast<unsigned char>(text.front())) != 0)
                text.remove_prefix(1);
            while (text.empty() == false && std::isspace(static_cast<unsigned char>(text.back())) != 0)
                text.remove_suffix(1);
            return text;
        }

        bool starts_with(const std::string_view text, const std::string_view prefix)
        {
            return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
        }

        int hex_value(const char value)
        {
            if (value >= '0' && value <= '9')
                return value - '0';
            if (value >= 'a' && value <= 'f')
                return value - 'a' + 10;
            if (value >= 'A' && value <= 'F')
                return value - 'A' + 10;
            return -1;
        }

        std::string percent_decode(const std::string_view text)
        {
            std::string result {};
            result.reserve(text.size());
            for (std::size_t index { 0 }; index < text.size(); ++index)
            {
                if (text[index] == '%' && index + 2 < text.size() && hex_value(text[index + 1]) >= 0 && hex_value(text[index + 2]) >= 0)
                {
                    result.push_back(static_cast<char>(hex_value(text[index + 1]) * 16 + hex_value(text[index + 2])));
                    index += 2;
                    continue;
                }
                result.push_back(text[index]);
            }
            return result;
        }

        std::optional<std::uint16_t> parse_port(const std::string_view text)
        {
            if (text.empty() || text.size() > 5)
                return std::nullopt;
            unsigned value { 0 };
            const auto [end, error] { std::from_chars(text.data(), text.data() + text.size(), value) };
            if (error != std::errc {} || end != text.data() + text.size() || value == 0 || value > 65535)
                return std::nullopt;
            return static_cast<std::uint16_t>(value);
        }

        // 경로를 다듬는다: 퍼센트 해제, 앞뒤 `/`와 끝의 `.git`을 뗀다. 빈 구성 요소나 `.`·`..`이 있으면 값이 없다.
        std::optional<std::string> clean_path(const std::string_view raw)
        {
            std::string path { percent_decode(raw) };
            // 질의와 조각은 저장소 이름이 아니다.
            if (const std::size_t cut { path.find_first_of("?#") }; cut != std::string::npos)
                path.erase(cut);
            while (path.empty() == false && (path.front() == '/' || path.front() == '\\'))
                path.erase(path.begin());
            while (path.empty() == false && (path.back() == '/' || path.back() == '\\'))
                path.pop_back();
            if (path.size() > 4 && path.ends_with(".git"))
                path.erase(path.size() - 4);
            if (path.empty())
                return std::nullopt;
            std::size_t start { 0 };
            while (start <= path.size())
            {
                const std::size_t end { std::min(path.find('/', start), path.size()) };
                const std::string_view segment { std::string_view { path }.substr(start, end - start) };
                if (segment.empty() || segment == "." || segment == ".." || segment.find('\\') != std::string_view::npos)
                    return std::nullopt;
                start = end + 1;
            }
            return path;
        }

        // 권한 부분(`user:pw@host:port`)에서 호스트와 포트를 가른다.
        bool split_authority(std::string_view authority, std::string& host, std::optional<std::uint16_t>& port)
        {
            if (const std::size_t at { authority.rfind('@') }; at != std::string_view::npos)
                authority.remove_prefix(at + 1);
            port.reset();
            if (authority.empty())
                return false;
            if (authority.front() == '[')
            {
                const std::size_t close { authority.find(']') };
                if (close == std::string_view::npos)
                    return false;
                host = lower(authority.substr(1, close - 1));
                const std::string_view rest { authority.substr(close + 1) };
                if (rest.empty())
                    return host.empty() == false;
                if (rest.front() != ':')
                    return false;
                port = parse_port(rest.substr(1));
                return port.has_value() && host.empty() == false;
            }
            const std::size_t colon { authority.rfind(':') };
            if (colon == std::string_view::npos)
            {
                host = lower(authority);
                return true;
            }
            host = lower(authority.substr(0, colon));
            const std::string_view port_text { authority.substr(colon + 1) };
            // `host:` 처럼 포트가 비었으면 없는 것으로 본다.
            if (port_text.empty() == false)
            {
                port = parse_port(port_text);
                if (port.has_value() == false)
                    return false;
            }
            return host.empty() == false;
        }

        bool valid_host(const std::string_view host)
        {
            if (host.empty())
                return false;
            return std::ranges::all_of(host, [](const char value) { return std::isalnum(static_cast<unsigned char>(value)) != 0 || value == '.' || value == '-' || value == '_' || value == ':'; });
        }

        std::string_view scheme_of_url(const std::string_view url)
        {
            const std::size_t separator { url.find("://") };
            return separator == std::string_view::npos ? std::string_view {} : url.substr(0, separator);
        }

        // `http://host:8123/x`의 `http://host:8123`이다. 스킴이 http(s)가 아니면 빈 문자열이다.
        std::string origin_of(const std::string_view url)
        {
            const std::size_t separator { url.find("://") };
            if (separator == std::string_view::npos)
                return {};
            const std::string scheme { lower(url.substr(0, separator)) };
            if (scheme != "http" && scheme != "https")
                return {};
            const std::size_t end { url.find_first_of("/?#", separator + 3) };
            return scheme + "://" + std::string { url.substr(separator + 3, end == std::string_view::npos ? std::string_view::npos : end - separator - 3) };
        }

        std::string strip_trailing_slashes(std::string text)
        {
            while (text.empty() == false && text.back() == '/')
                text.pop_back();
            return text;
        }
    } // namespace

    std::string_view hosting_provider_name(const hosting_provider provider) noexcept
    {
        switch (provider)
        {
        case hosting_provider::github:
            return "github";
        case hosting_provider::gitlab:
            return "gitlab";
        case hosting_provider::unknown:
            return "unknown";
        }
        return "unknown";
    }

    std::optional<hosting_provider> parse_hosting_provider(const std::string_view name) noexcept
    {
        if (name == "github")
            return hosting_provider::github;
        if (name == "gitlab")
            return hosting_provider::gitlab;
        return std::nullopt;
    }

    std::string hosting_provider_display_name(const hosting_provider provider)
    {
        switch (provider)
        {
        case hosting_provider::github:
            return "GitHub";
        case hosting_provider::gitlab:
            return "GitLab";
        case hosting_provider::unknown:
            break;
        }
        return tr_narrow(u8"hosting_remote.unknown_host");
    }

    std::string_view hosting_change_request_abbreviation(const hosting_provider provider) noexcept
    {
        return provider == hosting_provider::gitlab ? "MR" : "PR";
    }

    std::string hosting_repository::full_name() const
    {
        return owner.empty() ? name : owner + "/" + name;
    }

    std::string hosting_repository::web_url() const
    {
        return web_base + "/" + full_name();
    }

    std::string normalize_hosting_host(const std::string_view host)
    {
        return lower(trim(host));
    }

    std::optional<remote_location> parse_remote_url(const std::string_view raw_url)
    {
        const std::string_view url { trim(raw_url) };
        if (url.empty())
            return std::nullopt;
        remote_location location {};
        if (const std::size_t separator { url.find("://") }; separator != std::string_view::npos)
        {
            std::string scheme { lower(url.substr(0, separator)) };
            if (scheme == "git+ssh" || scheme == "ssh+git")
                scheme = "ssh";
            if (scheme != "https" && scheme != "http" && scheme != "ssh" && scheme != "git")
                return std::nullopt;
            const std::string_view rest { url.substr(separator + 3) };
            const std::size_t slash { rest.find('/') };
            if (slash == std::string_view::npos)
                return std::nullopt;
            if (split_authority(rest.substr(0, slash), location.host, location.port) == false || valid_host(location.host) == false)
                return std::nullopt;
            const std::optional<std::string> path { clean_path(rest.substr(slash + 1)) };
            if (path.has_value() == false)
                return std::nullopt;
            location.scheme = std::move(scheme);
            location.path = *path;
            return location;
        }

        // scp 꼴: [user@]host:path. 첫 `:` 앞에 `/`가 있으면 경로다.
        const std::size_t colon { url.find(':') };
        if (colon == std::string_view::npos || colon == 0)
            return std::nullopt;
        const std::string_view head { url.substr(0, colon) };
        if (head.find('/') != std::string_view::npos || head.find('\\') != std::string_view::npos)
            return std::nullopt;
        std::string_view host_text { head };
        if (const std::size_t at { host_text.rfind('@') }; at != std::string_view::npos)
            host_text.remove_prefix(at + 1);
        // `C:\repo`·`C:/repo`는 드라이브 문자다.
        if (host_text.size() == 1 && std::isalpha(static_cast<unsigned char>(host_text.front())) != 0)
            return std::nullopt;
        location.host = lower(host_text);
        if (location.host.empty() || location.host.find(':') != std::string::npos || valid_host(location.host) == false)
            return std::nullopt;
        const std::optional<std::string> path { clean_path(url.substr(colon + 1)) };
        if (path.has_value() == false)
            return std::nullopt;
        location.scheme = "ssh";
        location.path = *path;
        return location;
    }

    std::optional<hosting_repository> resolve_hosting_repository(const std::string_view url, const std::string_view remote_name, const std::vector<hosting_host_setting>& hosts)
    {
        const std::optional<remote_location> location { parse_remote_url(url) };
        if (location.has_value() == false)
            return std::nullopt;
        const std::size_t last_slash { location->path.rfind('/') };
        // 프로젝트는 `소유자/이름` 이상이어야 한다.
        if (last_slash == std::string::npos)
            return std::nullopt;

        const bool web_scheme { location->scheme == "https" || location->scheme == "http" };
        std::string bare_host { location->host };
        // ssh.github.com:443은 github.com의 ssh 우회 주소다.
        if (bare_host == "ssh.github.com")
            bare_host = "github.com";
        const bool default_port { location->port.has_value() == false || (location->scheme == "https" && *location->port == 443) || (location->scheme == "http" && *location->port == 80) };
        const std::string port_key { web_scheme && default_port == false ? bare_host + ":" + std::to_string(*location->port) : bare_host };

        const hosting_host_setting* configured { nullptr };
        for (const std::string& key : { port_key, bare_host })
        {
            const auto found { std::ranges::find_if(hosts, [&key](const hosting_host_setting& setting) { return normalize_hosting_host(setting.host) == key; }) };
            if (found != hosts.end())
            {
                configured = &*found;
                break;
            }
        }

        hosting_repository result {};
        result.remote_name = std::string { remote_name };
        result.owner = location->path.substr(0, last_slash);
        result.name = location->path.substr(last_slash + 1);
        result.host_key = port_key;
        result.credential_host = port_key;
        result.credential_protocol = location->scheme == "http" ? "http" : "https";

        const std::string site_origin { web_scheme ? (location->scheme + "://" + port_key) : "https://" + bare_host };
        if (configured != nullptr)
        {
            result.configured = true;
            result.provider = configured->provider;
            const std::string api { strip_trailing_slashes(std::string { trim(configured->api_base) }) };
            if (api.empty())
            {
                result.web_base = site_origin;
                result.api_base = site_origin + (configured->provider == hosting_provider::gitlab ? "/api/v4" : "/api/v3");
            }
            else
            {
                result.api_base = api;
                // ssh 원격이면 웹 주소를 알 수 없으므로 API 주소의 출처를 쓴다.
                const std::string api_origin { origin_of(api) };
                result.web_base = web_scheme || api_origin.empty() ? site_origin : api_origin;
                if (starts_with(api, "http://"))
                    result.credential_protocol = "http";
            }
            return result;
        }
        if (bare_host == "github.com" && port_key == bare_host)
        {
            result.provider = hosting_provider::github;
            result.web_base = "https://github.com";
            result.api_base = "https://api.github.com";
            result.credential_host = "github.com";
            result.credential_protocol = "https";
            return result;
        }
        if (bare_host == "gitlab.com" && port_key == bare_host)
        {
            result.provider = hosting_provider::gitlab;
            result.web_base = "https://gitlab.com";
            result.api_base = "https://gitlab.com/api/v4";
            result.credential_host = "gitlab.com";
            result.credential_protocol = "https";
            return result;
        }
        result.provider = hosting_provider::unknown;
        result.web_base = site_origin;
        return result;
    }

    std::optional<hosting_repository> hosting_repository_for_host(const std::string_view host, const std::vector<hosting_host_setting>& hosts)
    {
        const std::string key { normalize_hosting_host(host) };
        if (key.empty())
            return std::nullopt;
        // http로만 여는 내부 서버면 그 서버에 만든 저장소의 원격도 http다. 같은 규칙으로 풀리도록 scheme을 맞춘다.
        const auto configured { std::ranges::find_if(hosts, [&key](const hosting_host_setting& setting) { return normalize_hosting_host(setting.host) == key; }) };
        const bool http_only { configured != hosts.end() && starts_with(lower(std::string { trim(configured->api_base) }), "http://") };
        std::optional<hosting_repository> resolved { resolve_hosting_repository(std::string { http_only ? "http://" : "https://" } + key + "/owner/name", {}, hosts) };
        if (resolved.has_value() == false || resolved->supported() == false)
            return std::nullopt;
        resolved->owner.clear();
        resolved->name.clear();
        return resolved;
    }

    std::optional<hosting_repository> pick_hosting_repository(const std::vector<hosting_remote_candidate>& remotes, const std::vector<hosting_host_setting>& hosts)
    {
        std::optional<hosting_repository> fallback {};
        const auto consider = [&](const hosting_remote_candidate& remote) -> std::optional<hosting_repository> {
            std::optional<hosting_repository> resolved { resolve_hosting_repository(remote.url, remote.name, hosts) };
            if (resolved.has_value() && resolved->supported())
                return resolved;
            if (resolved.has_value() && fallback.has_value() == false)
                fallback = std::move(resolved);
            return std::nullopt;
        };
        for (const hosting_remote_candidate& remote : remotes)
            if (remote.name == "origin")
                if (std::optional<hosting_repository> found { consider(remote) }; found.has_value())
                    return found;
        for (const hosting_remote_candidate& remote : remotes)
            if (remote.name != "origin")
                if (std::optional<hosting_repository> found { consider(remote) }; found.has_value())
                    return found;
        return fallback;
    }

    std::optional<std::string> validate_hosting_host_setting(const hosting_host_setting& setting)
    {
        const std::string host { normalize_hosting_host(setting.host) };
        if (host.empty())
            return tr_narrow(u8"hosting_remote.host_empty");
        if (std::ranges::any_of(host, [](const char value) { return std::isalnum(static_cast<unsigned char>(value)) == 0 && value != '.' && value != '-' && value != '_' && value != ':'; }))
            return tr_narrow(u8"hosting_remote.host_invalid_characters");
        if (const std::size_t colon { host.find(':') }; colon != std::string::npos)
        {
            if (host.find(':', colon + 1) != std::string::npos || colon == 0 || parse_port(std::string_view { host }.substr(colon + 1)).has_value() == false)
                return tr_narrow(u8"hosting_remote.host_invalid_port");
        }
        const std::string_view api { trim(setting.api_base) };
        if (api.empty())
            return std::nullopt;
        const std::string_view scheme { scheme_of_url(api) };
        const std::string scheme_lower { lower(scheme) };
        if (scheme_lower != "http" && scheme_lower != "https")
            return tr_narrow(u8"hosting_remote.api_scheme_invalid");
        const std::string_view rest { api.substr(scheme.size() + 3) };
        if (rest.empty() || rest.front() == '/' || std::ranges::any_of(api, [](const char value) { return std::isspace(static_cast<unsigned char>(value)) != 0; }))
            return tr_narrow(u8"hosting_remote.api_unreadable");
        return std::nullopt;
    }
} // namespace cyella
