#include "presentation/palette/palette_model.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

using namespace cyella;

namespace {
    palette_context repository_context(const app_settings& settings)
    {
        palette_context context {};
        context.settings = &settings;
        context.availability.repository = true;
        context.availability.fetch = true;
        context.availability.pull = true;
        context.availability.push = true;
        context.availability.branch = true;
        context.availability.stash = true;
        context.availability.commit = true;
        return context;
    }

    std::vector<std::u8string> keys_of(const std::vector<palette_item>& items)
    {
        std::vector<std::u8string> keys {};
        for (const palette_item& item : items)
            keys.push_back(item.key);
        return keys;
    }

    std::size_t position_of(const std::vector<palette_item>& items, const std::u8string_view key)
    {
        const auto found { std::ranges::find(items, key, &palette_item::key) };
        return static_cast<std::size_t>(found - items.begin());
    }

    ref make_ref(const ref_kind kind, std::string name, const bool current = false)
    {
        ref value {};
        value.kind = kind;
        value.name = std::move(name);
        value.is_current = current;
        return value;
    }
} // namespace

TEST_CASE("commands that cannot run are left out of the list", "[palette]")
{
    const app_settings settings {};
    palette_context start {};
    start.settings = &settings;
    const std::vector<palette_item> before { rank_palette_items(start, u8"") };
    REQUIRE(std::ranges::find(before, u8"cmd:git.fetch", &palette_item::key) == before.end());
    REQUIRE(std::ranges::find(before, u8"cmd:app.settings", &palette_item::key) != before.end());

    const palette_context repository { repository_context(settings) };
    const std::vector<palette_item> after { rank_palette_items(repository, u8"") };
    REQUIRE(std::ranges::find(after, u8"cmd:git.fetch", &palette_item::key) != after.end());
    // 풀 상위가 없으면 pull은 서지 않는다.
    palette_context no_pull { repository };
    no_pull.availability.pull = false;
    const std::vector<palette_item> without { rank_palette_items(no_pull, u8"") };
    REQUIRE(std::ranges::find(without, u8"cmd:git.pull", &palette_item::key) == without.end());
}

TEST_CASE("initial consonants find the command", "[palette]")
{
    const app_settings settings {};
    const palette_context context { repository_context(settings) };
    const std::vector<palette_item> fetch { rank_palette_items(context, u8"ㄱㅈㅇㄱ") };
    REQUIRE(fetch.empty() == false);
    REQUIRE(fetch.front().key == u8"cmd:git.fetch");

    const std::vector<palette_item> branch { rank_palette_items(context, u8"ㅂㄹㅊ") };
    REQUIRE(branch.size() >= 2);
    // "브랜치로 이동…"이 글머리에서 맞고 "새 브랜치 만들기…"는 중간에서 맞는다.
    REQUIRE(branch.front().key == u8"cmd:git.branch.checkout");
    REQUIRE(position_of(branch, u8"cmd:git.branch.new") == 1);
}

TEST_CASE("english keywords find commands too", "[palette]")
{
    const app_settings settings {};
    const palette_context context { repository_context(settings) };
    const std::vector<palette_item> found { rank_palette_items(context, u8"stash") };
    REQUIRE(found.empty() == false);
    REQUIRE(std::ranges::find(found, u8"cmd:git.stash", &palette_item::key) != found.end());
    REQUIRE(rank_palette_items(context, u8"qqqzzz").empty());
}

TEST_CASE("the most recently used command ranks first among equal matches", "[palette]")
{
    app_settings settings {};
    const palette_context empty_history { repository_context(settings) };
    // "원격"으로 시작하는 명령이 셋이다 (가져오기·받아 합치기·올리기). 글머리에서 맞는 점수가 비슷해 최근에 쓴 것이 이긴다.
    const std::vector<palette_item> plain { rank_palette_items(empty_history, u8"원격") };
    REQUIRE(plain.size() >= 3);

    record_command_use(settings, "git.fetch");
    const palette_context fetched { repository_context(settings) };
    REQUIRE(rank_palette_items(fetched, u8"원격").front().key == u8"cmd:git.fetch");

    record_command_use(settings, "git.pull");
    const palette_context pulled { repository_context(settings) };
    const std::vector<palette_item> after_pull { rank_palette_items(pulled, u8"원격") };
    REQUIRE(after_pull.front().key == u8"cmd:git.pull");
    // 그 전에 쓴 fetch가 한 번도 안 쓴 push보다 앞이다.
    REQUIRE(position_of(after_pull, u8"cmd:git.fetch") < position_of(after_pull, u8"cmd:git.push"));
}

