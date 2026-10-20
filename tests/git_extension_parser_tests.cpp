// submodule·worktree·Git LFS의 해석기와 명령 조립기 test다. 해석기는 실제 git 2.52와 git-lfs 3.7로 만든 고정
// 출력(tests/data/git/*)을 그대로 읽는다. 경로는 `C:/work`로 바꿔 두었다.
#include "domain/lfs.h"
#include "infrastructure/git_diff_parser.h"
#include "infrastructure/git_error_classifier.h"
#include "infrastructure/git_lfs_commands.h"
#include "infrastructure/git_lfs_parser.h"
#include "infrastructure/git_progress_parser.h"
#include "infrastructure/git_submodule_commands.h"
#include "infrastructure/git_submodule_parser.h"
#include "infrastructure/git_worktree_commands.h"
#include "infrastructure/git_worktree_parser.h"

#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    using namespace cyella;
    using cyella::test::read_fixture;
    using cyella::test::to_text;

    const std::string sub_a { "libs/subA" };
    const std::string sub_b { "libs/subB" };

    submodule_sources sources_of(const std::string_view status_fixture)
    {
        submodule_sources sources {};
        sources.status = parse_git_submodule_status(read_fixture(std::string { status_fixture }));
        sources.modules = parse_gitmodules_config(read_fixture("gitmodules_config.bin"));
        sources.gitlinks = parse_git_gitlinks(read_fixture("submodule_gitlinks.bin"));
        return sources;
    }

    const submodule_info* find_submodule(const std::vector<submodule_info>& list, const std::string& path)
    {
        for (const submodule_info& info : list)
            if (info.path == path)
                return &info;
        return nullptr;
    }
} // namespace

// ---------------------------------------------------------------- submodule

TEST_CASE("parse_git_submodule_status reads the four prefixes and the description", "[git][submodule]")
{
    const std::vector<submodule_status_entry> mixed { parse_git_submodule_status(read_fixture("submodule_status_mixed.bin")) };
    REQUIRE(mixed.size() == 2);
    CHECK(mixed[0].prefix == '+');
    CHECK(mixed[0].sha == "9b8895860a444517b36dd491c64f404084088883");
    CHECK(mixed[0].path == sub_a);
    CHECK(mixed[0].description == "9b88958");
    CHECK(mixed[1].prefix == '-');
    CHECK(mixed[1].path == sub_b);
    CHECK(mixed[1].description.empty());

    const std::vector<submodule_status_entry> clean { parse_git_submodule_status(read_fixture("submodule_status_clean.bin")) };
    REQUIRE(clean.size() == 2);
    CHECK(clean[0].prefix == ' ');
    CHECK(clean[0].description == "heads/main");

    const std::vector<submodule_status_entry> conflict { parse_git_submodule_status(read_fixture("submodule_status_conflict.bin")) };
    REQUIRE(conflict.size() == 2);
    CHECK(conflict[1].prefix == 'U');
    CHECK(conflict[1].sha == std::string(40, '0'));
}

TEST_CASE("parse_git_submodule_status keeps nested paths and tolerates spaces, CRLF and junk", "[git][submodule]")
{
    const std::vector<submodule_status_entry> nested { parse_git_submodule_status(read_fixture("submodule_status_nested.bin")) };
    REQUIRE(nested.size() == 2);
    CHECK(nested[1].path == "mid/a/deep/c");

    // 경로에 공백과 괄호가 있어도 마지막 ` (설명)`만 설명이다.
    std::string text { " 1ecfd40f48933961e1d368341744a076db579db3 my libs/a (b) (heads/main)\r\n" };
    text += "garbage line\r\n";
    text += "-5d78d2c7a83e11388390d75e4ed2c4e311003d5d plain dir\r\n";
    const std::vector<submodule_status_entry> entries { parse_git_submodule_status(text) };
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].path == "my libs/a (b)");
    CHECK(entries[0].description == "heads/main");
    CHECK(entries[1].path == "plain dir");
    CHECK(entries[1].description.empty());
}

