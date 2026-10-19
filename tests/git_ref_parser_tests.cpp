#include "infrastructure/git_ref_parser.h"

#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace {
    using cyella::git_ref_list;
    using cyella::parse_git_refs;
    using cyella::ref;
    using cyella::ref_kind;

    const ref* find_ref(const git_ref_list& list, const std::string_view full_name)
    {
        for (const ref& entry : list.refs)
            if (entry.full_name == full_name)
                return &entry;
        return nullptr;
    }

    // 필드를 0x1F로 이은 한 레코드를 만든다.
    std::string record(const std::initializer_list<std::string_view> fields)
    {
        std::string bytes {};
        bool first { true };
        for (const std::string_view field : fields)
        {
            if (first == false)
                bytes.push_back('\x1F');
            first = false;
            bytes.append(field);
        }
        bytes.push_back('\n');
        return bytes;
    }
} // namespace

TEST_CASE("parse_git_refs splits local remote and tag refs", "[git][ref]")
{
    const git_ref_list list { parse_git_refs(cyella::test::read_fixture("for_each_ref.bin")) };

    // 원격 HEAD symref는 목록에 넣지 않는다: 3 local + 2 remote + 2 tag
    CHECK(list.refs.size() == 7);
    CHECK(list.skipped_count == 0);
    CHECK(find_ref(list, "refs/remotes/origin/HEAD") == nullptr);

    const ref* feature { find_ref(list, "refs/heads/feature") };
    REQUIRE(feature != nullptr);
    CHECK(feature->kind == ref_kind::local);
    CHECK(feature->name == "feature");
    CHECK(feature->target_sha == "2d0b877cfe6e7db2eba8c62f8b9db4994d21bd47");
    CHECK(feature->upstream == "origin/feature");
    CHECK_FALSE(feature->is_current);

    const ref* remote { find_ref(list, "refs/remotes/origin/main") };
    REQUIRE(remote != nullptr);
    CHECK(remote->kind == ref_kind::remote);
    CHECK(remote->name == "origin/main");
}

TEST_CASE("parse_git_refs reads ahead behind and the current branch", "[git][ref]")
{
    const git_ref_list list { parse_git_refs(cyella::test::read_fixture("for_each_ref.bin")) };

    const ref* main_branch { find_ref(list, "refs/heads/main") };
    REQUIRE(main_branch != nullptr);
    CHECK(main_branch->is_current);
    CHECK(main_branch->upstream == "origin/main");
    CHECK(main_branch->ahead == 2);
    CHECK(main_branch->behind == 1);
    CHECK_FALSE(main_branch->upstream_gone);

    const ref* feature { find_ref(list, "refs/heads/feature") };
    REQUIRE(feature != nullptr);
    CHECK(feature->ahead == 0);
    CHECK(feature->behind == 0);
}

TEST_CASE("parse_git_refs detects a gone upstream from the missing tracking ref", "[git][ref]")
{
    const git_ref_list list { parse_git_refs(cyella::test::read_fixture("for_each_ref.bin")) };

    const ref* gone { find_ref(list, "refs/heads/gone-branch") };
    REQUIRE(gone != nullptr);
    CHECK(gone->upstream == "origin/stale");
    CHECK(gone->upstream_gone);
    CHECK(gone->ahead == 0);
}

TEST_CASE("parse_git_refs detects a gone upstream even when the track text is localized", "[git][ref]")
{
    // 추적 ref(refs/remotes/origin/gone)가 목록에 없고 track 문구는 알 수 없는 언어다.
    const std::string bytes { record({ "refs/heads/topic", "aaaa", "", "*", "refs/remotes/origin/gone", "origin/gone", "[desaparecido]", "" }) };
    const git_ref_list list { parse_git_refs(bytes) };

    REQUIRE(list.refs.size() == 1);
    CHECK(list.refs[0].upstream_gone);
}

TEST_CASE("parse_git_refs reports the commit an annotated tag points at", "[git][ref]")
{
    const git_ref_list list { parse_git_refs(cyella::test::read_fixture("for_each_ref.bin")) };

    const ref* lightweight { find_ref(list, "refs/tags/v1.0") };
    REQUIRE(lightweight != nullptr);
    CHECK(lightweight->kind == ref_kind::tag);
    CHECK_FALSE(lightweight->is_annotated_tag);
    CHECK(lightweight->target_sha == "c1bf80d6a7260e533f88e475f0d4c611194766c2");

    const ref* annotated { find_ref(list, "refs/tags/v2.0") };
    REQUIRE(annotated != nullptr);
    CHECK(annotated->name == "v2.0");
    CHECK(annotated->is_annotated_tag);
    // 태그 객체(a71ad86...)가 아니라 그것이 가리키는 커밋이다.
    CHECK(annotated->target_sha == "c1bf80d6a7260e533f88e475f0d4c611194766c2");
    CHECK(annotated->remotes.empty());
}

