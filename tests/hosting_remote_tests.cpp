#include "domain/hosting_remote.h"
#include "infrastructure/json_app_settings_store.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace cyella;

TEST_CASE("remote URL forms are split into host and path", "[hosting]")
{
    struct sample
    {
        const char* url;
        const char* scheme;
        const char* host;
        int port;
        const char* path;
    };
    const sample samples[] {
        { "https://github.com/owner/repo.git", "https", "github.com", 0, "owner/repo" },
        { "https://github.com/owner/repo", "https", "github.com", 0, "owner/repo" },
        { "https://github.com/owner/repo/", "https", "github.com", 0, "owner/repo" },
        { "  https://GitHub.com/Owner/Repo.git  ", "https", "github.com", 0, "Owner/Repo" },
        { "https://user:secret@github.com/owner/repo.git", "https", "github.com", 0, "owner/repo" },
        { "https://oauth2:token@gitlab.example.com:8443/group/sub/repo.git", "https", "gitlab.example.com", 8443, "group/sub/repo" },
        { "http://127.0.0.1:8123/owner/repo.git", "http", "127.0.0.1", 8123, "owner/repo" },
        { "git@github.com:owner/repo.git", "ssh", "github.com", 0, "owner/repo" },
        { "github.com:owner/repo", "ssh", "github.com", 0, "owner/repo" },
        { "git@gitlab.com:group/sub/deep/repo.git", "ssh", "gitlab.com", 0, "group/sub/deep/repo" },
        { "ssh://git@github.com/owner/repo.git", "ssh", "github.com", 0, "owner/repo" },
        { "ssh://git@ssh.github.com:443/owner/repo.git", "ssh", "ssh.github.com", 443, "owner/repo" },
        { "ssh://git@git.example.com:2222/team/repo.git", "ssh", "git.example.com", 2222, "team/repo" },
        { "git+ssh://git@git.example.com/team/repo.git", "ssh", "git.example.com", 0, "team/repo" },
        { "git://git.example.com/team/repo.git", "git", "git.example.com", 0, "team/repo" },
        { "ssh://git@[::1]:2222/team/repo.git", "ssh", "::1", 2222, "team/repo" },
        { "https://github.com/owner/my%20repo.git", "https", "github.com", 0, "owner/my repo" },
        { "https://github.com/owner/repo.git?x=1#frag", "https", "github.com", 0, "owner/repo" },
    };
    for (const sample& entry : samples)
    {
        CAPTURE(entry.url);
        const std::optional<remote_location> location = parse_remote_url(entry.url);
        REQUIRE(location.has_value());
        CHECK(location->scheme == entry.scheme);
        CHECK(location->host == entry.host);
        CHECK(location->port.value_or(0) == entry.port);
        CHECK(location->path == entry.path);
    }
}

TEST_CASE("the secret in a remote URL's user info is masked for display", "[hosting]")
{
    CHECK(mask_remote_url_userinfo("https://user:tok@h/r.git") == "https://user:***@h/r.git");
    // 사용자 이름 없이 들어간 값은 token이다.
    CHECK(mask_remote_url_userinfo("https://tok@h/r.git") == "https://***@h/r.git");
    // 비밀번호에 `@`가 들어가도 마지막 `@`까지 가린다.
    CHECK(mask_remote_url_userinfo("https://user:p@ss@h/r.git") == "https://user:***@h/r.git");
    // ssh의 접속 계정 이름, scp 꼴, 로컬 경로, 사용자 정보가 없는 URL은 그대로다.
    CHECK(mask_remote_url_userinfo("ssh://git@h/r.git") == "ssh://git@h/r.git");
    CHECK(mask_remote_url_userinfo("ssh://git:secret@h/r.git") == "ssh://git:***@h/r.git");
    CHECK(mask_remote_url_userinfo("git@github.com:me/r.git") == "git@github.com:me/r.git");
    CHECK(mask_remote_url_userinfo("C:/repo") == "C:/repo");
    CHECK(mask_remote_url_userinfo("https://h/r.git") == "https://h/r.git");
    CHECK(mask_remote_url_userinfo("https://h/path@x") == "https://h/path@x");
}

