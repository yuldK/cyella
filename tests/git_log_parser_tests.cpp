#include "infrastructure/git_log_parser.h"

#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using cyella::commit_info;
    using cyella::git_log_parser;
    using cyella::parse_git_log;

    // 모든 필드를 한 줄로 요약해, 두 결과가 같은지 한 번에 비교한다.
    std::string describe(const commit_info& commit)
    {
        std::string text { commit.sha };
        for (const std::string& parent : commit.parents)
            text.append(" <").append(parent).append(">");
        text.append(" | ").append(commit.author_name).append(" | ").append(commit.author_email);
        text.append(" | ").append(std::to_string(commit.author_time)).append(" | ").append(std::to_string(commit.committer_time));
        text.append(" | ").append(commit.subject);
        if (commit.body.has_value())
            text.append(" | body=").append(*commit.body);
        return text;
    }

    std::vector<std::string> describe_all(const std::vector<commit_info>& commits)
    {
        std::vector<std::string> lines {};
        lines.reserve(commits.size());
        for (const commit_info& commit : commits)
            lines.push_back(describe(commit));
        return lines;
    }

    // chunks가 정한 크기로 잘라 feed한다.
    std::vector<commit_info> parse_in_chunks(const std::string_view bytes, const bool has_body, const std::vector<std::size_t>& sizes)
    {
        std::vector<commit_info> commits {};
        git_log_parser parser { [&commits](commit_info&& commit) { commits.push_back(std::move(commit)); }, has_body };
        std::size_t position { 0 };
        std::size_t turn { 0 };
        while (position < bytes.size())
        {
            const std::size_t size { std::min(sizes[turn++ % sizes.size()], bytes.size() - position) };
            parser.feed(bytes.substr(position, size));
            position += size;
        }
        parser.finish();
        REQUIRE(parser.malformed_record_count() == 0);
        REQUIRE(parser.commit_count() == commits.size());
        return commits;
    }
} // namespace

TEST_CASE("parse_git_log reads commits in git's order", "[git][log]")
{
    const std::vector<commit_info> commits { parse_git_log(cyella::test::read_fixture("log.bin")) };

    REQUIRE(commits.size() == 5);
    CHECK(commits[0].subject == "other side commit");
    CHECK(commits[0].sha == "b0e5395bb8548964e185a8580d1971ec6b7c3ccc");
    CHECK(commits[0].parents == std::vector<std::string> { "4d3a07aa88350bcda88bb4c4d96c9beb761f5909" });
    CHECK(commits[0].author_name == "Ann Author");
    CHECK(commits[0].author_email == "ann@example.com");
    CHECK(commits[0].author_time == 1767661200);
    CHECK(commits[0].committer_time == 1767661200);
    CHECK_FALSE(commits[0].body.has_value());
}

TEST_CASE("parse_git_log reads merge parents and the root commit", "[git][log]")
{
    const std::vector<commit_info> commits { parse_git_log(cyella::test::read_fixture("log.bin")) };

    REQUIRE(commits.size() == 5);
    const commit_info& merge { commits[1] };
    CHECK(merge.subject == "Merge branch 'feature'");
    REQUIRE(merge.parents.size() == 2);
    CHECK(merge.parents[0] == "4d3a07aa88350bcda88bb4c4d96c9beb761f5909");
    CHECK(merge.parents[1] == "2d0b877cfe6e7db2eba8c62f8b9db4994d21bd47");

    const commit_info& root { commits[4] };
    CHECK(root.subject == "initial commit");
    CHECK(root.parents.empty());
}

TEST_CASE("parse_git_log keeps non-ASCII subjects byte for byte", "[git][log]")
{
    const std::vector<commit_info> commits { parse_git_log(cyella::test::read_fixture("log.bin")) };

    REQUIRE(commits.size() == 5);
    CHECK(commits[3].subject == "main change \xED\x95\x9C\xEA\xB8\x80 \xEC\xA0\x9C\xEB\xAA\xA9 \xE2\x9C\x93");
}

TEST_CASE("parse_git_log reads bodies when requested", "[git][log]")
{
    const std::vector<commit_info> commits { parse_git_log(cyella::test::read_fixture("log_body.bin"), true) };

    REQUIRE(commits.size() == 5);
    REQUIRE(commits[2].body.has_value());
    CHECK(commits[2].subject == "feat: add f");
    CHECK(*commits[2].body == "Body line 1\nBody line 2\n\n- bullet\n");
    // 본문이 없는 커밋은 값은 있되 비어 있다.
    REQUIRE(commits[0].body.has_value());
    CHECK(commits[0].body->empty());
}

TEST_CASE("parse_git_log reads a paged log", "[git][log]")
{
    const std::vector<commit_info> commits { parse_git_log(cyella::test::read_fixture("log_paged.bin")) };

    REQUIRE(commits.size() == 2);
    CHECK(commits[0].subject == "Merge branch 'feature'");
    CHECK(commits[1].subject == "feat: add f");
}

TEST_CASE("parse_git_log counts malformed records without losing the good ones", "[git][log]")
{
    const std::string good { cyella::test::read_fixture("log.bin") };
    std::string bytes { "not a commit record" };
    bytes.push_back('\0');
    bytes.append(good);

    std::vector<commit_info> commits {};
    git_log_parser parser { [&commits](commit_info&& commit) { commits.push_back(std::move(commit)); } };
    parser.feed(bytes);
    parser.finish();

    CHECK(commits.size() == 5);
    CHECK(parser.malformed_record_count() == 1);
}

