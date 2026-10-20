#include "presentation/selector_model.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace cyella;

namespace {
    ref local_branch(std::string name, const bool current = false, const std::uint32_t ahead = 0, const std::uint32_t behind = 0)
    {
        ref value {};
        value.kind = ref_kind::local;
        value.full_name = "refs/heads/" + name;
        value.name = std::move(name);
        value.is_current = current;
        value.ahead = ahead;
        value.behind = behind;
        return value;
    }

    ref other_ref(const ref_kind kind, std::string name)
    {
        ref value {};
        value.kind = kind;
        value.name = std::move(name);
        return value;
    }

    const menu_entry* find_entry(const selector_list& list, const std::u8string& key)
    {
        const auto found { std::ranges::find(list.entries, key, &menu_entry::key) };
        return found == list.entries.end() ? nullptr : &*found;
    }

    std::vector<std::u8string> keys_of(const selector_list& list)
    {
        std::vector<std::u8string> keys {};
        for (const menu_entry& entry : list.entries)
            keys.push_back(entry.key);
        return keys;
    }
} // namespace

TEST_CASE("selector matching ignores ASCII case and slash direction and treats empty text as everything", "[presentation][selector]")
{
    CHECK(selector_matches(u8"Feature/Login", u8"feature"));
    CHECK(selector_matches(u8"Feature/Login", u8"  LOGIN "));
    CHECK(selector_matches(u8"C:\\work\\repo", u8"work/repo"));
    CHECK(selector_matches(u8"저장소 한글", u8"한글"));
    CHECK(selector_matches(u8"anything", u8""));
    CHECK(selector_matches(u8"anything", u8"   "));
    CHECK_FALSE(selector_matches(u8"main", u8"mainx"));
    CHECK_FALSE(selector_matches(u8"main", u8"dev"));
    CHECK_FALSE(selector_matches(u8"", u8"a"));
}

TEST_CASE("the branch selector lists local branches only and marks the current one as not selectable", "[presentation][selector]")
{
    const std::vector<ref> refs {
        local_branch("feature/login", false, 2, 1),
        local_branch("main", true),
        other_ref(ref_kind::remote, "origin/main"),
        other_ref(ref_kind::tag, "v1.0"),
        local_branch("release/1.0"),
    };
    const selector_list list { build_branch_selector(refs, u8"", false) };
    CHECK(keys_of(list) == std::vector<std::u8string> { u8"branch:feature/login", u8"branch:main", u8"branch:release/1.0" });
    CHECK(list.matched == 3);
    CHECK(list.hidden == 0);

    const menu_entry* const current { find_entry(list, u8"branch:main") };
    REQUIRE(current != nullptr);
    CHECK_FALSE(current->enabled);
    CHECK(current->label.find(u8"(현재)") != std::u8string::npos);
    const menu_entry* const other { find_entry(list, u8"branch:feature/login") };
    REQUIRE(other != nullptr);
    CHECK(other->enabled);
    // 앞섬·뒤처짐이 라벨 끝에 붙는다.
    CHECK(other->label == u8"feature/login   ↑2   ↓1");
    CHECK(current->icon != other->icon);
}

TEST_CASE("the branch selector filters by name and says when nothing matches", "[presentation][selector]")
{
    const std::vector<ref> refs { local_branch("feature/login", false), local_branch("feature/logout"), local_branch("main", true) };
    const selector_list filtered { build_branch_selector(refs, u8"LOG", false) };
    CHECK(keys_of(filtered) == std::vector<std::u8string> { u8"branch:feature/login", u8"branch:feature/logout" });

    const selector_list none { build_branch_selector(refs, u8"zzz", false) };
    CHECK(none.matched == 0);
    REQUIRE(none.entries.size() == 1);
    CHECK(none.entries.front().key == u8"none");
    CHECK_FALSE(none.entries.front().enabled);
}

TEST_CASE("a detached HEAD has no current branch so every branch can be checked out", "[presentation][selector]")
{
    const std::vector<ref> refs { local_branch("main", true), local_branch("dev") };
    const selector_list list { build_branch_selector(refs, u8"", true) };
    for (const menu_entry& entry : list.entries)
        CHECK(entry.enabled);
}

TEST_CASE("long branch lists are cut to the visible limit with a hint to narrow them", "[presentation][selector]")
{
    std::vector<ref> refs {};
    for (int index { 0 }; index < 30; ++index)
        refs.push_back(local_branch("topic/" + std::to_string(100 + index)));
    const selector_list list { build_branch_selector(refs, u8"", false) };
    CHECK(list.matched == 30);
    CHECK(list.hidden == 30 - selector_visible_limit);
    REQUIRE(list.entries.size() == selector_visible_limit + 1);
    CHECK(list.entries.back().key == u8"more");
    CHECK_FALSE(list.entries.back().enabled);
    CHECK(list.entries.back().label.find(u8"외 18개") != std::u8string::npos);

    // 거르면 찾을 수 있다.
    const selector_list narrowed { build_branch_selector(refs, u8"129", false) };
    CHECK(keys_of(narrowed) == std::vector<std::u8string> { u8"branch:topic/129" });
}

