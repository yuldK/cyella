#include "application/repository_tabs.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

using namespace cyella;

namespace {
    // 탭 줄을 한눈에 비교하려는 요약이다. 저장소 탭은 표시 이름, 시작 탭은 `*`다.
    std::vector<std::u8string> labels(const repository_tabs& tabs)
    {
        std::vector<std::u8string> result {};
        for (const repository_tab& tab : tabs.tabs())
            result.push_back(tab.kind == repository_tab_kind::start ? std::u8string { u8"*" } : tab.display_name);
        return result;
    }

    using label_list = std::vector<std::u8string>;

    std::u8string repo(const char8_t* const name)
    {
        return std::u8string { u8"C:\\work\\" } + name;
    }

    // 저장소 탭 a b c를 열고 b를 선택한 모양이다.
    repository_tabs three_tabs()
    {
        repository_tabs tabs {};
        static_cast<void>(tabs.open_repository(repo(u8"a")));
        static_cast<void>(tabs.open_repository(repo(u8"b")));
        static_cast<void>(tabs.open_repository(repo(u8"c")));
        REQUIRE(labels(tabs) == label_list { u8"a", u8"b", u8"c" });
        REQUIRE(tabs.select(1));
        return tabs;
    }
} // namespace

TEST_CASE("tabs start with one selected start tab", "[repository_tabs]")
{
    const repository_tabs tabs {};
    CHECK(labels(tabs) == label_list { u8"*" });
    CHECK(tabs.selected_index() == 0);
    CHECK(tabs.selected().kind == repository_tab_kind::start);
    CHECK(tabs.selected().id != 0);
}

TEST_CASE("opening a repository replaces the selected start tab in place", "[repository_tabs]")
{
    repository_tabs tabs {};
    const repository_tab_id start_id { tabs.selected().id };

    const open_repository_result result { tabs.open_repository(u8"C:/work/alpha/") };

    CHECK(labels(tabs) == label_list { u8"alpha" });
    CHECK(result.created);
    CHECK(result.replaced_start_tab);
    CHECK(result.id == tabs.selected().id);
    // 시작 탭에 붙어 있던 화면 상태가 저장소 탭으로 새지 않도록 id가 바뀐다.
    CHECK(result.id != start_id);
    CHECK(tabs.selected().path == u8"C:\\work\\alpha");
    CHECK(tabs.selected().kind == repository_tab_kind::repository);
}

TEST_CASE("opening from a repository tab inserts right after the selected tab", "[repository_tabs]")
{
    repository_tabs tabs { three_tabs() };
    REQUIRE(tabs.selected_index() == 1);

    const open_repository_result result { tabs.open_repository(repo(u8"d")) };

    CHECK(labels(tabs) == label_list { u8"a", u8"b", u8"d", u8"c" });
    CHECK(tabs.selected_index() == 2);
    CHECK(result.created);
    CHECK_FALSE(result.replaced_start_tab);
    CHECK(result.id == tabs.selected().id);
}

TEST_CASE("opening an open repository selects it without adding a tab", "[repository_tabs]")
{
    repository_tabs tabs { three_tabs() };
    const repository_tab_id first_id { tabs.tabs()[0].id };

    // 대소문자와 구분자가 달라도 같은 저장소다.
    const open_repository_result result { tabs.open_repository(u8"c:/WORK/A/") };

    CHECK(labels(tabs) == label_list { u8"a", u8"b", u8"c" });
    CHECK(tabs.selected_index() == 0);
    CHECK_FALSE(result.created);
    CHECK_FALSE(result.replaced_start_tab);
    CHECK(result.id == first_id);
}

TEST_CASE("opening an open repository keeps a selected start tab", "[repository_tabs]")
{
    repository_tabs tabs { three_tabs() };
    static_cast<void>(tabs.new_tab());
    REQUIRE(tabs.selected().kind == repository_tab_kind::start);

    static_cast<void>(tabs.open_repository(repo(u8"a")));

    // 이미 열린 저장소는 시작 탭을 쓰지 않고 그 탭으로 간다. 시작 탭은 남는다.
    CHECK(labels(tabs) == label_list { u8"a", u8"b", u8"c", u8"*" });
    CHECK(tabs.selected_index() == 0);
}

