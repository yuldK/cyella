#include "domain/remote_web_link.h"
#include "presentation/codicon_names.h"

#include "luil/generated/codicons.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace cyella;

namespace {
    std::string url_of(const std::string_view remote, const std::vector<web_link_setting>& links = {}, const std::vector<hosting_host_setting>& hosts = {})
    {
        const std::optional<remote_web_link> link { resolve_remote_web_link(remote, links, hosts) };
        return link.has_value() ? link->url : std::string { "(none)" };
    }
} // namespace

TEST_CASE("known public services turn https, ssh and scp remotes into the repository page", "[web_link]")
{
    CHECK(url_of("https://github.com/octo/hello.git") == "https://github.com/octo/hello");
    CHECK(url_of("git@github.com:octo/hello.git") == "https://github.com/octo/hello");
    CHECK(url_of("ssh://git@ssh.github.com:443/octo/hello.git") == "https://github.com/octo/hello");
    // 사용자 정보는 주소에 남지 않는다.
    CHECK(url_of("https://me:token@gitlab.com/group/sub/app.git") == "https://gitlab.com/group/sub/app");
    CHECK(url_of("git@gitea.com:me/tool.git") == "https://gitea.com/me/tool");
    CHECK(url_of("https://codeberg.org/me/tool") == "https://codeberg.org/me/tool");
    CHECK(url_of("git@bitbucket.org:team/repo.git") == "https://bitbucket.org/team/repo");

    const std::optional<remote_web_link> github { resolve_remote_web_link("git@github.com:octo/hello.git", {}, {}) };
    REQUIRE(github.has_value());
    CHECK(github->service == web_link_service::github);
    CHECK(github->icon == "github");
    CHECK(resolve_remote_web_link("git@gitea.com:me/tool.git", {}, {})->service == web_link_service::gitea);

    // 로컬 경로는 링크가 없다.
    CHECK(url_of("C:\\repos\\shared.git") == "(none)");
    CHECK(url_of("../other") == "(none)");
}

TEST_CASE("a host no rule knows still links to the same host and guesses its service from the name", "[web_link]")
{
    CHECK(url_of("https://git.unknown.example/me/x.git") == "https://git.unknown.example/me/x");
    CHECK(url_of("git@git.unknown.example:me/x.git") == "https://git.unknown.example/me/x");
    // ssh 포트는 웹 주소에 남지 않고, http(s) 포트는 남는다. 내장 서비스도 다른 포트면 그 출처다.
    CHECK(url_of("ssh://git@git.unknown.example:2222/me/x.git") == "https://git.unknown.example/me/x");
    CHECK(url_of("http://192.168.0.10:8080/me/x.git") == "http://192.168.0.10:8080/me/x");
    CHECK(url_of("https://github.com:8443/octo/hello.git") == "https://github.com:8443/octo/hello");

    const std::optional<remote_web_link> gitlab { resolve_remote_web_link("git@gitlab.corp.example:team/sub/app.git", {}, {}) };
    REQUIRE(gitlab.has_value());
    CHECK(gitlab->url == "https://gitlab.corp.example/team/sub/app");
    CHECK(gitlab->service == web_link_service::gitlab);
    CHECK(gitlab->icon == "repo");
    CHECK(gitlab->host == "gitlab.corp.example");
    CHECK(resolve_remote_web_link("https://forgejo.example/me/x", {}, {})->service == web_link_service::gitea);

    const std::optional<remote_web_link> other { resolve_remote_web_link("https://git.unknown.example:8443/me/x.git", {}, {}) };
    REQUIRE(other.has_value());
    CHECK(other->service == web_link_service::other);
    CHECK(other->icon == "link-external");
    CHECK(other->host == "git.unknown.example:8443");
}