TEST_CASE("parse_gitmodules_config joins path and url by section name", "[git][submodule]")
{
    const std::vector<gitmodules_entry> modules { parse_gitmodules_config(read_fixture("gitmodules_config.bin")) };
    REQUIRE(modules.size() == 2);
    CHECK(modules[0].name == sub_a);
    CHECK(modules[0].path == sub_a);
    CHECK(modules[0].url == "../subA");
    CHECK(modules[1].url == "../subB");
    CHECK(parse_gitmodules_config({}).empty());

    // 이름에 점이 있어도 마지막 점이 변수 경계다.
    const std::string nul(1, '\0');
    const std::string dotted { "submodule.vendor/lib.v2.path\nvendor/lib.v2" + nul + "submodule.vendor/lib.v2.url\nhttps://example.com/x.git" + nul };
    const std::vector<gitmodules_entry> dotted_entries { parse_gitmodules_config(dotted) };
    REQUIRE(dotted_entries.size() == 1);
    CHECK(dotted_entries[0].name == "vendor/lib.v2");
    CHECK(dotted_entries[0].path == "vendor/lib.v2");
    CHECK(dotted_entries[0].url == "https://example.com/x.git");
}

TEST_CASE("parse_git_gitlinks keeps only mode 160000 entries", "[git][submodule]")
{
    const std::vector<gitlink_entry> links { parse_git_gitlinks(read_fixture("submodule_gitlinks.bin")) };
    REQUIRE(links.size() == 2);
    CHECK(links[0].path == sub_a);
    CHECK(links[0].sha == "1ecfd40f48933961e1d368341744a076db579db3");
    CHECK(links[0].stage == 0);

    const std::string nul(1, '\0');
    const std::string with_file { "100644 e69de29bb2d1d6434b8b29ae775ad8c2e48c5391 0\ta.txt" + nul };
    CHECK(parse_git_gitlinks(with_file).empty());
}

TEST_CASE("assemble_submodules maps status prefixes to states", "[git][submodule]")
{
    SECTION("an uninitialized submodule keeps the recorded commit and the url")
    {
        const std::vector<submodule_info> list { assemble_submodules(sources_of("submodule_status_mixed.bin"), nullptr) };
        REQUIRE(list.size() == 2);
        const submodule_info& uninitialized { *find_submodule(list, sub_b) };
        CHECK(uninitialized.state == submodule_state::uninitialized);
        CHECK(uninitialized.recorded_sha == "5d78d2c7a83e11388390d75e4ed2c4e311003d5d");
        CHECK(uninitialized.checked_out_sha.empty());
        CHECK(uninitialized.url == "../subB");
        CHECK(uninitialized.name == sub_b);
    }

    SECTION("a plus prefix is out of date unless the recorded commit is an ancestor of the checked out one")
    {
        const std::vector<submodule_info> behind { assemble_submodules(sources_of("submodule_status_mixed.bin"), [](const submodule_info&) { return std::optional<bool> { false }; }) };
        const submodule_info& moved { *find_submodule(behind, sub_a) };
        CHECK(moved.state == submodule_state::out_of_date);
        CHECK(moved.recorded_sha == "1ecfd40f48933961e1d368341744a076db579db3");
        CHECK(moved.checked_out_sha == "9b8895860a444517b36dd491c64f404084088883");

        const std::vector<submodule_info> ahead { assemble_submodules(sources_of("submodule_status_mixed.bin"), [](const submodule_info&) { return std::optional<bool> { true }; }) };
        CHECK(find_submodule(ahead, sub_a)->state == submodule_state::modified);

        // 조상 관계를 모르면 뒤처진 것으로 본다.
        const std::vector<submodule_info> unknown { assemble_submodules(sources_of("submodule_status_mixed.bin"), nullptr) };
        CHECK(find_submodule(unknown, sub_a)->state == submodule_state::out_of_date);
    }

    SECTION("a matching checkout is initialized, and dirty content makes it modified")
    {
        submodule_sources sources { sources_of("submodule_status_clean.bin") };
        std::vector<submodule_info> list { assemble_submodules(sources, nullptr) };
        CHECK(find_submodule(list, sub_a)->state == submodule_state::initialized);
        CHECK(find_submodule(list, sub_a)->description == "heads/main");

        file_change dirty {};
        dirty.path = sub_a;
        dirty.is_submodule = true;
        dirty.submodule_flags = "S.M.";
        sources.changes.push_back(dirty);
        list = assemble_submodules(sources, nullptr);
        CHECK(find_submodule(list, sub_a)->state == submodule_state::modified);
        CHECK(find_submodule(list, sub_b)->state == submodule_state::initialized);

        // 커밋만 바뀐 표시(`SC..`)는 status 접두어가 이미 말한다. 내용 변경이 아니다.
        sources.changes.front().submodule_flags = "SC..";
        list = assemble_submodules(sources, nullptr);
        CHECK(find_submodule(list, sub_a)->state == submodule_state::initialized);
    }

    SECTION("a conflicted submodule has no checked out commit")
    {
        const std::vector<submodule_info> list { assemble_submodules(sources_of("submodule_status_conflict.bin"), nullptr) };
        const submodule_info& conflicted { *find_submodule(list, sub_b) };
        CHECK(conflicted.state == submodule_state::conflict);
        CHECK(conflicted.checked_out_sha.empty());
        CHECK(find_submodule(list, sub_a)->state == submodule_state::uninitialized);
    }

    SECTION("nested submodules know their depth and have no url")
    {
        submodule_sources sources {};
        sources.status = parse_git_submodule_status(read_fixture("submodule_status_nested.bin"));
        const std::vector<submodule_info> list { assemble_submodules(sources, nullptr) };
        REQUIRE(list.size() == 2);
        CHECK(list[0].depth == 0);
        CHECK(list[1].depth == 1);
        CHECK(list[1].url.empty());
        // 일치 상태의 기록은 곧 체크아웃이다.
        CHECK(list[1].recorded_sha == list[1].checked_out_sha);
    }
}