TEST_CASE("parse_git_refs reports the remote default branch separately", "[git][ref]")
{
    const git_ref_list list { parse_git_refs(cyella::test::read_fixture("for_each_ref.bin")) };

    REQUIRE(list.remote_defaults.size() == 1);
    CHECK(list.remote_defaults[0].remote == "origin");
    CHECK(list.remote_defaults[0].branch == "main");
}

TEST_CASE("parse_git_refs handles remotes whose names contain slashes", "[git][ref]")
{
    const std::string bytes { record({ "refs/remotes/team/fork/HEAD", "aaaa", "", " ", "", "", "", "refs/remotes/team/fork/dev/x" }) };
    const git_ref_list list { parse_git_refs(bytes) };

    CHECK(list.refs.empty());
    REQUIRE(list.remote_defaults.size() == 1);
    CHECK(list.remote_defaults[0].remote == "team/fork");
    CHECK(list.remote_defaults[0].branch == "dev/x");
}

TEST_CASE("parse_git_refs accepts NUL terminated records and skips foreign refs", "[git][ref]")
{
    std::string bytes { record({ "refs/heads/a", "aaaa", "", " ", "", "", "", "" }) };
    bytes.back() = '\0';
    bytes.push_back('\n');
    bytes.append(record({ "refs/notes/commits", "bbbb", "", " ", "", "", "", "" }));
    bytes.append("too\x1Fshort\n");
    bytes.append(record({ "refs/stash", "cccc", "", " ", "", "", "", "" }));

    const git_ref_list list { parse_git_refs(bytes) };
    REQUIRE(list.refs.size() == 2);
    CHECK(list.refs[0].name == "a");
    CHECK(list.refs[1].kind == ref_kind::stash);
    CHECK(list.skipped_count == 2);
}

TEST_CASE("parse_git_refs handles empty input", "[git][ref]")
{
    const git_ref_list list { parse_git_refs({}) };
    CHECK(list.refs.empty());
    CHECK(list.remote_defaults.empty());
}

TEST_CASE("parse_ls_remote_tags and apply_remote_tags mark tags that exist on the remote", "[git][ref]")
{
    git_ref_list list { parse_git_refs(cyella::test::read_fixture("for_each_ref.bin")) };
    const std::vector<cyella::remote_tag> remote_tags { cyella::parse_ls_remote_tags(cyella::test::read_fixture("ls_remote_tags.bin")) };

    REQUIRE(remote_tags.size() == 1);
    CHECK(remote_tags[0].name == "v1.0");
    CHECK(remote_tags[0].sha == "c1bf80d6a7260e533f88e475f0d4c611194766c2");

    cyella::apply_remote_tags(list.refs, "origin", remote_tags);
    CHECK(find_ref(list, "refs/tags/v1.0")->remotes == std::vector<std::string> { "origin" });
    CHECK(find_ref(list, "refs/tags/v2.0")->remotes.empty());
    // 태그가 아닌 ref는 건드리지 않는다.
    CHECK(find_ref(list, "refs/heads/main")->remotes.empty());

    // 다른 원격의 결과는 따로 합쳐지고, 원격에서 사라지면 그 원격만 빠진다.
    cyella::apply_remote_tags(list.refs, "upstream", remote_tags);
    CHECK(find_ref(list, "refs/tags/v1.0")->remotes == std::vector<std::string> { "origin", "upstream" });
    cyella::apply_remote_tags(list.refs, "origin", {});
    CHECK(find_ref(list, "refs/tags/v1.0")->remotes == std::vector<std::string> { "upstream" });
}

TEST_CASE("parse_ls_remote_tags prefers the peeled commit of annotated tags", "[git][ref]")
{
    const std::string bytes = "1111111111111111111111111111111111111111\trefs/tags/v2.0\n"
                              "2222222222222222222222222222222222222222\trefs/tags/v2.0^{}\n"
                              "3333333333333333333333333333333333333333\trefs/tags/v3.0\n"
                              "4444444444444444444444444444444444444444\trefs/heads/main\n";
    const std::vector<cyella::remote_tag> tags { cyella::parse_ls_remote_tags(bytes) };

    REQUIRE(tags.size() == 2);
    CHECK(tags[0].name == "v2.0");
    CHECK(tags[0].sha == "2222222222222222222222222222222222222222");
    CHECK(tags[1].name == "v3.0");
}