TEST_CASE("local paths and broken URLs are not parsed", "[hosting]")
{
    const char* const urls[] {
        "",
        "   ",
        "C:\\work\\repo",
        "C:/work/repo",
        "/srv/git/repo.git",
        "../repo",
        "./repo",
        "relative/path",
        "file:///srv/repo",
        "https://github.com",
        "https://github.com/",
        "https:///owner/repo",
        "https://github.com/owner/../repo",
        "https://host:99999/owner/repo",
        "git@:owner/repo",
        "ftp://host/owner/repo",
        "https://host:abc/owner/repo",
    };
    for (const char* url : urls)
    {
        CAPTURE(url);
        CHECK(parse_remote_url(url).has_value() == false);
    }
}

TEST_CASE("built-in hosts know their provider and API base", "[hosting]")
{
    const std::vector<hosting_host_setting> none {};
    SECTION("github.com의 https와 ssh")
    {
        for (const char* url : { "https://github.com/octo/hello.git", "git@github.com:octo/hello.git", "ssh://git@ssh.github.com:443/octo/hello.git", "ssh://git@github.com/octo/hello.git" })
        {
            CAPTURE(url);
            const std::optional<hosting_repository> repository = resolve_hosting_repository(url, "origin", none);
            REQUIRE(repository.has_value());
            CHECK(repository->provider == hosting_provider::github);
            CHECK(repository->owner == "octo");
            CHECK(repository->name == "hello");
            CHECK(repository->api_base == "https://api.github.com");
            CHECK(repository->web_base == "https://github.com");
            CHECK(repository->credential_host == "github.com");
            CHECK(repository->credential_protocol == "https");
            CHECK(repository->host_key == "github.com");
            CHECK(repository->configured == false);
            CHECK(repository->remote_name == "origin");
            CHECK(repository->web_url() == "https://github.com/octo/hello");
        }
    }
    SECTION("gitlab.com과 하위 그룹")
    {
        const std::optional<hosting_repository> repository = resolve_hosting_repository("git@gitlab.com:group/sub/app.git", "upstream", none);
        REQUIRE(repository.has_value());
        CHECK(repository->provider == hosting_provider::gitlab);
        CHECK(repository->owner == "group/sub");
        CHECK(repository->name == "app");
        CHECK(repository->full_name() == "group/sub/app");
        CHECK(repository->api_base == "https://gitlab.com/api/v4");
        CHECK(hosting_change_request_abbreviation(repository->provider) == "MR");
    }
    SECTION("모르는 호스트는 unknown이다")
    {
        const std::optional<hosting_repository> repository = resolve_hosting_repository("https://git.example.com/team/app.git", "origin", none);
        REQUIRE(repository.has_value());
        CHECK(repository->provider == hosting_provider::unknown);
        CHECK(repository->supported() == false);
        CHECK(repository->api_base.empty());
    }
    SECTION("경로가 한 단계뿐이면 프로젝트가 아니다")
    {
        CHECK(resolve_hosting_repository("https://github.com/owner", "origin", none).has_value() == false);
    }
}

