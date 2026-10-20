#include "domain/app_settings.h"
#include "domain/repository_path.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <vector>

using namespace cyella;

namespace {
    std::vector<std::u8string> paths_of(const app_settings& settings)
    {
        std::vector<std::u8string> paths {};
        for (const recent_repository& value : settings.recent_repositories)
            paths.push_back(value.path);
        return paths;
    }

    // 숫자로 만든 서로 다른 저장소 경로다.
    std::u8string repository_number(const int number)
    {
        const std::string text { std::to_string(number) };
        std::u8string path { u8"C:\\work\\repo" };
        path.append(text.begin(), text.end());
        return path;
    }
} // namespace

TEST_CASE("repository_path normalizes separators and trailing separators", "[repository_path]")
{
    CHECK(normalize_repository_path(u8"C:/work/repo/") == u8"C:\\work\\repo");
    CHECK(normalize_repository_path(u8"C:\\work\\repo\\\\") == u8"C:\\work\\repo");
    // 뿌리는 끝 구분자를 지키지 않으면 상대 경로가 된다.
    CHECK(normalize_repository_path(u8"C:\\") == u8"C:\\");
    CHECK(normalize_repository_path(u8"") == u8"");
}

TEST_CASE("repository_path_key ignores ASCII case and separator style only", "[repository_path]")
{
    CHECK(repository_path_key(u8"C:\\Work\\Repo") == repository_path_key(u8"c:/work/repo/"));
    CHECK(repository_path_key(u8"C:\\work\\repo") != repository_path_key(u8"C:\\work\\repo2"));
    // 한글은 접지 않고 그대로 비교한다.
    CHECK(repository_path_key(u8"C:\\작업\\저장소") == repository_path_key(u8"c:/작업/저장소"));
    CHECK(repository_path_key(u8"C:\\작업\\저장소") != repository_path_key(u8"C:\\작업\\다른"));
}

TEST_CASE("repository display name and parent folder", "[repository_path]")
{
    CHECK(repository_display_name(u8"C:\\work\\my.repo") == u8"my.repo");
    CHECK(repository_display_name(u8"C:\\work\\repo\\") == u8"repo");
    CHECK(repository_display_name(u8"C:\\work\\한글 저장소") == u8"한글 저장소");
    CHECK(repository_display_name(u8"C:\\") == u8"C:\\");
    CHECK(repository_display_name(u8"repo") == u8"repo");

    CHECK(repository_parent_folder(u8"C:\\work\\repo") == u8"C:\\work");
    CHECK(repository_parent_folder(u8"C:\\repo") == u8"C:\\");
    CHECK(repository_parent_folder(u8"C:\\") == u8"");
    CHECK(repository_parent_folder(u8"repo") == u8"");
    CHECK(repository_parent_folder(u8"\\\\server\\share\\repo") == u8"\\\\server\\share");
}

TEST_CASE("touch puts a repository at the front and fills display name", "[app_settings]")
{
    app_settings settings {};
    touch_recent_repository(settings, u8"C:/work/alpha/", u8"2026-10-20T01:00:00Z");
    touch_recent_repository(settings, u8"C:\\work\\beta", u8"2026-10-20T02:00:00Z");

    REQUIRE(settings.recent_repositories.size() == 2);
    CHECK(settings.recent_repositories[0].path == u8"C:\\work\\beta");
    CHECK(settings.recent_repositories[0].display_name == u8"beta");
    CHECK(settings.recent_repositories[0].opened_at == u8"2026-10-20T02:00:00Z");
    CHECK(settings.recent_repositories[1].path == u8"C:\\work\\alpha");
    CHECK_FALSE(settings.recent_repositories[1].pinned);
}

TEST_CASE("touch of a known repository moves it to the front without duplicating", "[app_settings]")
{
    app_settings settings {};
    touch_recent_repository(settings, u8"C:\\work\\alpha", u8"2026-10-20T01:00:00Z");
    touch_recent_repository(settings, u8"C:\\work\\beta", u8"2026-10-20T02:00:00Z");
    // 대소문자와 구분자가 달라도 같은 저장소다. 새 표기와 시각으로 바뀐다.
    touch_recent_repository(settings, u8"c:/WORK/Alpha/", u8"2026-10-20T03:00:00Z");

    REQUIRE(settings.recent_repositories.size() == 2);
    CHECK(settings.recent_repositories[0].path == u8"c:\\WORK\\Alpha");
    CHECK(settings.recent_repositories[0].opened_at == u8"2026-10-20T03:00:00Z");
    CHECK(settings.recent_repositories[1].path == u8"C:\\work\\beta");
}

TEST_CASE("touch ignores an empty path", "[app_settings]")
{
    app_settings settings {};
    touch_recent_repository(settings, u8"", u8"2026-10-20T01:00:00Z");
    CHECK(settings.recent_repositories.empty());
}