TEST_CASE("git_log_parser accepts a final record without a terminating NUL", "[git][log]")
{
    std::string bytes { cyella::test::read_fixture("log.bin") };
    bytes.pop_back();

    CHECK(parse_git_log(bytes).size() == 5);
}

TEST_CASE("git_log_parser gives the same result for any chunking", "[git][log]")
{
    const std::string bytes { cyella::test::read_fixture("log.bin") };
    const std::vector<std::string> expected { describe_all(parse_git_log(bytes)) };
    REQUIRE(expected.size() == 5);

    SECTION("one byte at a time")
    {
        CHECK(describe_all(parse_in_chunks(bytes, false, { 1 })) == expected);
    }

    SECTION("seven bytes at a time")
    {
        CHECK(describe_all(parse_in_chunks(bytes, false, { 7 })) == expected);
    }

    SECTION("a chunk larger than the input")
    {
        CHECK(describe_all(parse_in_chunks(bytes, false, { bytes.size() + 100 })) == expected);
    }

    SECTION("random chunk sizes, including splits inside UTF-8 sequences")
    {
        std::mt19937 generator { 20260101u };
        std::uniform_int_distribution<std::size_t> distribution { 1, 40 };
        for (int round { 0 }; round < 50; ++round)
        {
            std::vector<std::size_t> sizes {};
            for (int index { 0 }; index < 64; ++index)
                sizes.push_back(distribution(generator));
            CHECK(describe_all(parse_in_chunks(bytes, false, sizes)) == expected);
        }
    }
}

TEST_CASE("git_log_parser gives the same result for any chunking with bodies", "[git][log]")
{
    const std::string bytes { cyella::test::read_fixture("log_body.bin") };
    const std::vector<std::string> expected { describe_all(parse_git_log(bytes, true)) };

    CHECK(describe_all(parse_in_chunks(bytes, true, { 1 })) == expected);
    CHECK(describe_all(parse_in_chunks(bytes, true, { 7 })) == expected);
    CHECK(describe_all(parse_in_chunks(bytes, true, { 3, 11, 1, 29 })) == expected);
}

TEST_CASE("git_log_parser can be reused after finish", "[git][log]")
{
    const std::string bytes { cyella::test::read_fixture("log.bin") };
    std::vector<commit_info> commits {};
    git_log_parser parser { [&commits](commit_info&& commit) { commits.push_back(std::move(commit)); } };

    parser.feed(bytes);
    parser.finish();
    parser.feed(bytes);
    parser.finish();

    CHECK(commits.size() == 10);
}

TEST_CASE("git_log_parser streams 100000 commits without quadratic copying", "[git][log][perf]")
{
    constexpr std::size_t commit_count { 100000 };
    const std::string parent { "4d3a07aa88350bcda88bb4c4d96c9beb761f5909" };

    std::string bytes {};
    bytes.reserve(commit_count * 200);
    for (std::size_t index { 0 }; index < commit_count; ++index)
    {
        std::string sha { std::to_string(index) };
        sha.insert(0, 40 - sha.size(), 'a');
        bytes.append(sha).push_back('\x1F');
        bytes.append(parent).push_back('\x1F');
        bytes.append("Ann Author").push_back('\x1F');
        bytes.append("ann@example.com").push_back('\x1F');
        bytes.append("1767661200").push_back('\x1F');
        bytes.append("1767661200").push_back('\x1F');
        bytes.append("commit subject number ").append(std::to_string(index));
        bytes.push_back('\0');
    }

    std::size_t seen { 0 };
    git_log_parser parser { [&seen](commit_info&&) { ++seen; } };
    const auto begin { std::chrono::steady_clock::now() };
    // 파이프 한 번에 읽는 크기 정도의 조각으로, 레코드 경계와 어긋나게 넣는다.
    for (std::size_t position { 0 }; position < bytes.size(); position += 4093)
        parser.feed(std::string_view { bytes }.substr(position, 4093));
    parser.finish();
    const auto elapsed { std::chrono::steady_clock::now() - begin };

    CHECK(seen == commit_count);
    CHECK(parser.malformed_record_count() == 0);
    // 선형이면 Debug에서도 1초 안쪽이다. 이차 복사라면 수십 초가 걸린다.
    CHECK(elapsed < std::chrono::seconds { 10 });
}

TEST_CASE("git_log_parser copes with one huge record fed byte by byte", "[git][log][perf]")
{
    // 조각마다 꼬리 전체를 다시 찾거나 옮기면 이 입력에서 이차 시간이 걸린다.
    std::string bytes { std::string(40, 'b') };
    bytes.push_back('\x1F');
    bytes.push_back('\x1F');
    bytes.append("n\x1F"
                 "e\x1F"
                 "1\x1F"
                 "2\x1F"
                 "subject\x1F");
    bytes.append(200000, 'x');
    bytes.push_back('\0');

    std::size_t seen { 0 };
    std::size_t body_size { 0 };
    const auto handler = [&](commit_info&& commit) {
        ++seen;
        body_size = commit.body.value_or(std::string {}).size();
    };
    git_log_parser parser { handler, true };
    const auto begin { std::chrono::steady_clock::now() };
    for (const char byte : bytes)
        parser.feed(std::string_view { &byte, 1 });
    parser.finish();
    const auto elapsed { std::chrono::steady_clock::now() - begin };

    CHECK(seen == 1);
    CHECK(body_size == 200000);
    CHECK(elapsed < std::chrono::seconds { 10 });
}
