// 우클릭 메뉴와 끌어 놓기 메뉴의 항목 규칙, 끄는 동안의 문구, 읽어 둔 이력의 조상 판정을 검증한다 (UI 없이).
#include "presentation/drop_hints.h"
#include "presentation/graph_index.h"
#include "presentation/menu_model.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {
    using namespace cyella;

    std::string sha_of(const int index)
    {
        char buffer[41] {};
        std::snprintf(buffer, sizeof(buffer), "%08x%032x", index, index);
        return buffer;
    }

    commit_info commit(const int index, std::vector<int> parents)
    {
        commit_info value {};
        value.sha = sha_of(index);
        for (const int parent : parents)
            value.parents.push_back(sha_of(parent));
        return value;
    }

    // topo 순서(자식이 먼저)다. 9 → 8 → 7 → 5(main), 8 → 6 → 5 갈래 둘이 5에서 만난다:
    //   9(feature) ─ 8 ─ 6 ─┐
    //                        5 ─ 4 ─ 3(main)
    //   7(side) ───────────┘
    // 실제로는 아래처럼 쓴다.
    //   9: 부모 8, 8: 부모 5, 7: 부모 5, 5: 부모 4, 4: 부모 3, 3: 부모 없음
    std::vector<commit_info> history()
    {
        return { commit(9, { 8 }), commit(7, { 5 }), commit(8, { 5 }), commit(5, { 4 }), commit(4, { 3 }), commit(3, {}) };
    }

    bool has_entry(const menu_model& model, const std::u8string& key)
    {
        return std::any_of(model.entries.begin(), model.entries.end(), [&key](const menu_entry& entry) { return entry.key == key; });
    }

    bool entry_enabled(const menu_model& model, const std::u8string& key)
    {
        for (const menu_entry& entry : model.entries)
            if (entry.key == key)
                return entry.enabled;
        return false;
    }

    menu_context main_context()
    {
        menu_context context {};
        context.current_branch = "main";
        context.head_sha = sha_of(7);
        context.remotes = { "origin" };
        context.refs = {
            { .kind = ref_kind::local, .name = "main", .target_sha = sha_of(7), .upstream = "origin/main", .is_current = true },
            { .kind = ref_kind::local, .name = "feature", .target_sha = sha_of(9) },
            { .kind = ref_kind::remote, .name = "origin/main", .target_sha = sha_of(5) },
            { .kind = ref_kind::tag, .name = "v1", .target_sha = sha_of(5), .remotes = { "origin" } },
            { .kind = ref_kind::tag, .name = "v2", .target_sha = sha_of(5) },
        };
        return context;
    }

    drop_party local(const std::string& name, const int sha)
    {
        return { .kind = ref_kind::local, .name = name, .sha = sha_of(sha), .is_commit = false };
    }

    drop_party remote(const std::string& name, const int sha)
    {
        return { .kind = ref_kind::remote, .name = name, .sha = sha_of(sha), .is_commit = false };
    }
} // namespace

TEST_CASE("the history index answers ancestry and commit counts for loaded commits", "[presentation][drop]")
{
    const graph_index index { history() };
    CHECK(index.size() == 6);
    CHECK(index.is_ancestor(sha_of(5), sha_of(9)) == true);
    CHECK(index.is_ancestor(sha_of(3), sha_of(7)) == true);
    CHECK(index.is_ancestor(sha_of(9), sha_of(5)) == false);
    CHECK(index.is_ancestor(sha_of(7), sha_of(9)) == false);
    CHECK(index.is_ancestor(sha_of(9), sha_of(9)) == true);
    // 읽지 않은 커밋은 모른다.
    CHECK_FALSE(index.is_ancestor(sha_of(1), sha_of(9)).has_value());

    // rev-list --count from..to
    CHECK(index.count(sha_of(5), sha_of(9)) == 2);
    CHECK(index.count(sha_of(7), sha_of(9)) == 2);
    CHECK(index.count(sha_of(9), sha_of(7)) == 1);
    CHECK(index.count(sha_of(9), sha_of(5)) == 0);
    CHECK(index.count(sha_of(3), sha_of(9)) == 4);
    CHECK_FALSE(index.count(sha_of(1), sha_of(9)).has_value());
    // 항목 키(앞 15자)로도 찾는다.
    CHECK(index.find(sha_of(9).substr(0, 15)) == index.find(sha_of(9)));
}