TEST_CASE("submodule pointer changes appear as gitlink modes in the diff", "[git][submodule][diff]")
{
    const std::vector<diff_file> files { parse_git_diff(read_fixture("diff_gitlink.patch")) };
    REQUIRE(files.size() == 1);
    CHECK(files[0].new_path == sub_a);
    CHECK(files[0].old_mode == "160000");
    CHECK(files[0].new_mode == "160000");
    CHECK_FALSE(files[0].is_binary);
    REQUIRE(files[0].hunks.size() == 1);
    CHECK(files[0].hunks[0].lines.front().content == "Subproject commit 1ecfd40f48933961e1d368341744a076db579db3");

    const std::vector<diff_file> dirty { parse_git_diff(read_fixture("diff_gitlink_dirty.patch")) };
    REQUIRE(dirty.size() == 1);
    CHECK(dirty[0].hunks[0].lines.back().content == "Subproject commit 9b8895860a444517b36dd491c64f404084088883-dirty");
}

TEST_CASE("submodule command lines", "[git][submodule][command]")
{
    CHECK(to_text(make_git_submodule_status_args()) == std::vector<std::string> { "submodule", "status", "--recursive" });
    CHECK(to_text(make_git_gitmodules_config_args()) == std::vector<std::string> { "config", "-f", ".gitmodules", "-z", "--get-regexp", "^submodule[.]" });
    const std::vector<std::string> paths { sub_a, sub_b };
    CHECK(to_text(make_git_gitlinks_args(paths)) == std::vector<std::string> { "--literal-pathspecs", "ls-files", "--stage", "-z", "--", sub_a, sub_b });
    CHECK_THROWS_AS(make_git_gitlinks_args({}), std::invalid_argument);

    git_submodule_update_options options {};
    CHECK(to_text(make_git_submodule_update_args(options)) == std::vector<std::string> { "submodule", "update", "--init", "--recursive", "--progress" });
    options.init = false;
    options.recursive = false;
    options.paths = { sub_a };
    CHECK(to_text(make_git_submodule_update_args(options)) == std::vector<std::string> { "submodule", "update", "--progress", "--", sub_a });
    CHECK(to_text(make_git_submodule_sync_args(true, {})) == std::vector<std::string> { "submodule", "sync", "--recursive" });
    CHECK_THROWS_AS(make_git_submodule_update_args(git_submodule_update_options { .paths = { std::string {} } }), std::invalid_argument);
}

// ---------------------------------------------------------------- worktree

