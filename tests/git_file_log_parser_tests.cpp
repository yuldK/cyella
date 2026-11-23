#include "infrastructure/git_file_log_parser.h"

#include "fake_git_runner.h"
#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using cyella::git_file_log_entry;
    using cyella::git_file_log_parser;
    using cyella::parse_git_file_log;
    using cyella::test::sha_of;

    const std::string first_sha { "44dc65181bf053aec47a2ecbac197fcacf2450f7" };
    const std::string second_sha { "f31445188ab1d83d45f716a1752f2dd36fb516dc" };
    const std::string third_sha { "5ed2b77872588fff0b07af79a85bd001b980b058" };
    const std::string fourth_sha { "283b4124352629f213fdec2cda47fba269d9c8f8" };

    // 모든 필드를 한 줄로 요약해, 두 결과가 같은지 한 번에 비교한다.
    std::string describe(const git_file_log_entry& entry)
    {
        std::string text { entry.commit.sha };
        for (const std::string& parent : entry.commit.parents)
            text.append(" <").append(parent).append(">");
        text.append(" | ").append(entry.commit.author_name).append(" | ").append(entry.commit.author_email);
        text.append(" | ").append(std::to_string(entry.commit.author_time)).append(" | ").append(std::to_string(entry.commit.committer_time));
        text.append(" | ").append(entry.commit.subject).append(" | ");
        text.push_back(entry.status);
        text.append(" | ").append(entry.path).append(" | ").append(entry.previous_path);
        return text;
    }

    std::vector<std::string> describe_all(const std::vector<git_file_log_entry>& entries)
    {
        std::vector<std::string> lines {};
        for (const git_file_log_entry& entry : entries)
            lines.push_back(describe(entry));
        return lines;
    }

    struct parsed_log
    {
        std::vector<git_file_log_entry> entries {};
        std::size_t malformed { 0 };
    };

    // sizes가 정한 크기로 잘라 feed한다.
    parsed_log parse_in_chunks(const std::string_view bytes, const std::string& path, const std::vector<std::size_t>& sizes)
    {
        parsed_log parsed {};
        git_file_log_parser parser { [&parsed](git_file_log_entry&& entry) { parsed.entries.push_back(std::move(entry)); }, path };
        std::size_t position { 0 };
        std::size_t turn { 0 };
        while (position < bytes.size())
        {
            const std::size_t size { std::min(sizes[turn++ % sizes.size()], bytes.size() - position) };
            parser.feed(bytes.substr(position, size));
            position += size;
        }
        parser.finish();
        REQUIRE(parser.entry_count() == parsed.entries.size());
        parsed.malformed = parser.malformed_record_count();
        return parsed;
    }

    parsed_log parse_whole(const std::string_view bytes, const std::string& path)
    {
        return parse_in_chunks(bytes, path, { bytes.size() + 1 });
    }

    // 손으로 만든 레코드의 머리다 (`0x1E <log 필드> NUL`).
    std::string header(const int commit, const std::string& subject = "subject")
    {
        return "\x1E" + cyella::test::log_record(sha_of(commit), commit == 1 ? cyella::test::text_list {} : cyella::test::text_list { sha_of(commit - 1) }, subject);
    }

    // 머리 뒤의 name-status 한 항목이다. 첫 항목만 줄바꿈 뒤에 온다.
    std::string change(const std::string_view status, const std::string_view path, const std::string_view second_path = {}, const bool first = true)
    {
        std::string text { first ? "\n" : "" };
        text.append(status).push_back('\0');
        text.append(path).push_back('\0');
        if (second_path.empty() == false)
            text.append(second_path).push_back('\0');
        return text;
    }
} // namespace

TEST_CASE("parse_git_file_log follows a file across a rename", "[git][file_log]")
{
    const std::vector<git_file_log_entry> entries { parse_git_file_log(cyella::test::read_fixture("file_log_renamed.bin"), "b[12].txt") };

    REQUIRE(entries.size() == 4);
    // 새 커밋이 앞이다.
    CHECK(entries[0].commit.sha == fourth_sha);
    CHECK(entries[0].commit.parents == std::vector<std::string> { third_sha });
    CHECK(entries[0].commit.author_name == "Ann Author");
    CHECK(entries[0].commit.author_email == "ann@example.com");
    CHECK(entries[0].commit.author_time == 1775264400);
    CHECK(entries[0].commit.committer_time == 1775264400);
    CHECK(entries[0].commit.subject == "fourth");
    CHECK_FALSE(entries[0].commit.body.has_value());
    CHECK(entries[0].status == 'M');
    CHECK(entries[0].path == "b[12].txt");
    CHECK(entries[0].previous_path.empty());

    // 이름을 바꾼 커밋은 새 경로에 있고 옛 경로를 함께 준다. 유사도 숫자(`R088`)는 버린다.
    CHECK(entries[1].commit.sha == third_sha);
    CHECK(entries[1].commit.subject == "third: rename");
    CHECK(entries[1].status == 'R');
    CHECK(entries[1].path == "b[12].txt");
    CHECK(entries[1].previous_path == "a[1].txt");

    // 그보다 오래된 커밋은 옛 경로에 있다.
    CHECK(entries[2].commit.sha == second_sha);
    CHECK(entries[2].commit.subject == "second \xED\x95\x9C\xEA\xB8\x80 \xEC\xA0\x9C\xEB\xAA\xA9 \xE2\x9C\x93");
    CHECK(entries[2].status == 'M');
    CHECK(entries[2].path == "a[1].txt");
    CHECK(entries[2].previous_path.empty());

    CHECK(entries[3].commit.sha == first_sha);
    CHECK(entries[3].commit.parents.empty());
    CHECK(entries[3].status == 'A');
    CHECK(entries[3].path == "a[1].txt");
}