TEST_CASE("configured self-hosted servers come from the settings table", "[hosting]")
{
    const std::vector<hosting_host_setting> hosts {
        { "git.corp.example", hosting_provider::gitlab, "" },
        { "ghe.corp.example", hosting_provider::github, "https://ghe.corp.example/api/v3/" },
        { "127.0.0.1:8123", hosting_provider::github, "http://127.0.0.1:8123" },
    };
    SECTION("API 주소를 적지 않으면 종류의 기본 경로를 쓴다")
    {
        const std::optional<hosting_repository> repository = resolve_hosting_repository("git@git.corp.example:team/sub/app.git", "origin", hosts);
        REQUIRE(repository.has_value());
        CHECK(repository->provider == hosting_provider::gitlab);
        CHECK(repository->configured);
        CHECK(repository->api_base == "https://git.corp.example/api/v4");
        CHECK(repository->owner == "team/sub");
        CHECK(repository->credential_host == "git.corp.example");
    }
    SECTION("ssh 포트는 호스트 열쇠에 들지 않는다")
    {
        const std::optional<hosting_repository> repository = resolve_hosting_repository("ssh://git@git.corp.example:2222/team/app.git", "origin", hosts);
        REQUIRE(repository.has_value());
        CHECK(repository->host_key == "git.corp.example");
        CHECK(repository->credential_host == "git.corp.example");
        CHECK(repository->web_base == "https://git.corp.example");
    }
    SECTION("적은 API 주소가 우선이고 끝의 /는 뗀다")
    {
        const std::optional<hosting_repository> repository = resolve_hosting_repository("https://GHE.corp.example/o/r.git", "origin", hosts);
        REQUIRE(repository.has_value());
        CHECK(repository->provider == hosting_provider::github);
        CHECK(repository->api_base == "https://ghe.corp.example/api/v3");
    }
    SECTION("포트가 있는 http 서버는 host:port로 찾는다")
    {
        const std::optional<hosting_repository> repository = resolve_hosting_repository("http://127.0.0.1:8123/octo/hello.git", "origin", hosts);
        REQUIRE(repository.has_value());
        CHECK(repository->provider == hosting_provider::github);
        CHECK(repository->api_base == "http://127.0.0.1:8123");
        CHECK(repository->web_base == "http://127.0.0.1:8123");
        CHECK(repository->credential_host == "127.0.0.1:8123");
        CHECK(repository->credential_protocol == "http");
        // 다른 포트의 같은 호스트는 표에 없다.
        const std::optional<hosting_repository> other = resolve_hosting_repository("http://127.0.0.1:9999/octo/hello.git", "origin", hosts);
        REQUIRE(other.has_value());
        CHECK(other->provider == hosting_provider::unknown);
    }
    SECTION("표가 내장 호스트보다 먼저다")
    {
        const std::vector<hosting_host_setting> override_hosts { { "github.com", hosting_provider::gitlab, "https://github.com/api/v4" } };
        const std::optional<hosting_repository> repository = resolve_hosting_repository("https://github.com/o/r.git", "origin", override_hosts);
        REQUIRE(repository.has_value());
        CHECK(repository->provider == hosting_provider::gitlab);
    }
}

TEST_CASE("a host row written with the default port is the same host", "[hosting]")
{
    // 검사가 받는 `host:443`·`host:80` 표기다. 기본 포트로 보고 포트 없는 원격과 맞춘다. 열쇠는 그대로 호스트 이름이다.
    const std::vector<hosting_host_setting> hosts {
        { "git.example.com:443", hosting_provider::github, "" },
        { "intra.example:80", hosting_provider::gitlab, "http://intra.example/api/v4" },
    };
    for (const hosting_host_setting& setting : hosts)
        REQUIRE_FALSE(validate_hosting_host_setting(setting).has_value());

    const std::optional<hosting_repository> repository = resolve_hosting_repository("https://git.example.com/o/r.git", "origin", hosts);
    REQUIRE(repository.has_value());
    CHECK(repository->configured);
    CHECK(repository->provider == hosting_provider::github);
    CHECK(repository->api_base == "https://git.example.com/api/v3");
    CHECK(repository->host_key == "git.example.com");
    CHECK(repository->credential_host == "git.example.com");

    const std::optional<hosting_repository> intra = resolve_hosting_repository("http://intra.example/team/app.git", "origin", hosts);
    REQUIRE(intra.has_value());
    CHECK(intra->configured);
    CHECK(intra->provider == hosting_provider::gitlab);

    // 다른 포트의 원격은 기본 포트를 적은 줄에 맞지 않는다. 그 줄의 provider·API 설정을 빌려 쓰지 않는다.
    const std::optional<hosting_repository> other_port = resolve_hosting_repository("https://git.example.com:8443/o/r.git", "origin", hosts);
    REQUIRE(other_port.has_value());
    CHECK_FALSE(other_port->configured);
    CHECK(other_port->host_key == "git.example.com:8443");

    // 원격이 아직 없는 게시도 같은 줄로 풀린다.
    const std::optional<hosting_repository> context = hosting_repository_for_host("git.example.com:443", hosts);
    REQUIRE(context.has_value());
    CHECK(context->configured);
    CHECK(context->host_key == "git.example.com");
}