TEST_CASE("opening an empty path does nothing", "[repository_tabs]")
{
    repository_tabs tabs { three_tabs() };
    const open_repository_result result { tabs.open_repository(u8"") };
    CHECK_FALSE(result.created);
    CHECK(result.id == tabs.selected().id);
    CHECK(labels(tabs) == label_list { u8"a", u8"b", u8"c" });
}

TEST_CASE("new tab adds a start tab at the end next to the add button", "[repository_tabs]")
{
    // 가운데 탭(b)이 선택된 채로 눌러도 맨 뒤에 선다.
    repository_tabs tabs { three_tabs() };

    const repository_tab_id id { tabs.new_tab() };

    CHECK(labels(tabs) == label_list { u8"a", u8"b", u8"c", u8"*" });
    CHECK(tabs.selected_index() == 3);
    CHECK(tabs.selected().id == id);

    // 시작 탭이 선택된 채 "+"를 또 누르면 시작 탭이 하나 더 생긴다.
    static_cast<void>(tabs.new_tab());
    CHECK(labels(tabs) == label_list { u8"a", u8"b", u8"c", u8"*", u8"*" });
    CHECK(tabs.selected_index() == 4);
}

TEST_CASE("select validates the index", "[repository_tabs]")
{
    repository_tabs tabs { three_tabs() };
    CHECK(tabs.select(2));
    CHECK(tabs.selected_index() == 2);
    CHECK_FALSE(tabs.select(2));
    CHECK_FALSE(tabs.select(3));
    CHECK(tabs.selected_index() == 2);
}

TEST_CASE("closing the selected tab selects the right neighbour", "[repository_tabs]")
{
    repository_tabs tabs { three_tabs() };
    const repository_tab_id c_id { tabs.tabs()[2].id };

    REQUIRE(tabs.close(1));

    CHECK(labels(tabs) == label_list { u8"a", u8"c" });
    CHECK(tabs.selected_index() == 1);
    CHECK(tabs.selected().id == c_id);
}

TEST_CASE("closing the selected last tab selects the left neighbour", "[repository_tabs]")
{
    repository_tabs tabs { three_tabs() };
    REQUIRE(tabs.select(2));
    const repository_tab_id b_id { tabs.tabs()[1].id };

    REQUIRE(tabs.close(2));

    CHECK(labels(tabs) == label_list { u8"a", u8"b" });
    CHECK(tabs.selected_index() == 1);
    CHECK(tabs.selected().id == b_id);
}

TEST_CASE("closing the selected first tab selects the new first tab", "[repository_tabs]")
{
    repository_tabs tabs { three_tabs() };
    REQUIRE(tabs.select(0));
    const repository_tab_id b_id { tabs.tabs()[1].id };

    REQUIRE(tabs.close(0));

    CHECK(labels(tabs) == label_list { u8"b", u8"c" });
    CHECK(tabs.selected_index() == 0);
    CHECK(tabs.selected().id == b_id);
}

TEST_CASE("closing another tab keeps the same tab selected", "[repository_tabs]")
{
    repository_tabs tabs { three_tabs() };
    const repository_tab_id b_id { tabs.selected().id };

    SECTION("왼쪽 탭을 닫으면 인덱스가 당겨진다")
    {
        REQUIRE(tabs.close(0));
        CHECK(labels(tabs) == label_list { u8"b", u8"c" });
        CHECK(tabs.selected_index() == 0);
        CHECK(tabs.selected().id == b_id);
    }

    SECTION("오른쪽 탭을 닫으면 인덱스가 그대로다")
    {
        REQUIRE(tabs.close(2));
        CHECK(labels(tabs) == label_list { u8"a", u8"b" });
        CHECK(tabs.selected_index() == 1);
        CHECK(tabs.selected().id == b_id);
    }
}