TEST_CASE("without a query the recently used commands come first", "[palette]")
{
    app_settings settings {};
    record_command_use(settings, "git.stash");
    record_command_use(settings, "tab.new");
    const palette_context context { repository_context(settings) };
    const std::vector<palette_item> items { rank_palette_items(context, u8"") };
    REQUIRE(items.size() > 3);
    REQUIRE(items[0].key == u8"cmd:tab.new");
    REQUIRE(items[1].key == u8"cmd:git.stash");
}

TEST_CASE("recency never adds a command that does not match", "[palette]")
{
    app_settings settings {};
    record_command_use(settings, "app.log");
    const palette_context context { repository_context(settings) };
    const std::vector<palette_item> items { rank_palette_items(context, u8"fetch") };
    REQUIRE(items.front().key == u8"cmd:git.fetch");
    REQUIRE(std::ranges::find(items, u8"cmd:app.log", &palette_item::key) == items.end());
}

TEST_CASE("shortcuts show next to the commands", "[palette]")
{
    app_settings settings {};
    const palette_context context { repository_context(settings) };
    const std::vector<palette_item> items { rank_palette_items(context, u8"명령 팔레트") };
    REQUIRE(items.empty() == false);
    REQUIRE(items.front().key == u8"cmd:palette.open");
    REQUIRE(items.front().detail == u8"Ctrl+Shift+P / Ctrl+P");
}

TEST_CASE("the selected item's menu entries join the commands", "[palette]")
{
    const app_settings settings {};
    palette_context context { repository_context(settings) };
    context.selection_menu.title = u8"feature/graph";
    context.selection_menu.entries.push_back(menu_entry { .key = u8"checkout", .label = u8"체크아웃" });
    context.selection_menu.entries.push_back(menu_entry { .key = u8"merge_into_current", .label = u8"현재 브랜치에 merge", .enabled = false });
    context.selection_menu.entries.push_back(menu_entry { .key = u8"copy_name", .label = u8"이름 복사" });
    const std::vector<palette_item> found { rank_palette_items(context, u8"체크아웃") };
    REQUIRE(found.empty() == false);
    REQUIRE(found.front().key == u8"menu:checkout");
    REQUIRE(found.front().title == u8"체크아웃 — feature/graph");
    // 꺼진 항목은 서지 않는다.
    const std::vector<palette_item> every { rank_palette_items(context, u8"") };
    REQUIRE(std::ranges::find(every, u8"menu:merge_into_current", &palette_item::key) == every.end());
    REQUIRE(std::ranges::find(every, u8"menu:copy_name", &palette_item::key) != every.end());
    // 키(영문)로도 찾는다.
    REQUIRE(rank_palette_items(context, u8"copy name").empty() == false);
}

TEST_CASE("branch mode lists local then remote branches and disables the current one", "[palette]")
{
    const app_settings settings {};
    palette_context context { repository_context(settings) };
    context.mode = palette_mode::branches;
    context.current_branch = "main";
    context.refs.push_back(make_ref(ref_kind::local, "feature/graph"));
    context.refs.push_back(make_ref(ref_kind::local, "main", true));
    context.refs.push_back(make_ref(ref_kind::remote, "origin/main"));
    context.refs.push_back(make_ref(ref_kind::remote, "origin/HEAD"));
    context.refs.push_back(make_ref(ref_kind::tag, "v1"));
    const std::vector<palette_item> all { rank_palette_items(context, u8"") };
    REQUIRE(keys_of(all) == std::vector<std::u8string> { u8"branch:feature/graph", u8"remote:origin/main", u8"branch:main" });
    REQUIRE(all.back().enabled == false);
    REQUIRE(all.back().detail == u8"현재 브랜치");

    const std::vector<palette_item> fuzzy { rank_palette_items(context, u8"fg") };
    REQUIRE(fuzzy.empty() == false);
    REQUIRE(fuzzy.front().key == u8"branch:feature/graph");
    // 분리된 머리에서는 현재 브랜치가 없다.
    context.detached = true;
    const std::vector<palette_item> detached { rank_palette_items(context, u8"main") };
    REQUIRE(std::ranges::find_if(detached, [](const palette_item& item) { return item.key == u8"branch:main"; })->enabled);
}