TEST_CASE("which branches can be dropped on which", "[presentation][drop]")
{
    CHECK(can_drop_on(local("feature", 9), local("main", 7)));
    CHECK(can_drop_on(local("feature", 9), remote("origin/main", 5)));
    CHECK(can_drop_on(remote("origin/main", 5), local("main", 7)));
    CHECK_FALSE(can_drop_on(local("main", 7), local("main", 7)));
    CHECK_FALSE(can_drop_on(remote("origin/main", 5), remote("origin/other", 5)));
    drop_party commit_party {};
    commit_party.is_commit = true;
    commit_party.sha = sha_of(4);
    CHECK(can_drop_on(local("feature", 9), commit_party));
    CHECK(can_drop_on(remote("origin/main", 5), commit_party));
    // 커밋을 끌 수는 없다.
    CHECK_FALSE(can_drop_on(commit_party, local("main", 7)));
}

TEST_CASE("the drop menu follows the contract table", "[presentation][drop]")
{
    const graph_index index { history() };
    menu_context context { main_context() };
    const auto facts_for = [&index](const drop_party& source, const drop_party& destination) {
        drop_facts facts {};
        facts.destination_is_ancestor = index.is_ancestor(destination.sha, source.sha);
        facts.source_is_ancestor = index.is_ancestor(source.sha, destination.sha);
        facts.only_in_source = index.count(destination.sha, source.sha);
        facts.only_in_destination = index.count(source.sha, destination.sha);
        return facts;
    };

    SECTION("feature onto main (current): rebase, merge and reset, but no fast-forward")
    {
        menu_target target { .kind = menu_target_kind::drop, .source = local("feature", 9), .destination = local("main", 7) };
        context.facts = facts_for(target.source, target.destination);
        const menu_model model { build_context_menu(target, context) };
        CHECK(model.title == u8"feature → main");
        CHECK_FALSE(has_entry(model, u8"fast_forward"));
        CHECK(entry_enabled(model, u8"merge"));
        CHECK(entry_enabled(model, u8"rebase"));
        // 인터랙티브 rebase도 켜져 있고 움직일 커밋 수가 보인다.
        CHECK(entry_enabled(model, u8"interactive_rebase"));
        const auto interactive { std::find_if(model.entries.begin(), model.entries.end(), [](const menu_entry& entry) { return entry.key == u8"interactive_rebase"; }) };
        REQUIRE(interactive != model.entries.end());
        CHECK(interactive->label == u8"feature를 main 위로 인터랙티브 rebase… · 커밋 2개");
        // 놓은 쪽이 현재 브랜치라 reset이 보인다. hard는 위험 표시다.
        CHECK(has_entry(model, u8"reset_soft"));
        CHECK(has_entry(model, u8"reset_mixed"));
        const auto hard { std::find_if(model.entries.begin(), model.entries.end(), [](const menu_entry& entry) { return entry.key == u8"reset_hard"; }) };
        REQUIRE(hard != model.entries.end());
        CHECK(hard->danger);
        const auto rebase { std::find_if(model.entries.begin(), model.entries.end(), [](const menu_entry& entry) { return entry.key == u8"rebase"; }) };
        CHECK(rebase->label == u8"feature를 main 위로 rebase · 커밋 2개");
    }

    SECTION("main onto feature: only fast-forward can be offered when the target is an ancestor")
    {
        // 놓은 쪽(feature)은 끈 쪽(main=7)의 조상이 아니다.
        menu_target target { .kind = menu_target_kind::drop, .source = local("main", 7), .destination = local("feature", 9) };
        context.facts = facts_for(target.source, target.destination);
        const menu_model model { build_context_menu(target, context) };
        CHECK_FALSE(has_entry(model, u8"fast_forward"));
        // feature는 현재 브랜치가 아니라 reset은 없다.
        CHECK_FALSE(has_entry(model, u8"reset_hard"));
    }

    SECTION("an ancestor target offers fast-forward and disables rebase")
    {
        // 놓은 쪽 5가 끈 쪽 9의 조상이다.
        menu_target target { .kind = menu_target_kind::drop, .source = local("feature", 9), .destination = local("old", 5) };
        context.facts = facts_for(target.source, target.destination);
        const menu_model model { build_context_menu(target, context) };
        REQUIRE(has_entry(model, u8"fast_forward"));
        CHECK(entry_enabled(model, u8"fast_forward"));
        CHECK_FALSE(entry_enabled(model, u8"rebase"));
        const auto forward { std::find_if(model.entries.begin(), model.entries.end(), [](const menu_entry& entry) { return entry.key == u8"fast_forward"; }) };
        CHECK(forward->label == u8"old를 feature로 fast-forward · 커밋 2개");
    }

    SECTION("a remote branch target shows only merge and rebase")
    {
        menu_target target { .kind = menu_target_kind::drop, .source = local("feature", 9), .destination = remote("origin/main", 5) };
        context.facts = facts_for(target.source, target.destination);
        const menu_model model { build_context_menu(target, context) };
        REQUIRE(model.entries.size() == 2);
        CHECK(model.entries[0].key == u8"merge_remote_into_source");
        CHECK(model.entries[1].key == u8"rebase");
        // 원격 브랜치가 이미 끈 쪽의 조상이면 merge는 이미 포함이다.
        CHECK_FALSE(model.entries[0].enabled);
        CHECK_FALSE(model.entries[1].enabled);
    }

    SECTION("a commit target offers a new branch there, and reset for the current branch")
    {
        drop_party commit_party {};
        commit_party.is_commit = true;
        commit_party.sha = sha_of(4);
        menu_target for_feature { .kind = menu_target_kind::drop, .source = local("feature", 9), .destination = commit_party };
        const menu_model plain { build_context_menu(for_feature, context) };
        REQUIRE(plain.entries.size() == 1);
        CHECK(plain.entries[0].key == u8"create_branch_here");

        menu_target for_current { .kind = menu_target_kind::drop, .source = local("main", 7), .destination = commit_party };
        const menu_model with_reset { build_context_menu(for_current, context) };
        CHECK(has_entry(with_reset, u8"create_branch_here"));
        CHECK(has_entry(with_reset, u8"reset_hard"));
    }

    SECTION("unknown relations leave fast-forward disabled until the answer arrives")
    {
        menu_target target { .kind = menu_target_kind::drop, .source = local("feature", 9), .destination = local("old", 5) };
        const menu_model model { build_context_menu(target, context) };
        REQUIRE(has_entry(model, u8"fast_forward"));
        CHECK_FALSE(entry_enabled(model, u8"fast_forward"));
    }

    SECTION("impossible drops produce no entries")
    {
        menu_target target { .kind = menu_target_kind::drop, .source = remote("origin/main", 5), .destination = remote("origin/x", 5) };
        CHECK(build_context_menu(target, context).entries.empty());
    }
}