TEST_CASE("an IPv6 remote keeps its brackets in the web base and the port key", "[hosting]")
{
    const std::optional<hosting_repository> repository = resolve_hosting_repository("http://[::1]:8080/o/r.git", "origin", {});
    REQUIRE(repository.has_value());
    CHECK(repository->web_base == "http://[::1]:8080");
    CHECK(repository->host_key == "[::1]:8080");
    CHECK(repository->credential_host == "[::1]:8080");
    CHECK(repository->web_url() == "http://[::1]:8080/o/r");

    const std::optional<hosting_repository> ssh = resolve_hosting_repository("ssh://git@[::1]:2222/o/r.git", "origin", {});
    REQUIRE(ssh.has_value());
    CHECK(ssh->web_base == "https://[::1]");
    CHECK(url_host("::1") == "[::1]");
    CHECK(url_host("git.example.com") == "git.example.com");
}

TEST_CASE("the project web address encodes the decoded path again", "[hosting]")
{
    // 소유자·이름은 API가 쓰도록 해제한 채 두고, 브라우저 주소만 다시 인코딩한다.
    const std::optional<hosting_repository> repository = resolve_hosting_repository("https://github.com/o/my%20repo%2541.git", "origin", {});
    REQUIRE(repository.has_value());
    CHECK(repository->name == "my repo%41");
    CHECK(repository->web_url() == "https://github.com/o/my%20repo%2541");
}

TEST_CASE("one remote is picked for the integration", "[hosting]")
{
    const std::vector<hosting_host_setting> none {};
    SECTION("origin이 먼저다")
    {
        const std::optional<hosting_repository> repository = pick_hosting_repository({ { "fork", "git@github.com:me/app.git" }, { "origin", "git@gitlab.com:team/app.git" } }, none);
        REQUIRE(repository.has_value());
        CHECK(repository->remote_name == "origin");
        CHECK(repository->provider == hosting_provider::gitlab);
    }
    SECTION("origin을 모르면 서비스를 아는 첫 원격이다")
    {
        const std::optional<hosting_repository> repository = pick_hosting_repository({ { "origin", "https://git.example.com/a/b.git" }, { "gh", "https://github.com/a/b.git" } }, none);
        REQUIRE(repository.has_value());
        CHECK(repository->remote_name == "gh");
    }
    SECTION("아는 서비스가 없으면 unknown으로라도 돌려준다")
    {
        const std::optional<hosting_repository> repository = pick_hosting_repository({ { "origin", "https://git.example.com/a/b.git" } }, none);
        REQUIRE(repository.has_value());
        CHECK(repository->provider == hosting_provider::unknown);
    }
    SECTION("읽을 수 있는 원격이 없으면 값이 없다")
    {
        CHECK(pick_hosting_repository({ { "origin", "/srv/git/repo.git" } }, none).has_value() == false);
        CHECK(pick_hosting_repository({}, none).has_value() == false);
    }
}

TEST_CASE("a host setting row is validated", "[hosting]")
{
    CHECK(validate_hosting_host_setting({ "git.example.com", hosting_provider::github, "" }).has_value() == false);
    CHECK(validate_hosting_host_setting({ "127.0.0.1:8123", hosting_provider::gitlab, "http://127.0.0.1:8123/api/v4" }).has_value() == false);
    CHECK(validate_hosting_host_setting({ "", hosting_provider::github, "" }).has_value());
    CHECK(validate_hosting_host_setting({ "git example.com", hosting_provider::github, "" }).has_value());
    CHECK(validate_hosting_host_setting({ "https://git.example.com", hosting_provider::github, "" }).has_value());
    CHECK(validate_hosting_host_setting({ "host:99999", hosting_provider::github, "" }).has_value());
    CHECK(validate_hosting_host_setting({ "host", hosting_provider::github, "ftp://host/api" }).has_value());
    CHECK(validate_hosting_host_setting({ "host", hosting_provider::github, "https://" }).has_value());
    CHECK(validate_hosting_host_setting({ "host", hosting_provider::github, "git.example.com/api" }).has_value());
    CHECK(parse_hosting_provider("github") == hosting_provider::github);
    CHECK(parse_hosting_provider("gitlab") == hosting_provider::gitlab);
    CHECK(parse_hosting_provider("bitbucket").has_value() == false);
    CHECK(parse_hosting_provider("unknown").has_value() == false);
}