TEST_CASE("parse_git_worktree_list reads branch, detached, locked and prunable entries", "[git][worktree]")
{
    const std::vector<worktree_info> list { parse_git_worktree_list(read_fixture("worktree_list.bin")) };
    REQUIRE(list.size() == 4);

    CHECK(list[0].path == "C:/work/super");
    CHECK(list[0].is_main);
    CHECK(list[0].branch == "main");
    CHECK(list[0].branch_ref == "refs/heads/main");
    CHECK(list[0].head_sha == "dda462266603e4feed5a7d63f0f3c14156c88402");
    CHECK_FALSE(list[0].locked);

    CHECK_FALSE(list[1].is_main);
    CHECK(list[1].branch == "feat");

    CHECK(list[2].detached);
    CHECK(list[2].branch.empty());
    CHECK(list[2].prunable);
    CHECK(list[2].prunable_reason == "gitdir file points to non-existent location");

    CHECK(list[3].branch == "newb");
    CHECK(list[3].locked);
    CHECK(list[3].lock_reason == "usb");
}

TEST_CASE("parse_git_worktree_list reads a bare repository and tolerates CRLF and unknown lines", "[git][worktree]")
{
    const std::vector<worktree_info> bare { parse_git_worktree_list(read_fixture("worktree_list_bare.bin")) };
    REQUIRE(bare.size() == 2);
    CHECK(bare[0].bare);
    CHECK(bare[0].is_main);
    CHECK(bare[1].branch == "main");

    const std::string text { "worktree C:/a b\r\nHEAD 1111111111111111111111111111111111111111\r\nfuture-key value\r\nlocked\r\n\r\nworktree C:/c\r\nbranch refs/heads/x/y\r\n" };
    const std::vector<worktree_info> list { parse_git_worktree_list(text) };
    REQUIRE(list.size() == 2);
    CHECK(list[0].path == "C:/a b");
    CHECK(list[0].locked);
    CHECK(list[0].lock_reason.empty());
    CHECK(list[1].branch == "x/y");
    CHECK(parse_git_worktree_list({}).empty());
}

TEST_CASE("apply_worktree_branches marks local branches checked out in other worktrees", "[git][worktree]")
{
    const std::vector<worktree_info> list { parse_git_worktree_list(read_fixture("worktree_list.bin")) };
    std::vector<ref> refs {};
    for (const char* const name : { "main", "feat", "newb", "other" })
    {
        ref value {};
        value.kind = ref_kind::local;
        value.name = name;
        value.full_name = std::string { "refs/heads/" } + name;
        refs.push_back(value);
    }
    ref remote {};
    remote.kind = ref_kind::remote;
    remote.name = "origin/feat";
    remote.full_name = "refs/remotes/origin/feat";
    refs.push_back(remote);

    // 현재 탭이 주 작업 트리(대소문자와 구분자가 달라도 같은 키)다.
    apply_worktree_branches(refs, list, u8"c:/work/super");
    CHECK(refs[0].worktree_path.empty());
    CHECK(refs[1].worktree_path == "C:/work/wt1");
    CHECK(refs[2].worktree_path == "C:/work/wt3");
    CHECK(refs[3].worktree_path.empty());
    CHECK(refs[4].worktree_path.empty());

    // wt1을 연 탭이면 main이 다른 worktree에 있고 feat는 자기 것이다.
    for (ref& value : refs)
        value.worktree_path.clear();
    apply_worktree_branches(refs, list, u8"c:/work/wt1");
    CHECK(refs[0].worktree_path == "C:/work/super");
    CHECK(refs[1].worktree_path.empty());
}