TEST_CASE("the hover hint names the likely action", "[presentation][drop]")
{
    auto data { std::make_shared<drop_hint_data>() };
    data->index = std::make_shared<const graph_index>(history());
    data->current_branch = "main";
    data->branches.emplace(u8"l:feature", local("feature", 9));
    data->branches.emplace(u8"l:main", local("main", 7));
    data->branches.emplace(u8"l:old", local("old", 5));
    data->branches.emplace(u8"rb:origin/main", remote("origin/main", 5));

    // optional을 Catch가 풀어 쓰지 않도록 글로 바꿔 견준다. 힌트가 없으면 빈 글이다.
    const auto hint = [&data](const std::u8string& source, const std::u8string& target) { return data->describe(source, target).value_or(std::u8string {}); };
    CHECK(hint(u8"l:feature", u8"label-l:main") == u8"feature를 main 위로 rebase · 커밋 2개");
    // 사이드바 행과 그래프 라벨은 같은 키를 쓴다.
    CHECK(hint(u8"l:feature", u8"row-l:main") == u8"feature를 main 위로 rebase · 커밋 2개");
    CHECK(hint(u8"l:feature", u8"label-l:old") == u8"old를 feature로 fast-forward · 커밋 2개");
    CHECK(hint(u8"l:feature", u8"label-rb:origin/main") == u8"feature를 origin/main 위로 rebase · 커밋 2개");
    CHECK(hint(u8"l:main", u8"label-l:main").empty());
    const std::string commit_sha { sha_of(4) };
    const std::u8string commit_owner { u8"commit-" + std::u8string { commit_sha.begin(), commit_sha.begin() + 15 } };
    CHECK(hint(u8"l:feature", commit_owner) == u8"0000000 커밋에서 브랜치 만들기");
    CHECK(hint(u8"l:feature", u8"unknown-owner").empty());
}