TEST_CASE("closing the last tab leaves a start tab", "[repository_tabs]")
{
    repository_tabs tabs {};
    static_cast<void>(tabs.open_repository(repo(u8"a")));
    const repository_tab_id old_id { tabs.selected().id };

    REQUIRE(tabs.close(0));

    CHECK(labels(tabs) == label_list { u8"*" });
    CHECK(tabs.selected_index() == 0);
    CHECK(tabs.selected().id != old_id);

    // 시작 탭 하나뿐일 때는 바꿀 것이 없다.
    const repository_tab_id start_id { tabs.selected().id };
    CHECK_FALSE(tabs.close(0));
    CHECK(tabs.selected().id == start_id);
    CHECK(tabs.tabs().size() == 1);
}

TEST_CASE("close rejects an out of range index", "[repository_tabs]")
{
    repository_tabs tabs { three_tabs() };
    CHECK_FALSE(tabs.close(3));
    CHECK(tabs.tabs().size() == 3);
}

TEST_CASE("reorder moves a tab and selection follows the selected tab", "[repository_tabs]")
{
    SECTION("선택된 탭을 옮긴다")
    {
        repository_tabs tabs { three_tabs() };
        const repository_tab_id b_id { tabs.selected().id };
        REQUIRE(tabs.reorder(1, 2));
        CHECK(labels(tabs) == label_list { u8"a", u8"c", u8"b" });
        CHECK(tabs.selected_index() == 2);
        CHECK(tabs.selected().id == b_id);
    }

    SECTION("다른 탭이 선택 위치를 지나가도 선택은 같은 탭이다")
    {
        repository_tabs tabs { three_tabs() };
        const repository_tab_id b_id { tabs.selected().id };
        REQUIRE(tabs.reorder(0, 2));
        CHECK(labels(tabs) == label_list { u8"b", u8"c", u8"a" });
        CHECK(tabs.selected_index() == 0);
        CHECK(tabs.selected().id == b_id);

        REQUIRE(tabs.reorder(2, 0));
        CHECK(labels(tabs) == label_list { u8"a", u8"b", u8"c" });
        CHECK(tabs.selected_index() == 1);
        CHECK(tabs.selected().id == b_id);
    }

    SECTION("범위 밖이거나 제자리면 바뀌지 않는다")
    {
        repository_tabs tabs { three_tabs() };
        CHECK_FALSE(tabs.reorder(1, 1));
        CHECK_FALSE(tabs.reorder(3, 0));
        CHECK_FALSE(tabs.reorder(0, 3));
        CHECK(labels(tabs) == label_list { u8"a", u8"b", u8"c" });
    }
}

TEST_CASE("insertion index places a tab before or after the target", "[repository_tabs]")
{
    // 탭 a b c d에서 b(1)를 끈다.
    SECTION("뒤쪽 탭의 앞뒤")
    {
        CHECK(insertion_index(1, 2, true) == 2u);
        CHECK(insertion_index(1, 3, false) == 2u);
        CHECK(insertion_index(1, 3, true) == 3u);
    }

    SECTION("앞쪽 탭의 앞뒤")
    {
        CHECK(insertion_index(1, 0, false) == 0u);
        CHECK(insertion_index(2, 0, true) == 1u);
    }

    SECTION("제자리면 없다")
    {
        CHECK_FALSE(insertion_index(1, 0, true).has_value());
        CHECK_FALSE(insertion_index(1, 2, false).has_value());
        CHECK_FALSE(insertion_index(1, 1, false).has_value());
        CHECK_FALSE(insertion_index(1, 1, true).has_value());
    }

    SECTION("구한 인덱스로 옮기면 그 자리에 선다")
    {
        repository_tabs tabs { three_tabs() };
        const std::optional<std::size_t> to { insertion_index(0, 2, false) };
        REQUIRE(to.has_value());
        REQUIRE(tabs.reorder(0, *to));
        CHECK(labels(tabs) == label_list { u8"b", u8"a", u8"c" });
    }
}

