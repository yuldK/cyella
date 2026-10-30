#include "domain/ssh_remote.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace cyella;

TEST_CASE("ssh remote URLs keep the server path the way git hands it to receive-pack", "[ssh][remote]")
{
    struct sample
    {
        const char* url;
        const char* user;
        const char* host;
        int port;
        const char* path;
    };
    const sample samples[] {
        // scp 꼴은 로그인 홈 기준이고, `/`로 시작하면 절대 경로다.
        { "me@git.example.org:repos/app.git", "me", "git.example.org", 0, "repos/app.git" },
        { "git.example.org:app.git", "", "git.example.org", 0, "app.git" },
        { "me@Git.Example.org:/srv/git/app.git", "me", "git.example.org", 0, "/srv/git/app.git" },
        { "  me@host:~/app.git  ", "me", "host", 0, "~/app.git" },
        { "me@host:~git/app.git", "me", "host", 0, "~git/app.git" },
        // ssh:// 꼴의 경로는 앞 `/`까지가 경로다. `/~`면 git처럼 앞 `/`를 뗀다.
        { "ssh://me@host/srv/git/app.git", "me", "host", 0, "/srv/git/app.git" },
        { "ssh://me@host:2222/srv/git/app.git", "me", "host", 2222, "/srv/git/app.git" },
        { "ssh://host/~/app.git", "", "host", 0, "~/app.git" },
        { "ssh://host/~me/app.git", "", "host", 0, "~me/app.git" },
        { "git+ssh://host/app", "", "host", 0, "/app" },
        { "ssh://me@[fe80::1]:22/srv/app.git", "me", "fe80::1", 22, "/srv/app.git" },
        // 경로 글자는 따옴표 안에 들어가므로 공백·따옴표도 받는다.
        { "me@host:my repo's.git", "me", "host", 0, "my repo's.git" },
    };
    for (const sample& entry : samples)
    {
        CAPTURE(entry.url);
        const std::optional<ssh_remote_target> target { parse_ssh_remote_target(entry.url) };
        REQUIRE(target.has_value());
        CHECK(target->user == entry.user);
        CHECK(target->host == entry.host);
        CHECK(target->port.value_or(0) == entry.port);
        CHECK(target->path == entry.path);
    }
    CHECK(parse_ssh_remote_target("me@host:app.git")->destination() == "me@host");
    CHECK(parse_ssh_remote_target("host:app.git")->destination() == "host");
}

TEST_CASE("non-ssh URLs and values that would become ssh options or shell words are not ssh targets", "[ssh][remote]")
{
    for (const char* const url : {
             "",
             "https://git.example.org/repos/app.git",
             "git://git.example.org/app.git",
             "C:\\work\\app.git",
             "C:/work/app.git",
             "/srv/git/app.git",
             "../app.git",
             "-oProxyCommand=calc:app.git",
             "me@-oProxyCommand=calc:app.git",
             "-l root@host:app.git",
             "me;rm@host:app.git",
             "host name:app.git",
             "me@host:-bad.git",
             "me@host:",
             "me@host:~",
             "me@host:~/",
             "me@host:~bad;user/app.git",
             "me@host:app\n.git",
             "ssh://host",
             "ssh://host/",
             "ssh://host:99999/app.git",
             "ssh://host:x/app.git",
             "ssh://[fe80::1/app.git",
         })
    {
        CAPTURE(url);
        CHECK_FALSE(parse_ssh_remote_target(url).has_value());
    }
}