TEST_CASE("context menu entries depend on the target", "[presentation][menu]")
{
    menu_context context { main_context() };

    SECTION("commit")
    {
        const menu_model model { build_context_menu({ .kind = menu_target_kind::commit, .sha = sha_of(9) }, context) };
        for (const char8_t* key : { u8"checkout_detached", u8"create_branch", u8"create_tag", u8"cherry_pick", u8"revert", u8"reset_soft", u8"reset_mixed", u8"reset_hard", u8"copy_sha" })
            CHECK(has_entry(model, key));
        CHECK(entry_enabled(model, u8"cherry_pick"));
        // 그 커밋 위의 커밋들을 인터랙티브 rebase한다.
        CHECK(entry_enabled(model, u8"interactive_rebase_here"));
        // HEAD 커밋은 cherry-pick·reset·인터랙티브 rebase의 대상이 아니다.
        const menu_model head { build_context_menu({ .kind = menu_target_kind::commit, .sha = sha_of(7) }, context) };
        CHECK_FALSE(entry_enabled(head, u8"cherry_pick"));
        CHECK_FALSE(entry_enabled(head, u8"reset_hard"));
        CHECK_FALSE(entry_enabled(head, u8"interactive_rebase_here"));
        // 병합 커밋은 cherry-pick과 revert가 꺼진다.
        context.commit_is_merge = true;
        const menu_model merge { build_context_menu({ .kind = menu_target_kind::commit, .sha = sha_of(9) }, context) };
        CHECK_FALSE(entry_enabled(merge, u8"cherry_pick"));
        CHECK_FALSE(entry_enabled(merge, u8"revert"));
    }

    SECTION("local branch")
    {
        const menu_model other { build_context_menu({ .kind = menu_target_kind::local_branch, .name = "feature", .sha = sha_of(9) }, context) };
        for (const char8_t* key :
            { u8"checkout", u8"merge_into_current", u8"rebase_current_onto", u8"interactive_rebase_current_onto", u8"push", u8"set_upstream", u8"rename", u8"delete", u8"copy_name" })
            CHECK(has_entry(other, key));
        CHECK(entry_enabled(other, u8"interactive_rebase_current_onto"));
        CHECK(entry_enabled(other, u8"checkout"));
        CHECK(entry_enabled(other, u8"delete"));
        CHECK(other.entries.front().label == u8"체크아웃");
        // upstream 해제는 upstream이 있는 브랜치에서만 켜진다 (main은 origin/main을 가진다).
        CHECK(has_entry(other, u8"unset_upstream"));
        CHECK_FALSE(entry_enabled(other, u8"unset_upstream"));
        const menu_model tracked { build_context_menu({ .kind = menu_target_kind::local_branch, .name = "main", .sha = sha_of(7) }, context) };
        CHECK(entry_enabled(tracked, u8"unset_upstream"));
        // 현재 브랜치는 체크아웃·삭제·자기 자신에게 merge가 꺼진다.
        const menu_model current { build_context_menu({ .kind = menu_target_kind::local_branch, .name = "main", .sha = sha_of(7) }, context) };
        CHECK_FALSE(entry_enabled(current, u8"checkout"));
        CHECK_FALSE(entry_enabled(current, u8"delete"));
        CHECK_FALSE(entry_enabled(current, u8"merge_into_current"));
        CHECK_FALSE(entry_enabled(current, u8"interactive_rebase_current_onto"));
        // detached HEAD에서는 현재 브랜치가 없어 merge·rebase를 쓸 수 없다.
        context.detached = true;
        const menu_model detached { build_context_menu({ .kind = menu_target_kind::local_branch, .name = "feature", .sha = sha_of(9) }, context) };
        CHECK_FALSE(entry_enabled(detached, u8"merge_into_current"));
    }

    SECTION("remote branch")
    {
        const menu_model model { build_context_menu({ .kind = menu_target_kind::remote_branch, .name = "origin/main", .sha = sha_of(5) }, context) };
        CHECK(has_entry(model, u8"checkout"));
        CHECK(has_entry(model, u8"merge_into_current"));
        CHECK_FALSE(has_entry(model, u8"delete"));
        CHECK_FALSE(has_entry(model, u8"rename"));
    }

    SECTION("tag lists one push entry per remote and a remote delete per remote that has it")
    {
        context.remotes = { "origin", "upstream" };
        context.refs[3].remotes = { "origin" };
        const menu_model model { build_context_menu({ .kind = menu_target_kind::tag, .name = "v1", .sha = sha_of(5) }, context) };
        REQUIRE(has_entry(model, u8"push:origin"));
        REQUIRE(has_entry(model, u8"push:upstream"));
        // 이미 그 원격에 있는 태그는 push할 수 없다.
        CHECK_FALSE(entry_enabled(model, u8"push:origin"));
        CHECK(entry_enabled(model, u8"push:upstream"));
        CHECK(has_entry(model, u8"delete_local"));
        CHECK(has_entry(model, u8"delete_remote:origin"));
        CHECK_FALSE(has_entry(model, u8"delete_remote:upstream"));
    }

    SECTION("stash")
    {
        const menu_model model { build_context_menu({ .kind = menu_target_kind::stash, .name = "stash@{0}", .index = 0 }, context) };
        CHECK(has_entry(model, u8"apply"));
        CHECK(has_entry(model, u8"pop"));
        REQUIRE(has_entry(model, u8"drop"));
        CHECK(model.entries.back().danger);
    }

    SECTION("tags header offers push all for each remote")
    {
        context.remotes = { "origin", "upstream" };
        const menu_model model { build_context_menu({ .kind = menu_target_kind::tags_section }, context) };
        CHECK(has_entry(model, u8"push_all:origin"));
        CHECK(has_entry(model, u8"push_all:upstream"));
        CHECK(has_entry(model, u8"create_tag_head"));
        // v2는 원격에 없고 v1은 origin에 있다.
        CHECK(model.entries.front().label.find(u8"원격에 없는 태그 1개") != std::u8string::npos);
    }

    SECTION("working tree files")
    {
        const menu_model unstaged { build_context_menu({ .kind = menu_target_kind::file, .name = "a.cpp", .side = change_side::unstaged }, context) };
        CHECK(has_entry(unstaged, u8"stage"));
        CHECK(entry_enabled(unstaged, u8"discard"));
        CHECK(has_entry(unstaged, u8"ignore"));
        CHECK(has_entry(unstaged, u8"open_external_diff"));
        CHECK_FALSE(has_entry(unstaged, u8"unstage"));

        const menu_model staged { build_context_menu({ .kind = menu_target_kind::file, .name = "a.cpp", .side = change_side::staged }, context) };
        CHECK(has_entry(staged, u8"unstage"));
        CHECK_FALSE(has_entry(staged, u8"discard"));

        // 추적하지 않는 파일은 git으로 버릴 수 없어 휴지통으로 보낸다.
        const menu_model untracked { build_context_menu({ .kind = menu_target_kind::file, .name = "new.txt", .side = change_side::unstaged, .untracked = true }, context) };
        CHECK(entry_enabled(untracked, u8"discard"));
        const auto trash_entry { std::ranges::find(untracked.entries, std::u8string { u8"discard" }, &menu_entry::key) };
        REQUIRE(trash_entry != untracked.entries.end());
        CHECK(trash_entry->label == u8"휴지통으로 보내기…");
        CHECK_FALSE(entry_enabled(untracked, u8"open_external_diff"));

        const menu_model conflicted { build_context_menu({ .kind = menu_target_kind::file, .name = "c.cpp", .conflicted = true }, context) };
        CHECK_FALSE(has_entry(conflicted, u8"stage"));
    }

    SECTION("files of a commit only open the diff and copy")
    {
        const menu_model model { build_context_menu({ .kind = menu_target_kind::commit_file, .name = "src/a.cpp", .sha = "1234567890abcdef" }, context) };
        CHECK(model.title == u8"src/a.cpp");
        CHECK(entry_enabled(model, u8"open_diff"));
        CHECK(entry_enabled(model, u8"copy_path"));
        CHECK(entry_enabled(model, u8"copy_name"));
        const auto revision { std::ranges::find(model.entries, std::u8string { u8"copy_revision_path" }, &menu_entry::key) };
        REQUIRE(revision != model.entries.end());
        CHECK(revision->label == u8"1234567:src/a.cpp 복사");
        // 커밋 안의 파일은 고칠 것이 없다.
        CHECK_FALSE(has_entry(model, u8"stage"));
        CHECK_FALSE(has_entry(model, u8"discard"));
    }
}

