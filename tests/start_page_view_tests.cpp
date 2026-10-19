#include "presentation/start_page_view.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

using namespace cyella;

namespace {
    std::vector<std::u8string> names_of(const start_page_view& view)
    {
        std::vector<std::u8string> names {};
        for (const start_page_item& item : view.items)
            names.push_back(item.display_name);
        return names;
    }

    using name_list = std::vector<std::u8string>;

    const repository_exists_function everything_exists { [](std::u8string_view) { return true; } };

    app_settings sample_settings()
    {
        app_settings settings {};
        // 설정 목록 순서: 고정 둘, 그다음 비고정.
        settings.recent_repositories.push_back({ u8"C:\\work\\pin-b", u8"pin-b", u8"2026-10-01T00:00:00Z", true });
        settings.recent_repositories.push_back({ u8"C:\\work\\pin-a", u8"pin-a", u8"2026-10-05T00:00:00Z", true });
        settings.recent_repositories.push_back({ u8"C:\\work\\old", u8"old", u8"2026-10-02T00:00:00Z", false });
        settings.recent_repositories.push_back({ u8"D:\\Projects\\Newest", u8"Newest", u8"2026-10-20T00:00:00Z", false });
        settings.recent_repositories.push_back({ u8"C:\\work\\한글 저장소", u8"한글 저장소", u8"2026-10-10T00:00:00Z", false });
        return settings;
    }
} // namespace

TEST_CASE("start page lists pinned first then recent by opened time", "[start_page]")
{
    const start_page_view view { build_start_page_view(sample_settings(), everything_exists) };

    // 고정은 설정 목록의 순서를 따르고, 나머지는 시각 내림차순이다.
    CHECK(names_of(view) == name_list { u8"pin-b", u8"pin-a", u8"Newest", u8"한글 저장소", u8"old" });
    CHECK(view.total_count == 5);
    CHECK(view.items[0].pinned);
    CHECK(view.items[1].pinned);
    CHECK_FALSE(view.items[2].pinned);
}

TEST_CASE("equal or missing timestamps keep the settings order", "[start_page]")
{
    app_settings settings {};
    settings.recent_repositories.push_back({ u8"C:\\a", u8"a", u8"", false });
    settings.recent_repositories.push_back({ u8"C:\\b", u8"b", u8"", false });
    settings.recent_repositories.push_back({ u8"C:\\c", u8"c", u8"2026-10-20T00:00:00Z", false });

    CHECK(names_of(build_start_page_view(settings, everything_exists)) == name_list { u8"c", u8"a", u8"b" });
}

TEST_CASE("items carry name parent folder and full path", "[start_page]")
{
    const start_page_view view { build_start_page_view(sample_settings(), everything_exists) };

    const start_page_item& newest { view.items[2] };
    CHECK(newest.display_name == u8"Newest");
    CHECK(newest.parent_folder == u8"D:\\Projects");
    CHECK(newest.path == u8"D:\\Projects\\Newest");
    CHECK(newest.opened_at == u8"2026-10-20T00:00:00Z");
    CHECK(view.items[3].parent_folder == u8"C:\\work");
}

TEST_CASE("a missing display name is derived from the path", "[start_page]")
{
    app_settings settings {};
    settings.recent_repositories.push_back({ u8"C:\\work\\derived", u8"", u8"", false });
    CHECK(build_start_page_view(settings, everything_exists).items[0].display_name == u8"derived");
}

TEST_CASE("missing paths are marked unavailable through the injected check", "[start_page]")
{
    const std::set<std::u8string> present { u8"C:\\work\\pin-a", u8"D:\\Projects\\Newest" };
    const start_page_view view { build_start_page_view(sample_settings(), [&present](const std::u8string_view path) { return present.contains(std::u8string { path }); }) };

    for (const start_page_item& item : view.items)
        CHECK(item.exists == present.contains(item.path));
    // 없는 경로도 목록에서 사라지지 않는다. 지우는 것은 사용자의 몫이다.
    CHECK(view.items.size() == 5);
}

TEST_CASE("a null existence check treats every path as available", "[start_page]")
{
    const start_page_view view { build_start_page_view(sample_settings(), repository_exists_function {}) };
    for (const start_page_item& item : view.items)
        CHECK(item.exists);
}

TEST_CASE("the existence check runs only for items that survive the filter", "[start_page]")
{
    std::vector<std::u8string> asked {};
    const start_page_view view {
        build_start_page_view(sample_settings(), [&asked](const std::u8string_view path) {
            asked.emplace_back(path);
            return true;
        }, u8"newest"),
    };

    CHECK(names_of(view) == name_list { u8"Newest" });
    CHECK(asked == name_list { u8"D:\\Projects\\Newest" });
    CHECK(view.total_count == 5);
}

TEST_CASE("search matches name or path ignoring ASCII case", "[start_page]")
{
    const app_settings settings { sample_settings() };

    CHECK(names_of(build_start_page_view(settings, everything_exists, u8"PIN")) == name_list { u8"pin-b", u8"pin-a" });
    // 경로의 폴더 부분으로도 찾는다.
    CHECK(names_of(build_start_page_view(settings, everything_exists, u8"projects")) == name_list { u8"Newest" });
    CHECK(names_of(build_start_page_view(settings, everything_exists, u8"c:\\WORK\\ol")) == name_list { u8"old" });
    // 슬래시로 쳐도 경로의 역슬래시와 맞는다.
    CHECK(names_of(build_start_page_view(settings, everything_exists, u8"c:/work/old")) == name_list { u8"old" });
    CHECK(build_start_page_view(settings, everything_exists, u8"zzz").items.empty());
}

TEST_CASE("search handles Korean text", "[start_page]")
{
    const app_settings settings { sample_settings() };

    CHECK(names_of(build_start_page_view(settings, everything_exists, u8"한글")) == name_list { u8"한글 저장소" });
    CHECK(names_of(build_start_page_view(settings, everything_exists, u8"저장")) == name_list { u8"한글 저장소" });
    CHECK(names_of(build_start_page_view(settings, everything_exists, u8"글 저")) == name_list { u8"한글 저장소" });
    CHECK(build_start_page_view(settings, everything_exists, u8"한국").items.empty());

    // 글자 하나의 UTF-8 바이트 일부만으로는 맞지 않는다 (유효한 UTF-8 검색어끼리의 비교다).
    CHECK(matches_search_text(u8"가나다", u8"나"));
    CHECK_FALSE(matches_search_text(u8"가나다", u8"라"));
}

TEST_CASE("blank search matches everything and surrounding spaces are ignored", "[start_page]")
{
    const app_settings settings { sample_settings() };
    CHECK(build_start_page_view(settings, everything_exists, u8"").items.size() == 5);
    CHECK(build_start_page_view(settings, everything_exists, u8"   ").items.size() == 5);
    CHECK(names_of(build_start_page_view(settings, everything_exists, u8"  old \t")) == name_list { u8"old" });
}

TEST_CASE("an empty settings gives an empty view", "[start_page]")
{
    const start_page_view view { build_start_page_view(app_settings {}, everything_exists, u8"x") };
    CHECK(view.items.empty());
    CHECK(view.total_count == 0);
}