TEST_CASE("worktree command lines", "[git][worktree][command]")
{
    CHECK(to_text(make_git_worktree_list_args()) == std::vector<std::string> { "worktree", "list", "--porcelain" });
    CHECK(to_text(make_git_worktree_prune_args()) == std::vector<std::string> { "worktree", "prune", "--verbose" });

    git_worktree_add_options existing { .path = "C:/w/wt", .branch = "feat" };
    CHECK(to_text(make_git_worktree_add_args(existing)) == std::vector<std::string> { "worktree", "add", "--", "C:/w/wt", "feat" });

    git_worktree_add_options created { .path = "C:/w/wt", .new_branch = "topic", .start_point = "abc1234" };
    CHECK(to_text(make_git_worktree_add_args(created)) == std::vector<std::string> { "worktree", "add", "-b", "topic", "--", "C:/w/wt", "abc1234" });
    created.start_point.clear();
    CHECK(to_text(make_git_worktree_add_args(created)) == std::vector<std::string> { "worktree", "add", "-b", "topic", "--", "C:/w/wt" });

    git_worktree_add_options detached { .path = "C:/w/wt", .start_point = "v1.0", .detach = true };
    CHECK(to_text(make_git_worktree_add_args(detached)) == std::vector<std::string> { "worktree", "add", "--detach", "--", "C:/w/wt", "v1.0" });

    CHECK_THROWS_AS(make_git_worktree_add_args(git_worktree_add_options { .path = "C:/w/wt", .new_branch = "-bad" }), std::invalid_argument);
    CHECK_THROWS_AS(make_git_worktree_add_args(git_worktree_add_options { .path = "", .branch = "feat" }), std::invalid_argument);
    CHECK_THROWS_AS(make_git_worktree_add_args(git_worktree_add_options { .path = "C:/w/wt", .branch = "--force" }), std::invalid_argument);

    CHECK(to_text(make_git_worktree_remove_args("C:/w/wt", false, false)) == std::vector<std::string> { "worktree", "remove", "--", "C:/w/wt" });
    CHECK(to_text(make_git_worktree_remove_args("C:/w/wt", true, false)) == std::vector<std::string> { "worktree", "remove", "--force", "--", "C:/w/wt" });
    // 잠긴 작업 트리는 `--force`가 두 번이어야 지워진다.
    CHECK(to_text(make_git_worktree_remove_args("C:/w/wt", true, true)) == std::vector<std::string> { "worktree", "remove", "--force", "--force", "--", "C:/w/wt" });
}

TEST_CASE("the classifier recognizes a branch checked out in another worktree", "[git][worktree][error]")
{
    const std::string error { "fatal: 'feat' is already used by worktree at 'C:/work/wt1'\n" };
    CHECK(classify_git_failure(error) == git_failure_kind::branch_checked_out_elsewhere);
    CHECK(find_checked_out_worktree_path(error) == std::optional<std::string> { "C:/work/wt1" });

    // 옛 git의 문구다.
    const std::string legacy { "fatal: 'feat' is already checked out at '/tmp/it's here'\n" };
    CHECK(classify_git_failure(legacy) == git_failure_kind::branch_checked_out_elsewhere);
    CHECK(find_checked_out_worktree_path(legacy) == std::optional<std::string> { "/tmp/it's here" });

    CHECK_FALSE(find_checked_out_worktree_path("fatal: not a git repository").has_value());
    CHECK(classify_git_failure("fatal: invalid reference: nope\n") == git_failure_kind::unclassified);
}

// ---------------------------------------------------------------- Git LFS

TEST_CASE("parse_lfs_pointer reads the three pointer lines", "[git][lfs]")
{
    const std::optional<lfs_pointer> pointer { parse_lfs_pointer(read_fixture("lfs_pointer.bin")) };
    REQUIRE(pointer.has_value());
    CHECK(pointer->oid == "684020d93350806eaa61d3ce806e6d71c16f32e05093973b664795c285fb4e1f");
    CHECK(pointer->size == 5000);

    const std::string crlf { "version https://git-lfs.github.com/spec/v1\r\noid sha256:684020d93350806eaa61d3ce806e6d71c16f32e05093973b664795c285fb4e1f\r\nsize 5000\r\n" };
    CHECK(parse_lfs_pointer(crlf) == pointer);
}