TEST_CASE("parse_git_file_log reads a log cut by --max-count", "[git][file_log]")
{
    const parsed_log parsed { parse_whole(cyella::test::read_fixture("file_log_limited.bin"), "b[12].txt") };

    CHECK(parsed.malformed == 0);
    REQUIRE(parsed.entries.size() == 3);
    CHECK(parsed.entries[0].commit.sha == fourth_sha);
    CHECK(parsed.entries[2].commit.sha == second_sha);
    CHECK(parsed.entries[2].path == "a[1].txt");
}

TEST_CASE("parse_git_file_log reads sha256 hashes", "[git][file_log]")
{
    const parsed_log parsed { parse_whole(cyella::test::read_fixture("file_log_sha256.bin"), "g.txt") };

    CHECK(parsed.malformed == 0);
    REQUIRE(parsed.entries.size() == 2);
    CHECK(parsed.entries[0].commit.sha == "0ea81596ac74a737dad2bc54ee41f00c6a33ddea95fef22559f2dd760f0570f5");
    CHECK(parsed.entries[0].commit.parents == std::vector<std::string> { "b3422603fe0fafc5152be945632a8609ddbc7df47e90581b60148407d67fd47a" });
    CHECK(parsed.entries[0].status == 'R');
    CHECK(parsed.entries[0].path == "g.txt");
    CHECK(parsed.entries[0].previous_path == "f.txt");
    CHECK(parsed.entries[1].status == 'A');
    CHECK(parsed.entries[1].path == "f.txt");
}

TEST_CASE("git_file_log_parser gives the same result for any chunking", "[git][file_log]")
{
    const std::string bytes { cyella::test::read_fixture("file_log_renamed.bin") };
    const std::string path { "b[12].txt" };
    const std::vector<std::string> expected { describe_all(parse_git_file_log(bytes, path)) };
    REQUIRE(expected.size() == 4);

    const auto check = [&](const std::vector<std::size_t>& sizes) {
        const parsed_log parsed { parse_in_chunks(bytes, path, sizes) };
        CHECK(parsed.malformed == 0);
        CHECK(describe_all(parsed.entries) == expected);
    };
    check({ 1 });
    check({ 7 });
    check({ bytes.size() + 100 });

    // 머리 한가운데, 상태와 경로 사이, UTF-8 문자 한가운데에서도 끊긴다.
    std::mt19937 generator { 20260401u };
    std::uniform_int_distribution<std::size_t> distribution { 1, 40 };
    for (int round { 0 }; round < 50; ++round)
    {
        std::vector<std::size_t> sizes {};
        for (int index { 0 }; index < 64; ++index)
            sizes.push_back(distribution(generator));
        check(sizes);
    }
}

TEST_CASE("a record without name-status inherits the path the newer entry leads to", "[git][file_log]")
{
    SECTION("after a rename it is the old path")
    {
        const std::string bytes { header(4) + change("R100", "old.txt", "new.txt") + header(3) + header(2) + change("M", "old.txt") + header(1) };
        const parsed_log parsed { parse_whole(bytes, "new.txt") };

        CHECK(parsed.malformed == 0);
        REQUIRE(parsed.entries.size() == 4);
        CHECK(parsed.entries[0].path == "new.txt");
        CHECK(parsed.entries[0].previous_path == "old.txt");
        CHECK(parsed.entries[1].commit.sha == sha_of(3));
        CHECK(parsed.entries[1].path == "old.txt");
        CHECK(parsed.entries[1].previous_path.empty());
        CHECK(parsed.entries[1].status == 'M');
        CHECK(parsed.entries[3].commit.sha == sha_of(1));
        CHECK(parsed.entries[3].path == "old.txt");
    }

    SECTION("the first record falls back to the requested path")
    {
        const parsed_log parsed { parse_whole(header(2) + header(1) + change("A", "a.txt"), "a.txt") };

        REQUIRE(parsed.entries.size() == 2);
        CHECK(parsed.entries[0].path == "a.txt");
        CHECK(parsed.entries[1].status == 'A');
    }

    SECTION("a copy leads to its source")
    {
        const parsed_log parsed { parse_whole(header(2) + change("C075", "source.txt", "copy.txt") + header(1), "copy.txt") };

        REQUIRE(parsed.entries.size() == 2);
        CHECK(parsed.entries[0].status == 'C');
        CHECK(parsed.entries[0].path == "copy.txt");
        CHECK(parsed.entries[0].previous_path == "source.txt");
        CHECK(parsed.entries[1].path == "source.txt");
    }
}