TEST_CASE("repository choices put open tabs first and skip recent entries that are already open", "[presentation][selector]")
{
    repository_tabs tabs {};
    const repository_tab_id alpha { tabs.open_repository(u8"C:\\work\\alpha").id };
    const repository_tab_id beta { tabs.open_repository(u8"C:\\work\\beta").id };
    app_settings settings {};
    settings.recent_repositories.push_back({ u8"C:\\work\\beta", u8"beta", u8"2026-10-20T10:00:00Z", false });
    settings.recent_repositories.push_back({ u8"C:\\old\\gamma", u8"gamma", u8"2026-10-19T10:00:00Z", false });
    settings.recent_repositories.push_back({ u8"D:\\gone\\delta", u8"delta", u8"2026-10-18T10:00:00Z", false });

    const auto exists = [](const std::u8string_view path) { return path != u8"D:\\gone\\delta"; };
    const std::vector<repository_choice> choices { make_repository_choices(tabs, settings, exists, beta) };
    REQUIRE(choices.size() == 4);
    CHECK(choices[0].tab == alpha);
    CHECK_FALSE(choices[0].current);
    CHECK(choices[1].tab == beta);
    CHECK(choices[1].current);
    CHECK(choices[2].tab == 0);
    CHECK(choices[2].name == u8"gamma");
    CHECK(choices[2].available);
    CHECK(choices[3].name == u8"delta");
    CHECK_FALSE(choices[3].available);
}

TEST_CASE("the repository selector groups tabs and recent entries, disables the current tab and missing paths and always offers the folder picker", "[presentation][selector]")
{
    repository_tabs tabs {};
    const repository_tab_id alpha { tabs.open_repository(u8"C:\\work\\alpha").id };
    const repository_tab_id beta { tabs.open_repository(u8"C:\\work\\beta").id };
    app_settings settings {};
    settings.recent_repositories.push_back({ u8"C:\\old\\gamma", u8"gamma", u8"2026-10-19T10:00:00Z", false });
    settings.recent_repositories.push_back({ u8"D:\\gone\\delta", u8"delta", u8"2026-10-18T10:00:00Z", false });
    const auto exists = [](const std::u8string_view path) { return path != u8"D:\\gone\\delta"; };

    const selector_list list { build_repository_selector(make_repository_choices(tabs, settings, exists, beta), u8"") };
    const auto tab_key = [](const repository_tab_id id) {
        const std::string digits { std::to_string(id) };
        return u8"tab:" + std::u8string { digits.begin(), digits.end() };
    };
    const std::u8string alpha_key { tab_key(alpha) };
    const std::u8string beta_key { tab_key(beta) };
    CHECK(keys_of(list) == std::vector<std::u8string> { alpha_key, beta_key, u8"recent:C:\\old\\gamma", u8"recent:D:\\gone\\delta", u8"open_folder" });

    const menu_entry* const alpha_entry { find_entry(list, alpha_key) };
    REQUIRE(alpha_entry != nullptr);
    CHECK(alpha_entry->enabled);
    CHECK(alpha_entry->label == u8"alpha   C:\\work");
    CHECK_FALSE(alpha_entry->separator_above);
    CHECK_FALSE(find_entry(list, beta_key)->enabled);
    // 최근 목록의 첫 항목 앞에 구분선이 선다.
    const menu_entry* const gamma { find_entry(list, u8"recent:C:\\old\\gamma") };
    REQUIRE(gamma != nullptr);
    CHECK(gamma->enabled);
    CHECK(gamma->separator_above);
    const menu_entry* const gone { find_entry(list, u8"recent:D:\\gone\\delta") };
    REQUIRE(gone != nullptr);
    CHECK_FALSE(gone->enabled);
    CHECK(gone->label.find(u8"사용할 수 없음") != std::u8string::npos);
    const menu_entry* const folder { find_entry(list, u8"open_folder") };
    REQUIRE(folder != nullptr);
    CHECK(folder->enabled);
    CHECK(folder->separator_above);
}

TEST_CASE("the repository selector filters by name or path and keeps the folder picker", "[presentation][selector]")
{
    repository_tabs tabs {};
    static_cast<void>(tabs.open_repository(u8"C:\\work\\alpha"));
    app_settings settings {};
    settings.recent_repositories.push_back({ u8"C:\\old\\gamma", u8"gamma", u8"2026-10-19T10:00:00Z", false });

    const std::vector<repository_choice> choices { make_repository_choices(tabs, settings, nullptr, tabs.selected().id) };
    const selector_list by_name { build_repository_selector(choices, u8"GAM") };
    CHECK(keys_of(by_name) == std::vector<std::u8string> { u8"recent:C:\\old\\gamma", u8"open_folder" });
    // 최근 목록만 남으면 앞에 열린 탭이 없어 구분선을 긋지 않는다.
    CHECK_FALSE(by_name.entries.front().separator_above);

    const selector_list by_path { build_repository_selector(choices, u8"c:/work") };
    CHECK(by_path.entries.front().label.find(u8"alpha") != std::u8string::npos);

    const selector_list none { build_repository_selector(choices, u8"nothing-like-this") };
    CHECK(keys_of(none) == std::vector<std::u8string> { u8"none", u8"open_folder" });
    CHECK(none.matched == 0);
}