TEST_CASE("branch drag keys round trip", "[presentation][drop]")
{
    CHECK(branch_drag_key(ref_kind::local, "feature/x") == u8"l:feature/x");
    CHECK(branch_drag_key(ref_kind::remote, "origin/x") == u8"rb:origin/x");
    CHECK(branch_drag_key(ref_kind::tag, "v1").empty());
    const auto parsed { parse_branch_drag_key(u8"rb:origin/x") };
    REQUIRE(parsed.has_value());
    CHECK(parsed->kind == ref_kind::remote);
    CHECK(parsed->name == "origin/x");
    CHECK_FALSE(parse_branch_drag_key(u8"t:v1").has_value());
    CHECK(menu_key_argument(u8"push:origin", u8"push:") == "origin");
    CHECK_FALSE(menu_key_argument(u8"push:", u8"push:").has_value());
}

TEST_CASE("the tab menu closes tabs in groups and opens repository folders elsewhere", "[menu][tabs]")
{
    const auto keys = [](const menu_model& model) {
        std::vector<std::u8string> list {};
        for (const menu_entry& entry : model.entries)
            list.push_back(entry.key);
        return list;
    };
    const auto enabled = [](const menu_model& model, const std::u8string_view key) {
        const auto found { std::ranges::find(model.entries, key, &menu_entry::key) };
        REQUIRE(found != model.entries.end());
        return found->enabled;
    };

    repository_tabs tabs {};
    const repository_tab_id start { tabs.selected().id };
    // 시작 탭 하나뿐이면 닫을 것이 없다.
    const menu_model only { build_tab_menu(tabs, start) };
    CHECK(only.title == u8"시작");
    CHECK(keys(only) == std::vector<std::u8string> { u8"close", u8"close_others", u8"close_right" });
    CHECK_FALSE(enabled(only, u8"close"));
    CHECK_FALSE(enabled(only, u8"close_others"));
    CHECK_FALSE(enabled(only, u8"close_right"));

    const repository_tab_id first { tabs.open_repository(u8"C:\\work\\first").id };
    const repository_tab_id second { tabs.open_repository(u8"C:\\work\\second").id };
    const menu_model middle { build_tab_menu(tabs, first) };
    CHECK(middle.title == u8"first");
    CHECK(keys(middle) == std::vector<std::u8string> { u8"close", u8"close_others", u8"close_right", u8"copy_path", u8"reveal", u8"open_vscode" });
    CHECK(enabled(middle, u8"close"));
    CHECK(enabled(middle, u8"close_others"));
    CHECK(enabled(middle, u8"close_right"));
    // 맨 오른쪽 탭은 오른쪽에 닫을 탭이 없다.
    CHECK_FALSE(enabled(build_tab_menu(tabs, second), u8"close_right"));
    // 사라진 탭은 빈 메뉴다.
    CHECK(build_tab_menu(tabs, 999).entries.empty());
}