TEST_CASE("repository mode lists tabs, recent repositories and the folder dialog", "[palette]")
{
    const app_settings settings {};
    palette_context context { repository_context(settings) };
    context.mode = palette_mode::repositories;
    context.repositories.push_back(repository_choice { .tab = 3, .name = u8"alpha", .path = u8"E:\\work\\alpha", .current = true });
    context.repositories.push_back(repository_choice { .tab = 0, .name = u8"beta", .path = u8"E:\\work\\beta" });
    context.repositories.push_back(repository_choice { .tab = 0, .name = u8"gone", .path = u8"E:\\work\\gone", .available = false });
    const std::vector<palette_item> all { rank_palette_items(context, u8"") };
    // 고를 수 있는 항목이 앞이다. 현재 탭과 사용할 수 없는 경로는 꺼져 있어 뒤로 간다.
    REQUIRE(keys_of(all) == std::vector<std::u8string> { u8"recent:E:\\work\\beta", u8"cmd:repo.open_folder", u8"tab:3", u8"recent:E:\\work\\gone" });
    REQUIRE(all[1].ui_thread_command);
    REQUIRE(all[2].enabled == false);
    REQUIRE(all[3].enabled == false);
    // 경로로도 찾는다.
    const std::vector<palette_item> by_path { rank_palette_items(context, u8"work beta") };
    REQUIRE(by_path.front().key == u8"recent:E:\\work\\beta");
}

TEST_CASE("profile mode marks the applied and the suggested profile", "[palette]")
{
    app_settings settings {};
    settings.profiles.push_back(user_profile { .name = u8"회사", .user_name = u8"홍길동", .user_email = u8"hong@corp.example" });
    settings.profiles.push_back(user_profile { .name = u8"개인", .user_name = u8"길동", .user_email = u8"me@example.com" });
    palette_context context { repository_context(settings) };
    context.mode = palette_mode::profiles;
    context.current_profile = u8"개인";
    context.suggested_profile = u8"회사";
    const std::vector<palette_item> all { rank_palette_items(context, u8"") };
    REQUIRE(all.size() == 3);
    REQUIRE(all[0].key == u8"profile:회사");
    REQUIRE(all[0].detail.find(u8"규칙 제안") != std::u8string::npos);
    REQUIRE(all[1].detail.find(u8"적용됨") != std::u8string::npos);
    REQUIRE(all[2].key == palette_manage_profiles_key);
    // 이메일로도 찾는다.
    REQUIRE(rank_palette_items(context, u8"corp").front().key == u8"profile:회사");
}

TEST_CASE("the visible list is capped and says how many are hidden", "[palette]")
{
    const app_settings settings {};
    palette_context context { repository_context(settings) };
    context.mode = palette_mode::branches;
    for (int index { 0 }; index < 40; ++index)
        context.refs.push_back(make_ref(ref_kind::local, "topic/" + std::to_string(index)));
    const palette_list list { build_palette_list(context, u8"") };
    REQUIRE(list.matched == 40);
    REQUIRE(list.hidden == 40 - palette_visible_limit);
    REQUIRE(list.items.size() == palette_visible_limit + 1);
    REQUIRE(list.items.back().key == palette_more_key);
    REQUIRE(list.items.back().enabled == false);

    const palette_list none { build_palette_list(context, u8"qqqzzz") };
    REQUIRE(none.matched == 0);
    REQUIRE(none.items.size() == 1);
    REQUIRE(none.items.front().key == palette_empty_key);
}
