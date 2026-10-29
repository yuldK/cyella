#include "domain/remote_web_link.h"

#include "domain/text_catalog.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace cyella {
    namespace {
        std::string lower(const std::string_view text)
        {
            std::string result { text };
            std::ranges::transform(result, result.begin(), [](const unsigned char value) { return static_cast<char>(std::tolower(value)); });
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

        std::string without_trailing_slashes(std::string text)
        {
            while (text.empty() == false && text.back() == '/')
                text.pop_back();
            return text;
        }

        struct builtin_link
        {
            std::string_view host {};
            web_link_service service { web_link_service::other };
        };

        // 공개 서비스다. 원격 URL의 호스트가 곧 웹 주소의 호스트다.
        constexpr std::array builtin_links {
            builtin_link { "github.com", web_link_service::github },
            builtin_link { "gitlab.com", web_link_service::gitlab },
            builtin_link { "gitea.com", web_link_service::gitea },
            builtin_link { "codeberg.org", web_link_service::gitea },
            builtin_link { "bitbucket.org", web_link_service::bitbucket },
        };

        // 규칙에 없는 호스트의 서비스를 이름으로 짐작한다 (`gitlab.corp.example`은 GitLab). 이름표와 기본 아이콘만 정하므로 틀려도 주소는 같다.
        web_link_service guess_service(const std::string_view host)
        {
            const auto has = [host](const std::string_view word) { return host.find(word) != std::string_view::npos; };
            if (has("github"))
                return web_link_service::github;
            if (has("gitlab"))
                return web_link_service::gitlab;
            if (has("gitea") || has("forgejo") || has("codeberg"))
                return web_link_service::gitea;
            if (has("bitbucket"))
                return web_link_service::bitbucket;
            return web_link_service::other;
        }

        remote_web_link make_link(const std::string& base, const std::string& path, const web_link_service service, std::string host, const std::string_view icon)
        {
            remote_web_link link {};
            link.url = without_trailing_slashes(base) + "/" + path;
            link.service = service;
            link.host = std::move(host);
            link.icon = std::string { trim(icon).empty() ? default_web_link_icon(service) : trim(icon) };
            return link;
        }
    } // namespace

    std::string_view web_link_service_name(const web_link_service service) noexcept
    {
        switch (service)
        {
        case web_link_service::github:
            return "github";
        case web_link_service::gitlab:
            return "gitlab";
        case web_link_service::gitea:
            return "gitea";
        case web_link_service::bitbucket:
            return "bitbucket";
        case web_link_service::other:
            return "other";
        }
        return "other";
    }

    std::optional<web_link_service> parse_web_link_service(const std::string_view name) noexcept
    {
        for (const web_link_service service : { web_link_service::github, web_link_service::gitlab, web_link_service::gitea, web_link_service::bitbucket, web_link_service::other })
            if (web_link_service_name(service) == name)
                return service;
        return std::nullopt;
    }

    std::string web_link_service_display_name(const web_link_service service)
    {
        switch (service)
        {
        case web_link_service::github:
            return "GitHub";
        case web_link_service::gitlab:
            return "GitLab";
        case web_link_service::gitea:
            return "Gitea";
        case web_link_service::bitbucket:
            return "Bitbucket";
        case web_link_service::other:
            return {};
        }
        return {};
    }

    std::string_view default_web_link_icon(const web_link_service service) noexcept
    {
        switch (service)
        {
        case web_link_service::github:
            return "github";
        case web_link_service::gitlab:
        case web_link_service::gitea:
        case web_link_service::bitbucket:
            return "repo";
        case web_link_service::other:
            return "link-external";
        }
        return "link-external";
    }

    std::optional<remote_web_link> resolve_remote_web_link(const std::string_view remote_url, const std::vector<web_link_setting>& links, const std::vector<hosting_host_setting>& hosts)
    {
        const std::optional<remote_location> location { parse_remote_url(remote_url) };
        if (location.has_value() == false || location->path.empty())
            return std::nullopt;
        const bool web_scheme { location->scheme == "https" || location->scheme == "http" };
        // ssh.github.com:443은 github.com의 ssh 우회 주소다 (`resolve_hosting_repository`와 같은 규칙).
        const std::string bare_host { location->host == "ssh.github.com" ? std::string { "github.com" } : location->host };
        const bool default_port { location->port.has_value() == false || (location->scheme == "https" && *location->port == 443) || (location->scheme == "http" && *location->port == 80) };
        const std::string port_key { web_scheme && default_port == false ? bare_host + ":" + std::to_string(*location->port) : bare_host };
        // 웹 주소를 적지 않은 규칙은 원격이 http(s)면 그 출처, 아니면 https다.
        const std::string site_origin { web_scheme ? location->scheme + "://" + port_key : "https://" + bare_host };

        for (const std::string& key : { port_key, bare_host })
        {
            const auto found { std::ranges::find_if(links, [&key](const web_link_setting& setting) { return normalize_hosting_host(setting.host) == key; }) };
            if (found == links.end())
                continue;
            const std::string web_base { trim(found->web_base) };
            return make_link(web_base.empty() ? site_origin : web_base, location->path, found->service, key, found->icon);
        }
        // API 연동 서버는 이미 웹 주소를 안다.
        if (const std::optional<hosting_repository> repository { resolve_hosting_repository(remote_url, {}, hosts) }; repository.has_value() && repository->configured)
        {
            const web_link_service service { repository->provider == hosting_provider::gitlab ? web_link_service::gitlab : web_link_service::github };
            return make_link(repository->web_base, location->path, service, repository->host_key, {});
        }
        // 내장 서비스는 기본 포트에서만 그 서비스다. 다른 포트는 아래처럼 원격의 출처를 그대로 쓴다.
        if (port_key == bare_host)
        {
            const auto builtin { std::ranges::find_if(builtin_links, [&bare_host](const builtin_link& link) { return link.host == bare_host; }) };
            if (builtin != builtin_links.end())
                return make_link("https://" + bare_host, location->path, builtin->service, bare_host, {});
        }
        // 어느 규칙에도 없는 호스트도 대개 같은 호스트에 웹 페이지가 있다. 다르면 웹 링크 표에 적는다.
        return make_link(site_origin, location->path, guess_service(bare_host), port_key, {});
    }

    std::vector<web_link_setting> builtin_web_links()
    {
        std::vector<web_link_setting> links {};
        for (const builtin_link& link : builtin_links)
            links.push_back(
                { .host = std::string { link.host }, .service = link.service, .web_base = "https://" + std::string { link.host }, .icon = std::string { default_web_link_icon(link.service) } });
        return links;
    }

    std::optional<std::string> validate_web_link_setting(const web_link_setting& setting)
    {
        // 호스트 규칙은 호스팅 표와 같다. 웹 주소는 따로 본다 (그쪽 문구는 API 주소를 말한다).
        if (std::optional<std::string> problem { validate_hosting_host_setting({ .host = setting.host, .provider = hosting_provider::github, .api_base = {} }) }; problem.has_value())
            return problem;
        const std::string_view base { trim(setting.web_base) };
        if (base.empty())
            return std::nullopt;
        const std::string scheme { lower(base.substr(0, base.find("://"))) };
        if (base.find("://") == std::string_view::npos || (scheme != "http" && scheme != "https"))
            return tr_narrow(u8"web_link.base_scheme_invalid");
        const std::string_view rest { base.substr(scheme.size() + 3) };
        if (rest.empty() || rest.front() == '/' || std::ranges::any_of(base, [](const char value) { return std::isspace(static_cast<unsigned char>(value)) != 0; }))
            return tr_narrow(u8"web_link.base_unreadable");
        return std::nullopt;
    }
} // namespace cyella