TEST_CASE("parse_lfs_pointer rejects text that only looks like a pointer", "[git][lfs]")
{
    const std::string oid { "684020d93350806eaa61d3ce806e6d71c16f32e05093973b664795c285fb4e1f" };
    CHECK_FALSE(parse_lfs_pointer("").has_value());
    CHECK_FALSE(parse_lfs_pointer("hello\nworld\n").has_value());
    CHECK_FALSE(parse_lfs_pointer("version https://git-lfs.github.com/spec/v1\noid sha256:" + oid + "\n").has_value());
    CHECK_FALSE(parse_lfs_pointer("version https://git-lfs.github.com/spec/v1\nsize 12\n").has_value());
    CHECK_FALSE(parse_lfs_pointer("version https://git-lfs.github.com/spec/v1\noid sha256:xyz\nsize 12\n").has_value());
    CHECK_FALSE(parse_lfs_pointer("version https://git-lfs.github.com/spec/v1\noid sha256:" + oid + "\nsize 12abc\n").has_value());
    CHECK_FALSE(parse_lfs_pointer("version https://git-lfs.github.com/spec/v1\noid sha256:" + oid + "\nsize 99999999999999999999999\n").has_value());
    // 포인터 파일은 1 KiB보다 작다.
    CHECK_FALSE(parse_lfs_pointer("version https://git-lfs.github.com/spec/v1\noid sha256:" + oid + "\nsize 12\n" + std::string(2000, 'x')).has_value());
    // 확장 줄이 더 있어도 필수 줄이 있으면 포인터다.
    CHECK(parse_lfs_pointer("version https://git-lfs.github.com/spec/v1\next-0-foo sha256:" + oid + "\noid sha256:" + oid + "\nsize 12\n").has_value());
}

TEST_CASE("git lfs ls-files output is read from json and from the long format", "[git][lfs]")
{
    const std::optional<std::vector<lfs_file>> json { parse_git_lfs_files_json(read_fixture("lfs_ls_files.json.bin")) };
    REQUIRE(json.has_value());
    REQUIRE(json->size() == 3);
    CHECK((*json)[0].path == "b2.bin");
    CHECK((*json)[0].size == 3000000);
    CHECK((*json)[0].downloaded);
    CHECK((*json)[0].checked_out);
    CHECK((*json)[2].oid == "684020d93350806eaa61d3ce806e6d71c16f32e05093973b664795c285fb4e1f");

    // 받았지만 작업 트리에는 포인터만 있는 복제본이다.
    const std::optional<std::vector<lfs_file>> pointers { parse_git_lfs_files_json(read_fixture("lfs_ls_files_missing.json.bin")) };
    REQUIRE(pointers.has_value());
    REQUIRE(pointers->size() == 3);
    CHECK((*pointers)[0].downloaded);
    CHECK_FALSE((*pointers)[0].checked_out);

    CHECK_FALSE(parse_git_lfs_files_json("not json").has_value());
    CHECK_FALSE(parse_git_lfs_files_json("{}").has_value());
    const std::optional<std::vector<lfs_file>> empty { parse_git_lfs_files_json(R"({"files": null})") };
    REQUIRE(empty.has_value());
    CHECK(empty->empty());

    const std::vector<lfs_file> long_form { parse_git_lfs_files_long(read_fixture("lfs_ls_files_long.bin")) };
    REQUIRE(long_form.size() == 3);
    CHECK(long_form[0].path == "b2.bin");
    CHECK(long_form[0].checked_out);
    const std::vector<lfs_file> long_missing { parse_git_lfs_files_long(read_fixture("lfs_ls_files_long_missing.bin")) };
    REQUIRE(long_missing.size() == 3);
    CHECK_FALSE(long_missing[0].checked_out);
    CHECK(parse_git_lfs_files_long("junk\n684020d93350 * short\n").empty());
}

TEST_CASE("git lfs support helpers", "[git][lfs]")
{
    CHECK(parse_git_lfs_version(read_fixture("lfs_version.bin")) == std::optional<std::string> { "git-lfs/3.7.1" });
    CHECK_FALSE(parse_git_lfs_version("git: 'lfs' is not a git command.").has_value());

    const std::set<std::string> tracked { parse_git_check_attr_lfs_paths(read_fixture("lfs_check_attr.bin")) };
    CHECK(tracked == std::set<std::string> { "big.bin", "sub/c.bin" });

    CHECK(gitattributes_uses_lfs("*.bin filter=lfs diff=lfs merge=lfs -text\n"));
    CHECK(gitattributes_uses_lfs("# header\n*.txt text\r\n*.psd -text filter=lfs\r\n"));
    CHECK_FALSE(gitattributes_uses_lfs("# *.bin filter=lfs\n*.txt text\n"));
    CHECK_FALSE(gitattributes_uses_lfs("*.bin filter=other\n"));
    CHECK_FALSE(gitattributes_uses_lfs(""));

    CHECK(lfs_object_relative_path("684020d93350806eaa61d3ce806e6d71c16f32e05093973b664795c285fb4e1f") == "lfs/objects/68/40/684020d93350806eaa61d3ce806e6d71c16f32e05093973b664795c285fb4e1f");
    CHECK(lfs_object_relative_path("../../x").empty());

    CHECK(format_byte_size(0) == "0 B");
    CHECK(format_byte_size(999) == "999 B");
    CHECK(format_byte_size(5000) == "5.0 KB");
    CHECK(format_byte_size(3000000) == "3.0 MB");
    CHECK(format_byte_size(1500000000) == "1.5 GB");
    CHECK(format_byte_size(999999) == "1.0 MB");
}

