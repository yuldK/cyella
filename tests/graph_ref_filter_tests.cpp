// 그래프의 브랜치 필터 값과 그 규칙이다 (ADR-012). 없어진 ref 걸러 내기, 현재 브랜치, 켜고 끄기, 사이드바 표시를 본다.
#include "domain/graph_ref_filter.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {
    using namespace cyella;
    using name_list = std::vector<std::string>;

    ref make_ref(const ref_kind kind, const std::string& name, const std::string& full_name)
    {
        ref value {};
        value.kind = kind;
        value.name = name;
        value.full_name = full_name;
        return value;
    }

    std::vector<ref> sample_refs()
    {
        return {
            make_ref(ref_kind::local, "main", "refs/heads/main"),
            make_ref(ref_kind::local, "feature", "refs/heads/feature"),
            make_ref(ref_kind::remote, "origin/main", "refs/remotes/origin/main"),
            make_ref(ref_kind::tag, "v1.0", "refs/tags/v1.0"),
            // 브랜치와 이름이 같은 태그다. 전체 이름으로 들어야 둘이 갈린다.
            make_ref(ref_kind::tag, "feature", "refs/tags/feature"),
        };
    }
} // namespace

TEST_CASE("an empty graph ref filter is empty and filters compare by their two lists", "[graph_filter]")
{
    graph_ref_filter filter {};
    CHECK(filter.empty());
    filter.hidden.push_back("refs/heads/feature");
    CHECK_FALSE(filter.empty());
    CHECK(filter == graph_ref_filter { .only = {}, .hidden = { "refs/heads/feature" } });
    CHECK(filter != graph_ref_filter { .only = { "refs/heads/feature" }, .hidden = {} });
}

TEST_CASE("settling a graph ref filter drops refs that are gone", "[graph_filter]")
{
    const std::vector<ref> refs { sample_refs() };
    graph_ref_filter filter {};
    filter.only = { "refs/heads/gone", "refs/heads/feature", "refs/tags/v0.9" };
    filter.hidden = { "refs/remotes/origin/gone", "refs/tags/v1.0" };

    const graph_ref_filter settled { settle_graph_ref_filter(filter, refs, "main") };
    CHECK(settled.only == name_list { "refs/heads/feature" });
    CHECK(settled.hidden == name_list { "refs/tags/v1.0" });
    // 이미 맞춘 필터는 그대로다. 실행기와 세션이 같은 값에 닿는다.
    CHECK(settle_graph_ref_filter(settled, refs, "main") == settled);

    // "이것만 보기"가 모두 없어지면 그 목록이 빈 필터다. 숨긴 것이 없으면 필터 없음이다.
    graph_ref_filter only_gone {};
    only_gone.only = { "refs/heads/gone", "refs/tags/gone" };
    CHECK(settle_graph_ref_filter(only_gone, refs, "main").empty());

    // 같은 이름의 브랜치와 태그는 따로다.
    graph_ref_filter tag_only {};
    tag_only.hidden = { "refs/tags/feature" };
    CHECK(settle_graph_ref_filter(tag_only, refs, "main").hidden == name_list { "refs/tags/feature" });

    // ref가 하나도 없으면(첫 커밋 전) 남는 것이 없다.
    CHECK(settle_graph_ref_filter(filter, {}, "main").empty());
}

TEST_CASE("the current branch cannot stay hidden but it can be the only one shown", "[graph_filter]")
{
    const std::vector<ref> refs { sample_refs() };
    graph_ref_filter filter {};
    filter.hidden = { "refs/heads/main", "refs/heads/feature", "refs/remotes/origin/main" };

    // 현재 브랜치만 빠진다. 같은 짧은 이름의 원격 브랜치는 다른 ref다.
    CHECK(settle_graph_ref_filter(filter, refs, "main").hidden == name_list { "refs/heads/feature", "refs/remotes/origin/main" });
    CHECK(settle_graph_ref_filter(filter, refs, "feature").hidden == name_list { "refs/heads/main", "refs/remotes/origin/main" });
    // detached HEAD에는 현재 브랜치가 없다.
    CHECK(settle_graph_ref_filter(filter, refs, {}).hidden == filter.hidden);
    // 태그 `feature`는 현재 브랜치 `feature`가 아니다.
    graph_ref_filter tag {};
    tag.hidden = { "refs/tags/feature" };
    CHECK(settle_graph_ref_filter(tag, refs, "feature").hidden == name_list { "refs/tags/feature" });

    graph_ref_filter only {};
    only.only = { "refs/heads/main" };
    CHECK(settle_graph_ref_filter(only, refs, "main").only == name_list { "refs/heads/main" });
}

TEST_CASE("toggling a ref turns it on and off again, and showing only a ref unhides it", "[graph_filter]")
{
    graph_ref_filter filter {};
    toggle_graph_ref_hidden(filter, "refs/heads/feature");
    toggle_graph_ref_hidden(filter, "refs/tags/v1.0");
    CHECK(filter.hidden == name_list { "refs/heads/feature", "refs/tags/v1.0" });
    toggle_graph_ref_hidden(filter, "refs/heads/feature");
    CHECK(filter.hidden == name_list { "refs/tags/v1.0" });

    // 여러 개를 함께 켤 수 있다.
    toggle_graph_ref_only(filter, "refs/heads/main");
    toggle_graph_ref_only(filter, "refs/tags/v1.0");
    CHECK(filter.only == name_list { "refs/heads/main", "refs/tags/v1.0" });
    // 보겠다고 고른 ref는 숨긴 목록에서 빠진다. `only`를 푼 뒤 다시 숨겨지지 않는다.
    CHECK(filter.hidden.empty());

    toggle_graph_ref_only(filter, "refs/heads/main");
    toggle_graph_ref_only(filter, "refs/tags/v1.0");
    CHECK(filter.empty());
}

TEST_CASE("the sidebar mark says what the graph does now: hidden refs are not marked while some are shown alone", "[graph_filter]")
{
    graph_ref_filter filter {};
    filter.hidden = { "refs/heads/feature" };
    CHECK(graph_ref_mark_of(filter, "refs/heads/feature") == graph_ref_mark::hidden);
    CHECK(graph_ref_mark_of(filter, "refs/heads/main") == graph_ref_mark::none);
    // 같은 짧은 이름의 태그는 숨겨지지 않았다.
    CHECK(graph_ref_mark_of(filter, "refs/tags/feature") == graph_ref_mark::none);

    filter.only = { "refs/heads/main" };
    CHECK(graph_ref_mark_of(filter, "refs/heads/main") == graph_ref_mark::only);
    CHECK(graph_ref_mark_of(filter, "refs/heads/feature") == graph_ref_mark::none);
    CHECK(graph_ref_mark_of({}, "refs/heads/main") == graph_ref_mark::none);
}