TEST_CASE("the hosting table is read from and written to the settings file", "[hosting][settings]")
{
    const std::u8string source = u8R"({
        "schema_version": 1,
        "hosting": { "hosts": [
            { "host": "Git.Corp.Example", "provider": "gitlab", "api_base": "" },
            { "host": "127.0.0.1:8123", "provider": "github", "api_base": "http://127.0.0.1:8123" },
            { "host": "bad host", "provider": "github" },
            { "host": "x.example", "provider": "bitbucket" },
            { "host": "git.corp.example", "provider": "github" },
            42
        ], "future_key": true }
    })";
    const app_settings_load_result loaded = parse_app_settings_json(source);
    REQUIRE(loaded.settings.hosting.hosts.size() == 2);
    CHECK(loaded.settings.hosting.hosts[0].host == "git.corp.example");
    CHECK(loaded.settings.hosting.hosts[0].provider == hosting_provider::gitlab);
    CHECK(loaded.settings.hosting.hosts[1].api_base == "http://127.0.0.1:8123");
    CHECK(loaded.diagnostics.empty() == false);

    // 쓰면 알 수 없는 키가 남고 표는 다시 읽힌다.
    const std::u8string written = serialize_app_settings_json(loaded.settings, loaded.shadow_source_json);
    const app_settings_load_result again = parse_app_settings_json(written);
    CHECK(again.settings.hosting == loaded.settings.hosting);
    CHECK(written.find(u8"future_key") != std::u8string::npos);

    // 표를 비우면 hosts가 지워진다.
    app_settings emptied { loaded.settings };
    emptied.hosting.hosts.clear();
    const std::u8string cleared = serialize_app_settings_json(emptied, loaded.shadow_source_json);
    CHECK(cleared.find(u8"\"hosts\"") == std::u8string::npos);
    CHECK(cleared.find(u8"future_key") != std::u8string::npos);

    // 표가 없는 파일은 hosting 키를 만들지 않는다.
    CHECK(serialize_app_settings_json(app_settings {}, u8"").find(u8"hosting") == std::u8string::npos);
}

TEST_CASE("the web link table is read from and written to the settings file next to the hosts", "[hosting][settings][web_link]")
{
    const std::u8string source = u8R"({
        "schema_version": 1,
        "hosting": { "web_links": [
            { "host": "SSH.Code.Example", "service": "gitea", "web_base": "https://code.example", "icon": "server" },
            { "host": "127.0.0.1:3000" },
            { "host": "bad host", "service": "gitea" },
            { "host": "x.example", "service": "svn" },
            { "host": "x2.example", "web_base": "ftp://x2.example" },
            { "host": "ssh.code.example", "service": "other" },
            "nope"
        ] }
    })";
    const app_settings_load_result loaded = parse_app_settings_json(source);
    REQUIRE(loaded.settings.hosting.web_links.size() == 2);
    CHECK(loaded.settings.hosting.web_links[0].host == "ssh.code.example");
    CHECK(loaded.settings.hosting.web_links[0].service == web_link_service::gitea);
    CHECK(loaded.settings.hosting.web_links[0].icon == "server");
    // 서비스를 적지 않으면 기타다.
    CHECK(loaded.settings.hosting.web_links[1].service == web_link_service::other);
    CHECK(loaded.diagnostics.empty() == false);

    const std::u8string written = serialize_app_settings_json(loaded.settings, loaded.shadow_source_json);
    CHECK(parse_app_settings_json(written).settings.hosting == loaded.settings.hosting);
    // 호스트 표가 없어도 웹 링크 표만으로 hosting을 쓰고, 둘 다 비우면 hosting을 지운다.
    CHECK(written.find(u8"\"hosts\"") == std::u8string::npos);
    app_settings emptied { loaded.settings };
    emptied.hosting.web_links.clear();
    CHECK(serialize_app_settings_json(emptied, loaded.shadow_source_json).find(u8"hosting") == std::u8string::npos);
}