TEST_CASE("paths are taken by position, whatever bytes they hold", "[git][file_log]")
{
    // 경로가 레코드 시작 글자(0x1E)나 줄바꿈으로 시작해도 다음 레코드로 읽지 않는다. 제목의 0x1E도 머리 안에 남는다.
    const std::string odd_path { std::string { "\x1E" } + "odd\nname.txt" };
    const std::string bytes { header(2, "subject with \x1E inside") + change("M", odd_path) + header(1) + change("A", "\nleading newline.txt") };
    const parsed_log parsed { parse_whole(bytes, odd_path) };

    CHECK(parsed.malformed == 0);
    REQUIRE(parsed.entries.size() == 2);
    CHECK(parsed.entries[0].commit.subject == "subject with \x1E inside");
    CHECK(parsed.entries[0].path == odd_path);
    CHECK(parsed.entries[1].path == "\nleading newline.txt");
}

TEST_CASE("only the first name-status entry of a record is used", "[git][file_log]")
{
    const std::string bytes { header(2) + change("M", "a.txt") + change("R090", "b.txt", "c.txt", false) + change("A", "d.txt", {}, false) + header(1) + change("A", "a.txt") };
    const parsed_log parsed { parse_whole(bytes, "a.txt") };

    CHECK(parsed.malformed == 0);
    REQUIRE(parsed.entries.size() == 2);
    CHECK(parsed.entries[0].status == 'M');
    CHECK(parsed.entries[0].path == "a.txt");
    CHECK(parsed.entries[0].previous_path.empty());
    CHECK(parsed.entries[1].commit.sha == sha_of(1));
    CHECK(parsed.entries[1].status == 'A');
}

TEST_CASE("git_file_log_parser counts broken records without losing the good ones", "[git][file_log]")
{
    SECTION("a header that is not a commit record skips to the next header")
    {
        std::string bytes { std::string { "\x1E" } + "not a commit record" };
        bytes.push_back('\0');
        bytes.append(change("M", "ghost.txt")).append(header(1)).append(change("A", "a.txt"));
        const parsed_log parsed { parse_whole(bytes, "a.txt") };

        CHECK(parsed.malformed == 1);
        REQUIRE(parsed.entries.size() == 1);
        CHECK(parsed.entries[0].commit.sha == sha_of(1));
        CHECK(parsed.entries[0].path == "a.txt");
    }

    SECTION("a status that is not a status drops its record")
    {
        const parsed_log parsed { parse_whole(header(2) + change("?", "a.txt") + header(1) + change("A", "a.txt"), "a.txt") };

        CHECK(parsed.malformed == 1);
        REQUIRE(parsed.entries.size() == 1);
        CHECK(parsed.entries[0].commit.sha == sha_of(1));
    }

    SECTION("output cut inside a path does not emit that record")
    {
        const std::string good { cyella::test::read_fixture("file_log_renamed.bin") };
        const parsed_log parsed { parse_whole(std::string_view { good }.substr(0, good.size() - 3), "b[12].txt") };

        CHECK(parsed.malformed == 1);
        CHECK(parsed.entries.size() == 3);
    }

    SECTION("output cut between the two paths of a rename does not emit that record")
    {
        const std::string whole { header(2) + change("M", "new.txt") + header(1) + change("R100", "old.txt", "new.txt") };
        const parsed_log parsed { parse_whole(std::string_view { whole }.substr(0, whole.size() - std::string_view { "new.txt" }.size() - 1), "new.txt") };

        CHECK(parsed.malformed == 1);
        REQUIRE(parsed.entries.size() == 1);
        CHECK(parsed.entries[0].commit.sha == sha_of(2));
    }

    SECTION("output cut inside a header keeps the records before it")
    {
        const std::string whole { header(2) + change("M", "a.txt") + header(1) };
        const parsed_log parsed { parse_whole(std::string_view { whole }.substr(0, whole.size() - 5), "a.txt") };

        CHECK(parsed.malformed == 1);
        REQUIRE(parsed.entries.size() == 1);
        CHECK(parsed.entries[0].commit.sha == sha_of(2));
    }

    SECTION("empty output is an empty history")
    {
        const parsed_log parsed { parse_whole({}, "a.txt") };

        CHECK(parsed.malformed == 0);
        CHECK(parsed.entries.empty());
    }
}

TEST_CASE("git_file_log_parser can be reused after finish", "[git][file_log]")
{
    const std::string bytes { header(2) + change("R100", "old.txt", "new.txt") + header(1) };
    std::vector<git_file_log_entry> entries {};
    git_file_log_parser parser { [&entries](git_file_log_entry&& entry) { entries.push_back(std::move(entry)); }, "new.txt" };

    parser.feed(bytes);
    parser.finish();
    // 둘째 출력의 첫 레코드에 name-status가 없으면 앞 출력이 남긴 옛 경로가 아니라 요청한 경로를 물려받는다.
    parser.feed(header(1));
    parser.finish();

    REQUIRE(entries.size() == 3);
    CHECK(entries[1].path == "old.txt");
    CHECK(entries[2].path == "new.txt");
}