TEST_CASE("unpinned entries are capped at twenty and the oldest is dropped", "[app_settings]")
{
    app_settings settings {};
    for (int number = 0; number < 25; ++number)
        touch_recent_repository(settings, repository_number(number), u8"2026-10-20T00:00:00Z");

    REQUIRE(settings.recent_repositories.size() == recent_repository_capacity);
    CHECK(settings.recent_repositories.front().path == repository_number(24));
    CHECK(settings.recent_repositories.back().path == repository_number(5));
}

TEST_CASE("pinned entries are never evicted and always come first", "[app_settings]")
{
    app_settings settings {};
    touch_recent_repository(settings, repository_number(0), u8"");
    REQUIRE(set_recent_repository_pinned(settings, repository_number(0), true));

    // 고정 하나 + 비고정을 상한보다 많이 넣는다. 고정은 상한에 들지 않고 남는다.
    for (int number = 1; number <= 30; ++number)
        touch_recent_repository(settings, repository_number(number), u8"");

    REQUIRE(settings.recent_repositories.size() == recent_repository_capacity + 1);
    CHECK(settings.recent_repositories.front().path == repository_number(0));
    CHECK(settings.recent_repositories.front().pinned);
    CHECK(settings.recent_repositories[1].path == repository_number(30));

    SECTION("고정을 여러 개 걸어도 모두 남는다")
    {
        for (int number = 11; number <= 30; ++number)
            REQUIRE(set_recent_repository_pinned(settings, repository_number(number), true));
        for (int number = 31; number <= 60; ++number)
            touch_recent_repository(settings, repository_number(number), u8"");

        std::size_t pinned { 0 };
        for (const recent_repository& value : settings.recent_repositories)
            if (value.pinned)
                ++pinned;
        CHECK(pinned == 21);
        CHECK(settings.recent_repositories.size() == pinned + recent_repository_capacity);
        for (std::size_t index = 0; index < pinned; ++index)
            CHECK(settings.recent_repositories[index].pinned);
    }
}

TEST_CASE("touching a pinned repository keeps it pinned", "[app_settings]")
{
    app_settings settings {};
    touch_recent_repository(settings, u8"C:\\work\\alpha", u8"");
    touch_recent_repository(settings, u8"C:\\work\\beta", u8"");
    REQUIRE(set_recent_repository_pinned(settings, u8"C:\\work\\alpha", true));

    touch_recent_repository(settings, u8"C:\\work\\alpha", u8"2026-10-20T05:00:00Z");

    REQUIRE(settings.recent_repositories.size() == 2);
    CHECK(settings.recent_repositories[0].path == u8"C:\\work\\alpha");
    CHECK(settings.recent_repositories[0].pinned);
    CHECK(settings.recent_repositories[0].opened_at == u8"2026-10-20T05:00:00Z");
}

TEST_CASE("pinning moves to the front of the pinned group and unpinning to the front of recent", "[app_settings]")
{
    app_settings settings {};
    for (const char8_t* name : { u8"a", u8"b", u8"c" })
        touch_recent_repository(settings, std::u8string { u8"C:\\work\\" } + name, u8"");
    // 목록: c b a
    REQUIRE(set_recent_repository_pinned(settings, u8"C:\\work\\a", true));
    REQUIRE(set_recent_repository_pinned(settings, u8"C:\\work\\c", true));
    // 마지막에 고정한 c가 고정 무리 맨 앞이다.
    CHECK(paths_of(settings) == std::vector<std::u8string> { u8"C:\\work\\c", u8"C:\\work\\a", u8"C:\\work\\b" });

    REQUIRE(set_recent_repository_pinned(settings, u8"C:\\work\\a", false));
    // 고정을 풀면 비고정 무리의 맨 앞이다.
    CHECK(paths_of(settings) == std::vector<std::u8string> { u8"C:\\work\\c", u8"C:\\work\\a", u8"C:\\work\\b" });
    CHECK_FALSE(settings.recent_repositories[1].pinned);
}

TEST_CASE("pin returns false when nothing changes", "[app_settings]")
{
    app_settings settings {};
    touch_recent_repository(settings, u8"C:\\work\\alpha", u8"");

    CHECK_FALSE(set_recent_repository_pinned(settings, u8"C:\\work\\alpha", false));
    CHECK_FALSE(set_recent_repository_pinned(settings, u8"C:\\work\\missing", true));
    CHECK(set_recent_repository_pinned(settings, u8"c:/WORK/alpha", true));
    CHECK_FALSE(set_recent_repository_pinned(settings, u8"C:\\work\\alpha", true));
}

TEST_CASE("remove drops a repository by normalized key", "[app_settings]")
{
    app_settings settings {};
    touch_recent_repository(settings, u8"C:\\work\\alpha", u8"");
    touch_recent_repository(settings, u8"C:\\work\\beta", u8"");

    CHECK(remove_recent_repository(settings, u8"c:/work/ALPHA/"));
    CHECK_FALSE(remove_recent_repository(settings, u8"C:\\work\\alpha"));
    REQUIRE(settings.recent_repositories.size() == 1);
    CHECK(settings.recent_repositories[0].path == u8"C:\\work\\beta");
}