TEST_CASE("configured rules come before the hosting table and the built-in services", "[web_link]")
{
    const std::vector<hosting_host_setting> hosts { { .host = "git.corp.example", .provider = hosting_provider::gitlab, .api_base = "https://gitlab.corp.example/api/v4" } };
    // API 연동 서버는 그 웹 주소를 쓴다 (ssh 원격이면 API 주소의 출처).
    const std::optional<remote_web_link> hosted { resolve_remote_web_link("git@git.corp.example:team/app.git", {}, hosts) };
    REQUIRE(hosted.has_value());
    CHECK(hosted->url == "https://gitlab.corp.example/team/app");
    CHECK(hosted->service == web_link_service::gitlab);

    std::vector<web_link_setting> links {};
    // ssh 호스트와 웹 주소가 다른 Gitea 서버다.
    links.push_back({ .host = "ssh.code.example", .service = web_link_service::gitea, .web_base = "https://code.example/", .icon = "server" });
    // 포트가 있는 http 서버는 웹 주소를 비워도 원격의 출처를 쓴다.
    links.push_back({ .host = "127.0.0.1:3000", .service = web_link_service::gitea, .web_base = {}, .icon = {} });
    // 설정이 내장 서비스를 덮는다.
    links.push_back({ .host = "github.com", .service = web_link_service::other, .web_base = "https://mirror.example/github", .icon = "globe" });
    const std::optional<remote_web_link> gitea { resolve_remote_web_link("ssh://git@ssh.code.example:2222/me/tool.git", links, hosts) };
    REQUIRE(gitea.has_value());
    CHECK(gitea->url == "https://code.example/me/tool");
    CHECK(gitea->icon == "server");
    CHECK(gitea->host == "ssh.code.example");
    CHECK(url_of("http://127.0.0.1:3000/me/tool.git", links, hosts) == "http://127.0.0.1:3000/me/tool");
    CHECK(resolve_remote_web_link("http://127.0.0.1:3000/me/tool.git", links, hosts)->icon == "repo");
    CHECK(url_of("git@github.com:octo/hello.git", links, hosts) == "https://mirror.example/github/octo/hello");
}

TEST_CASE("a web link rule needs a readable host and an http(s) web address", "[web_link]")
{
    CHECK_FALSE(validate_web_link_setting({ .host = "git.example.com" }).has_value());
    CHECK_FALSE(validate_web_link_setting({ .host = "git.example.com:3000", .web_base = "http://git.example.com:3000" }).has_value());
    CHECK(validate_web_link_setting({ .host = "" }).has_value());
    CHECK(validate_web_link_setting({ .host = "git example" }).has_value());
    CHECK(validate_web_link_setting({ .host = "git.example.com", .web_base = "ftp://git.example.com" }).has_value());
    CHECK(validate_web_link_setting({ .host = "git.example.com", .web_base = "git.example.com" }).has_value());
    CHECK(validate_web_link_setting({ .host = "git.example.com", .web_base = "https:///path" }).has_value());
    for (const web_link_service service : { web_link_service::github, web_link_service::gitlab, web_link_service::gitea, web_link_service::bitbucket, web_link_service::other })
        CHECK(parse_web_link_service(web_link_service_name(service)) == service);
    CHECK_FALSE(parse_web_link_service("svn").has_value());
    CHECK(builtin_web_links().size() == 5);
}

TEST_CASE("codicon names map to the glyphs luil draws", "[web_link][codicons]")
{
    CHECK(find_codicon("github") == luil::codicons::icon_github);
    CHECK(find_codicon("GitHub") == luil::codicons::icon_github);
    CHECK(find_codicon(" link-external ") == luil::codicons::icon_link_external);
    CHECK(find_codicon("repo") == luil::codicons::icon_repo);
    // 별칭도 찾는다.
    CHECK(find_codicon("plus") == luil::codicons::icon_add);
    CHECK(find_codicon("not-a-codicon") == 0);
    CHECK(find_codicon("") == 0);
    // 서비스 기본 아이콘은 모두 codicon에 있다.
    for (const web_link_service service : { web_link_service::github, web_link_service::gitlab, web_link_service::gitea, web_link_service::bitbucket, web_link_service::other })
        CHECK(find_codicon(default_web_link_icon(service)) != 0);
}