TEST_CASE("an LFS pointer shows up as an ordinary three line text diff", "[git][lfs][diff]")
{
    const std::vector<diff_file> files { parse_git_diff(read_fixture("diff_lfs_pointer.patch")) };
    REQUIRE(files.size() == 1);
    CHECK_FALSE(files[0].is_binary);
    CHECK(files[0].lines_added == 2);
    REQUIRE(files[0].hunks.size() == 1);
    CHECK(files[0].hunks[0].lines.size() == 5);
}

TEST_CASE("git lfs command lines", "[git][lfs][command]")
{
    CHECK(to_text(make_git_lfs_version_args()) == std::vector<std::string> { "lfs", "version" });
    CHECK(to_text(make_git_lfs_ls_files_json_args()) == std::vector<std::string> { "lfs", "ls-files", "--json" });
    CHECK(to_text(make_git_lfs_pull_args()) == std::vector<std::string> { "lfs", "pull" });
    CHECK(to_text(make_git_lfs_fetch_args(false)) == std::vector<std::string> { "lfs", "fetch" });
    CHECK(to_text(make_git_lfs_fetch_args(true)) == std::vector<std::string> { "lfs", "fetch", "--all" });
    CHECK(to_text(make_git_lfs_install_local_args()) == std::vector<std::string> { "lfs", "install", "--local" });
    CHECK(to_text(make_git_lfs_track_args("*.psd")) == std::vector<std::string> { "lfs", "track", "--", "*.psd" });
    CHECK(to_text(make_git_check_attr_filter_args()) == std::vector<std::string> { "check-attr", "-z", "--stdin", "filter" });
    CHECK_THROWS_AS(make_git_lfs_track_args(""), std::invalid_argument);
    CHECK_THROWS_AS(make_git_lfs_track_args("*.a\n*.b"), std::invalid_argument);
}

TEST_CASE("the progress parser reads git lfs progress lines", "[git][lfs][progress]")
{
    const std::u8string first { u8"Downloading LFS objects:   0% (0/1), 0 B | 0 B/s                                " };
    const std::optional<git_progress> zero { parse_git_progress(first) };
    REQUIRE(zero.has_value());
    CHECK(zero->phase_text == u8"Downloading LFS objects");
    REQUIRE(zero->fraction.has_value());
    CHECK(*zero->fraction == 0.0);

    const std::optional<git_progress> third { parse_git_progress(u8"Downloading LFS objects:  33% (1/3), 0 B | 0 B/s") };
    REQUIRE(third.has_value());
    CHECK(*third->fraction == 1.0 / 3.0);

    const std::optional<git_progress> done { parse_git_progress(u8"Uploading LFS objects: 100% (3/3), 3.0 MB | 1.2 MB/s, done.") };
    REQUIRE(done.has_value());
    CHECK(done->phase_text == u8"Uploading LFS objects");
    CHECK(*done->fraction == 1.0);

    const std::optional<git_progress> checkout { parse_git_progress(u8"Checking out LFS objects:  50% (1/2), 1.5 MB | 0 B/s") };
    REQUIRE(checkout.has_value());
    CHECK(*checkout->fraction == 0.5);

    // 같은 출력의 마지막 줄은 진행이 아니다.
    CHECK_FALSE(parse_git_progress(u8"Fetching reference refs/heads/main").has_value());
}