TEST_CASE("normalize repairs a hand edited list", "[app_settings]")
{
    std::vector<recent_repository> repositories {};
    repositories.push_back({ u8"C:\\a", u8"a", u8"", false });
    repositories.push_back({ u8"C:\\p1", u8"p1", u8"", true });
    repositories.push_back({ u8"c:\\A", u8"A", u8"", true });
    repositories.push_back({ u8"C:\\p2", u8"p2", u8"", true });
    for (int number = 0; number < 25; ++number)
        repositories.push_back({ repository_number(number), u8"", u8"", false });

    normalize_recent_repositories(repositories);

    // 같은 저장소의 중복은 먼저 나온 것만 남는다 (a는 비고정으로 먼저 나왔다).
    CHECK(repositories[0].path == u8"C:\\p1");
    CHECK(repositories[1].path == u8"C:\\p2");
    CHECK(repositories[0].pinned);
    CHECK(repositories[1].pinned);
    CHECK(repositories.size() == 2 + recent_repository_capacity);
    CHECK(repositories[2].path == u8"C:\\a");
}

TEST_CASE("theme preference names round trip", "[app_settings]")
{
    for (const theme_preference theme : { theme_preference::system, theme_preference::light, theme_preference::dark })
        CHECK(parse_theme_preference(theme_preference_name(theme)) == theme);
    CHECK_FALSE(parse_theme_preference(u8"Dark").has_value());
    CHECK_FALSE(parse_theme_preference(u8"").has_value());
}

TEST_CASE("window placement is valid only with a positive size", "[app_settings]")
{
    window_placement placement {};
    CHECK_FALSE(placement.valid());
    placement.width = 800;
    placement.height = 600;
    CHECK(placement.valid());
    placement.x = -1900;
    CHECK(placement.valid());
}

TEST_CASE("timestamp is ISO 8601 UTC", "[app_settings]")
{
    const std::chrono::system_clock::time_point epoch_plus { std::chrono::seconds { 1760952612 } };
    CHECK(format_utc_timestamp(epoch_plus) == u8"2025-10-20T09:30:12Z");
}
TEST_CASE("relocating a recent repository keeps its place, pin and time and renames it", "[settings][recent]")
{
    app_settings settings {};
    settings.recent_repositories.push_back({ u8"C:\\work\\pinned", u8"pinned", u8"2026-10-20T09:00:00Z", true });
    settings.recent_repositories.push_back({ u8"C:\\work\\moved", u8"moved", u8"2026-10-19T09:00:00Z", false });
    settings.recent_repositories.push_back({ u8"C:\\work\\last", u8"last", u8"2026-10-18T09:00:00Z", false });

    CHECK(relocate_recent_repository(settings, u8"c:/WORK/moved/", u8"D:\\archive\\renamed\\"));
    REQUIRE(settings.recent_repositories.size() == 3);
    CHECK(paths_of(settings) == std::vector<std::u8string> { u8"C:\\work\\pinned", u8"D:\\archive\\renamed", u8"C:\\work\\last" });
    const recent_repository& entry { settings.recent_repositories[1] };
    CHECK(entry.display_name == u8"renamed");
    CHECK(entry.opened_at == u8"2026-10-19T09:00:00Z");
    CHECK_FALSE(entry.pinned);
    CHECK(settings.recent_repositories[0].pinned);
}

TEST_CASE("relocating onto a path another entry already has drops the other entry", "[settings][recent]")
{
    app_settings settings {};
    settings.recent_repositories.push_back({ u8"C:\\work\\a", u8"a", u8"2026-10-20T09:00:00Z", false });
    settings.recent_repositories.push_back({ u8"C:\\work\\missing", u8"missing", u8"2026-10-19T09:00:00Z", true });
    settings.recent_repositories.push_back({ u8"C:\\work\\b", u8"b", u8"2026-10-18T09:00:00Z", false });

    CHECK(relocate_recent_repository(settings, u8"C:\\work\\missing", u8"c:\\work\\A"));
    // 같은 저장소가 둘이 되지 않는다. 옮긴 항목(고정)이 남고 이미 있던 항목이 사라진다.
    REQUIRE(settings.recent_repositories.size() == 2);
    CHECK(settings.recent_repositories[0].path == u8"c:\\work\\A");
    CHECK(settings.recent_repositories[0].pinned);
    CHECK(settings.recent_repositories[1].path == u8"C:\\work\\b");
}

TEST_CASE("relocating an unknown entry or to an empty path changes nothing", "[settings][recent]")
{
    app_settings settings {};
    settings.recent_repositories.push_back({ u8"C:\\work\\a", u8"a", u8"", false });
    const app_settings before { settings };
    CHECK_FALSE(relocate_recent_repository(settings, u8"C:\\work\\unknown", u8"D:\\x"));
    CHECK_FALSE(relocate_recent_repository(settings, u8"C:\\work\\a", u8""));
    CHECK(settings == before);
}