TEST_CASE("tab ids are unique and never reused", "[repository_tabs]")
{
    repository_tabs tabs {};
    std::set<repository_tab_id> seen { tabs.selected().id };

    for (int round = 0; round < 5; ++round)
    {
        const repository_tab_id opened { tabs.open_repository(repo(u8"a")).id };
        CHECK(seen.insert(opened).second);
        const repository_tab_id fresh { tabs.new_tab() };
        CHECK(seen.insert(fresh).second);
        REQUIRE(tabs.close(tabs.selected_index()));
        REQUIRE(tabs.close(0));
    }

    for (const repository_tab& tab : tabs.tabs())
        CHECK(tab.id != 0);
}

TEST_CASE("index_of and find_repository", "[repository_tabs]")
{
    const repository_tabs tabs { three_tabs() };
    CHECK(tabs.index_of(tabs.tabs()[2].id) == 2);
    CHECK_FALSE(tabs.index_of(999999).has_value());
    CHECK(tabs.find_repository(u8"c:/work/C") == 2);
    CHECK_FALSE(tabs.find_repository(repo(u8"zzz")).has_value());
}

TEST_CASE("session capture skips start tabs and remembers the active repository", "[repository_tabs]")
{
    repository_tabs tabs { three_tabs() };
    static_cast<void>(tabs.new_tab());
    REQUIRE(tabs.tabs()[3].kind == repository_tab_kind::start);
    REQUIRE(tabs.select(2));

    app_settings settings {};
    capture_open_tabs(tabs, settings);

    CHECK(settings.open_tabs == std::vector<std::u8string> { repo(u8"a"), repo(u8"b"), repo(u8"c") });
    CHECK(settings.active_tab == repo(u8"c"));

    // 시작 탭이 선택돼 있으면 활성 탭은 비운다.
    REQUIRE(tabs.select(3));
    capture_open_tabs(tabs, settings);
    CHECK(settings.active_tab.empty());
    CHECK(settings.open_tabs.size() == 3);
}

TEST_CASE("session restore rebuilds tabs and selects the active one", "[repository_tabs]")
{
    app_settings settings {};
    settings.open_tabs = { repo(u8"a"), repo(u8"b"), repo(u8"c") };
    settings.active_tab = repo(u8"b");

    const repository_tabs tabs { restore_tabs(settings) };

    CHECK(labels(tabs) == label_list { u8"a", u8"b", u8"c" });
    CHECK(tabs.selected_index() == 1);
}

TEST_CASE("session restore with nothing leaves one start tab", "[repository_tabs]")
{
    const repository_tabs tabs { restore_tabs(app_settings {}) };
    CHECK(labels(tabs) == label_list { u8"*" });
    CHECK(tabs.selected_index() == 0);

    app_settings only_empty {};
    only_empty.open_tabs = { u8"" };
    only_empty.active_tab = u8"C:\\gone";
    CHECK(labels(restore_tabs(only_empty)) == label_list { u8"*" });
}

TEST_CASE("session restore removes duplicates and falls back to the first tab", "[repository_tabs]")
{
    app_settings settings {};
    settings.open_tabs = { repo(u8"a"), u8"c:/WORK/a/", repo(u8"b"), u8"" };
    settings.active_tab = u8"C:\\work\\not-open";

    const repository_tabs tabs { restore_tabs(settings) };

    CHECK(labels(tabs) == label_list { u8"a", u8"b" });
    CHECK(tabs.selected_index() == 0);

    // 활성 경로는 인덱스가 아니라 경로로 찾으므로 중복이 걸러져도 같은 탭을 가리킨다.
    settings.active_tab = u8"C:/Work/B";
    CHECK(restore_tabs(settings).selected_index() == 1);
}

TEST_CASE("restore keeps ids monotonic", "[repository_tabs]")
{
    repository_tabs tabs {};
    const repository_tab_id before { tabs.selected().id };
    tabs.restore({ repo(u8"a"), repo(u8"b") }, repo(u8"a"));
    for (const repository_tab& tab : tabs.tabs())
        CHECK(tab.id > before);
}
