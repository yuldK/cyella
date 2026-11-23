#include "infrastructure/git_blame_parser.h"

#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using cyella::blame_line;
    using cyella::blame_origin;
    using cyella::git_blame_parser;
    using cyella::git_blame_result;
    using cyella::parse_git_blame;

    const std::string first_sha { "44dc65181bf053aec47a2ecbac197fcacf2450f7" };
    const std::string second_sha { "f31445188ab1d83d45f716a1752f2dd36fb516dc" };
    const std::string third_sha { "5ed2b77872588fff0b07af79a85bd001b980b058" };
    const std::string fourth_sha { "283b4124352629f213fdec2cda47fba269d9c8f8" };

    // 모든 필드를 한 줄로 요약해, 두 결과가 같은지 한 번에 비교한다.
    std::vector<std::string> describe(const git_blame_result& result)
    {
        std::vector<std::string> lines {};
        for (const blame_origin& origin : result.origins)
        {
            std::string text { "origin " + origin.sha + " | " + origin.path + " | " + origin.author_name + " | " + origin.author_email };
            text.append(" | ").append(std::to_string(origin.author_time)).append(" | ").append(origin.summary);
            text.append(" | ").append(origin.previous_sha).append(" | ").append(origin.previous_path).append(origin.uncommitted ? " | uncommitted" : "");
            lines.push_back(std::move(text));
        }
        for (const blame_line& line : result.lines)
            lines.push_back("line " + std::to_string(line.origin) + " " + std::to_string(line.original_line) + " " + std::to_string(line.final_line) + " " + line.text);
        lines.push_back(result.binary ? "binary" : "text");
        lines.push_back("malformed " + std::to_string(result.malformed_line_count));
        return lines;
    }

    // sizes가 정한 크기로 잘라 feed한다.
    git_blame_result parse_in_chunks(const std::string_view bytes, const std::vector<std::size_t>& sizes)
    {
        git_blame_parser parser {};
        std::size_t position { 0 };
        std::size_t turn { 0 };
        while (position < bytes.size())
        {
            const std::size_t size { std::min(sizes[turn++ % sizes.size()], bytes.size() - position) };
            parser.feed(bytes.substr(position, size));
            position += size;
        }
        return parser.finish();
    }

    std::vector<std::uint32_t> origins_of(const git_blame_result& result)
    {
        std::vector<std::uint32_t> origins {};
        for (const blame_line& line : result.lines)
            origins.push_back(line.origin);
        return origins;
    }

    std::vector<std::string> texts_of(const git_blame_result& result)
    {
        std::vector<std::string> texts {};
        for (const blame_line& line : result.lines)
            texts.push_back(line.text);
        return texts;
    }

    // 손으로 만든 출력의 한 묶음 머리다. 커밋 정보는 그 커밋이 처음 나올 때만 붙인다.
    std::string group_header(const std::string& sha, const int original, const int final_line, const int count, const std::string_view details = {})
    {
        std::string text { sha + " " + std::to_string(original) + " " + std::to_string(final_line) + " " + std::to_string(count) + "\n" };
        text.append(details);
        return text;
    }

    const std::string hand_sha { std::string(40, 'c') };
    constexpr std::string_view hand_details { "author Morgan\nauthor-mail <morgan@example.com>\nauthor-time 1700000000\nauthor-tz +0000\nsummary hand made\n" };
} // namespace

TEST_CASE("parse_git_blame reads origins once per commit and path and points every line at one", "[git][blame]")
{
    const git_blame_result result { parse_git_blame(cyella::test::read_fixture("blame_renamed.bin")) };

    CHECK(result.malformed_line_count == 0);
    CHECK_FALSE(result.binary);
    REQUIRE(result.origins.size() == 4);
    // 주인은 처음 나온 순서다.
    CHECK(result.origins[0].sha == first_sha);
    CHECK(result.origins[0].path == "a[1].txt");
    CHECK(result.origins[0].author_name == "Ann Author");
    CHECK(result.origins[0].author_email == "ann@example.com");
    CHECK(result.origins[0].author_time == 1775005200);
    CHECK(result.origins[0].summary == "first");
    // 파일을 만든 커밋에는 앞 커밋이 없다.
    CHECK(result.origins[0].previous_sha.empty());
    CHECK(result.origins[0].previous_path.empty());
    CHECK_FALSE(result.origins[0].uncommitted);

    CHECK(result.origins[1].sha == third_sha);
    CHECK(result.origins[1].path == "b[12].txt");
    CHECK(result.origins[1].summary == "third: rename");
    // 이름을 바꾼 커밋의 앞 커밋은 옛 경로에 있다.
    CHECK(result.origins[1].previous_sha == second_sha);
    CHECK(result.origins[1].previous_path == "a[1].txt");

    CHECK(result.origins[2].sha == second_sha);
    CHECK(result.origins[2].path == "a[1].txt");
    CHECK(result.origins[2].summary == "second \xED\x95\x9C\xEA\xB8\x80 \xEC\xA0\x9C\xEB\xAA\xA9 \xE2\x9C\x93");
    CHECK(result.origins[2].author_time == 1775091600);
    CHECK(result.origins[3].sha == fourth_sha);
    CHECK(result.origins[3].previous_sha == third_sha);
    CHECK(result.origins[3].previous_path == "b[12].txt");

    // 첫 커밋은 한 줄짜리 묶음과 네 줄짜리 묶음으로 두 번 나온다. 둘째 묶음에는 커밋 정보도 `filename`도 없다.
    CHECK(origins_of(result) == std::vector<std::uint32_t> { 0, 1, 0, 0, 0, 0, 2, 3 });
    CHECK(texts_of(result) == std::vector<std::string> { "one", "TWO", "three", "four", "five", "six", "seven", "eight" });
    for (std::size_t index { 0 }; index < result.lines.size(); ++index)
    {
        CHECK(result.lines[index].original_line == index + 1);
        CHECK(result.lines[index].final_line == index + 1);
    }
}

TEST_CASE("parse_git_blame marks lines with an all-zero hash as uncommitted", "[git][blame]")
{
    const git_blame_result result { parse_git_blame(cyella::test::read_fixture("blame_worktree.bin")) };

    CHECK(result.malformed_line_count == 0);
    REQUIRE(result.origins.size() == 4);
    CHECK(result.origins[0].sha == std::string(40, '0'));
    CHECK(result.origins[0].uncommitted);
    CHECK(result.origins[0].path == "b[12].txt");
    // 커밋하지 않은 줄의 앞 커밋은 HEAD다.
    CHECK(result.origins[0].previous_sha == fourth_sha);
    for (std::size_t index { 1 }; index < result.origins.size(); ++index)
        CHECK_FALSE(result.origins[index].uncommitted);

    // 맨 앞에 끼운 줄과 고친 마지막 줄이 같은 주인이다. 둘째 묶음에는 커밋 정보가 다시 오지 않는다.
    REQUIRE(result.lines.size() == 9);
    CHECK(origins_of(result) == std::vector<std::uint32_t> { 0, 1, 2, 1, 1, 1, 1, 3, 0 });
    CHECK(result.lines[0].text == "zero");
    // 끼운 줄 때문에 최종 줄 번호가 원래 줄 번호보다 하나 밀린다.
    CHECK(result.lines[1].text == "one");
    CHECK(result.lines[1].original_line == 1);
    CHECK(result.lines[1].final_line == 2);
    // 파일이 줄바꿈 없이 끝나도 git이 `\n`을 붙여 낸다.
    CHECK(result.lines[8].text == "EIGHT");
    CHECK(result.lines[8].final_line == 9);
}

TEST_CASE("parse_git_blame keeps one origin per path when a commit spans two paths", "[git][blame]")
{
    const git_blame_result result { parse_git_blame(cyella::test::read_fixture("blame_two_paths.bin")) };

    CHECK(result.malformed_line_count == 0);
    REQUIRE(result.origins.size() == 2);
    CHECK(result.origins[0].sha == result.origins[1].sha);
    CHECK(result.origins[0].path == "p.txt");
    CHECK(result.origins[1].path == "q.txt");
    // 커밋 정보는 처음 한 번만 온다. 둘째 경로의 주인도 같은 정보를 가진다.
    CHECK(result.origins[1].author_name == "Ann Author");
    CHECK(result.origins[1].author_email == "ann@example.com");
    CHECK(result.origins[1].author_time == result.origins[0].author_time);
    CHECK(result.origins[1].summary == "base");

    REQUIRE(result.lines.size() == 10);
    CHECK(origins_of(result) == std::vector<std::uint32_t> { 0, 0, 0, 1, 1, 1, 1, 1, 1, 1 });
    // q.txt에서 온 줄은 그 파일의 줄 번호로 센다.
    CHECK(result.lines[3].text == "q1");
    CHECK(result.lines[3].original_line == 1);
    CHECK(result.lines[3].final_line == 4);
    CHECK(result.lines[9].original_line == 7);
    CHECK(result.lines[9].final_line == 10);
}

TEST_CASE("parse_git_blame keeps carriage returns and invalid UTF-8 byte for byte", "[git][blame]")
{
    const git_blame_result result { parse_git_blame(cyella::test::read_fixture("blame_crlf.bin")) };

    CHECK(result.malformed_line_count == 0);
    CHECK_FALSE(result.binary);
    CHECK(texts_of(result) == std::vector<std::string> { "keep\r", "me\r", "\xFF\xFE not utf-8\r", "no newline at end" });
}

TEST_CASE("parse_git_blame reports a file with NUL bytes as binary and drops its lines", "[git][blame]")
{
    const git_blame_result result { parse_git_blame(cyella::test::read_fixture("blame_binary.bin")) };

    CHECK(result.malformed_line_count == 0);
    CHECK(result.binary);
    CHECK(result.lines.empty());
    CHECK(result.origins.size() == 1);
}

TEST_CASE("parse_git_blame reads sha256 hashes and their all-zero uncommitted form", "[git][blame]")
{
    const git_blame_result result { parse_git_blame(cyella::test::read_fixture("blame_sha256.bin")) };

    CHECK(result.malformed_line_count == 0);
    REQUIRE(result.origins.size() == 3);
    CHECK(result.origins[0].sha == "b3422603fe0fafc5152be945632a8609ddbc7df47e90581b60148407d67fd47a");
    CHECK(result.origins[0].path == "f.txt");
    CHECK(result.origins[1].sha == "0ea81596ac74a737dad2bc54ee41f00c6a33ddea95fef22559f2dd760f0570f5");
    CHECK(result.origins[1].path == "g.txt");
    CHECK(result.origins[1].previous_sha == result.origins[0].sha);
    CHECK(result.origins[1].previous_path == "f.txt");
    CHECK(result.origins[2].sha == std::string(64, '0'));
    CHECK(result.origins[2].uncommitted);
    CHECK_FALSE(result.origins[1].uncommitted);
    CHECK(origins_of(result) == std::vector<std::uint32_t> { 0, 0, 1, 2 });
    CHECK(texts_of(result) == std::vector<std::string> { "a", "b", "c", "d" });
}

TEST_CASE("git_blame_parser gives the same result for any chunking", "[git][blame]")
{
    for (const std::string_view name : { "blame_renamed.bin", "blame_worktree.bin", "blame_two_paths.bin", "blame_crlf.bin", "blame_binary.bin", "blame_sha256.bin" })
    {
        INFO("fixture " << name);
        const std::string bytes { cyella::test::read_fixture(name) };
        const std::vector<std::string> expected { describe(parse_git_blame(bytes)) };
        REQUIRE(expected.back() == "malformed 0");

        CHECK(describe(parse_in_chunks(bytes, { 1 })) == expected);
        CHECK(describe(parse_in_chunks(bytes, { 7 })) == expected);
        CHECK(describe(parse_in_chunks(bytes, { bytes.size() + 100 })) == expected);

        // 줄 한가운데, 해시 한가운데, UTF-8 문자 한가운데에서도 끊긴다.
        std::mt19937 generator { 20260401u };
        std::uniform_int_distribution<std::size_t> distribution { 1, 40 };
        for (int round { 0 }; round < 50; ++round)
        {
            std::vector<std::size_t> sizes {};
            for (int index { 0 }; index < 64; ++index)
                sizes.push_back(distribution(generator));
            CHECK(describe(parse_in_chunks(bytes, sizes)) == expected);
        }
    }
}

TEST_CASE("parse_git_blame returns nothing for an empty file", "[git][blame]")
{
    const git_blame_result result { parse_git_blame({}) };

    CHECK(result.origins.empty());
    CHECK(result.lines.empty());
    CHECK_FALSE(result.binary);
    CHECK(result.malformed_line_count == 0);
}

// 실제 git으로는 만들지 못한 모양이다 (Windows의 git은 경로의 `\`를 `/`로 바꾸고 파일 이름에 `"`를 쓸 수 없다). git의
// `write_name_quoted`가 내는 C 스타일 인용을 손으로 적었다.
TEST_CASE("parse_git_blame unquotes paths and skips keys it does not know", "[git][blame]")
{
    std::string bytes { group_header(hand_sha, 1, 1, 1, hand_details) };
    bytes.append("boundary\nsome-future-key with a value\n");
    bytes.append("previous " + std::string(40, 'd') + " \"old \\\"name\\\"\\t\\355\\225\\234.txt\"\n");
    bytes.append("filename \"new\\\\name.txt\"\n");
    bytes.append("\tcontent\n");

    const git_blame_result result { parse_git_blame(bytes) };

    CHECK(result.malformed_line_count == 0);
    REQUIRE(result.origins.size() == 1);
    CHECK(result.origins[0].path == "new\\name.txt");
    CHECK(result.origins[0].previous_sha == std::string(40, 'd'));
    CHECK(result.origins[0].previous_path == "old \"name\"\t\xED\x95\x9C.txt");
    CHECK(result.origins[0].author_name == "Morgan");
    CHECK(result.origins[0].author_email == "morgan@example.com");
    CHECK(result.origins[0].author_time == 1700000000);
    CHECK(result.origins[0].summary == "hand made");
    REQUIRE(result.lines.size() == 1);
    CHECK(result.lines[0].text == "content");
}

TEST_CASE("parse_git_blame strips only the leading tab and the final newline of a content line", "[git][blame]")
{
    std::string bytes { group_header(hand_sha, 1, 1, 3, hand_details) };
    bytes.append("filename a.txt\n");
    // 내용이 탭으로 시작하거나, 머리 줄처럼 보이거나, 비어 있어도 내용이다.
    bytes.append("\t\tindented with a tab \n");
    bytes.append(hand_sha + " 2 2\n\t" + hand_sha + " 9 9 9\n");
    bytes.append(hand_sha + " 3 3\n\t\n");

    const git_blame_result result { parse_git_blame(bytes) };

    CHECK(result.malformed_line_count == 0);
    CHECK(texts_of(result) == std::vector<std::string> { "\tindented with a tab ", hand_sha + " 9 9 9", "" });
    CHECK(origins_of(result) == std::vector<std::uint32_t> { 0, 0, 0 });
}

TEST_CASE("parse_git_blame counts broken input instead of guessing", "[git][blame]")
{
    const std::string good { cyella::test::read_fixture("blame_renamed.bin") };

    SECTION("text that is not blame output")
    {
        const git_blame_result result { parse_git_blame("fatal: no such path 'a.txt' in HEAD\n") };
        CHECK(result.malformed_line_count == 1);
        CHECK(result.lines.empty());
        CHECK(result.origins.empty());
    }

    SECTION("a garbage line before good output does not lose the good lines")
    {
        const git_blame_result result { parse_git_blame("garbage\n" + good) };
        CHECK(result.malformed_line_count == 1);
        CHECK(result.lines.size() == 8);
    }

    SECTION("output cut in the middle of a content line")
    {
        const git_blame_result result { parse_git_blame(std::string_view { good }.substr(0, good.size() - 3)) };
        CHECK(result.malformed_line_count > 0);
        CHECK(result.lines.size() == 7);
    }

    SECTION("output cut after a header, before its content line")
    {
        const std::size_t last_header { good.rfind(fourth_sha + " 8 8 1") };
        REQUIRE(last_header != std::string::npos);
        const git_blame_result result { parse_git_blame(std::string_view { good }.substr(0, good.find('\n', last_header) + 1)) };
        CHECK(result.malformed_line_count == 1);
        CHECK(result.lines.size() == 7);
    }

    SECTION("a content line without a header")
    {
        const git_blame_result result { parse_git_blame("\torphan\n") };
        CHECK(result.malformed_line_count == 1);
        CHECK(result.lines.empty());
    }

    SECTION("a repeated header for a commit that was never described")
    {
        const git_blame_result result { parse_git_blame(hand_sha + " 1 1 1\n\tcontent\n") };
        CHECK(result.malformed_line_count == 1);
        CHECK(result.lines.empty());
    }

    SECTION("a header whose line numbers are not numbers is not a header")
    {
        const git_blame_result result { parse_git_blame(hand_sha + " one 1 1\n\tcontent\n") };
        CHECK(result.malformed_line_count == 2);
        CHECK(result.lines.empty());
    }
}

TEST_CASE("git_blame_parser can be reused after finish", "[git][blame]")
{
    const std::string bytes { cyella::test::read_fixture("blame_two_paths.bin") };
    git_blame_parser parser {};

    parser.feed(bytes);
    const std::vector<std::string> first { describe(parser.finish()) };
    // 앞 출력의 커밋과 묶음을 기억한 채로 새 출력을 읽으면 주인 번호가 어긋난다.
    parser.feed(bytes);
    CHECK(describe(parser.finish()) == first);

    // 잘린 출력 뒤에도 꼬리가 남지 않는다.
    parser.feed(std::string_view { bytes }.substr(0, bytes.size() / 2));
    CHECK(parser.finish().malformed_line_count > 0);
    parser.feed(bytes);
    CHECK(describe(parser.finish()) == first);
}

TEST_CASE("git_blame_parser streams 200000 lines without quadratic copying", "[git][blame][perf]")
{
    constexpr std::size_t line_count { 200000 };
    constexpr std::size_t commit_count { 500 };

    std::string bytes {};
    bytes.reserve(line_count * 80);
    for (std::size_t index { 0 }; index < line_count; ++index)
    {
        // 한 줄짜리 묶음이 커밋을 돌아가며 나온다. 커밋 정보는 처음 나올 때만 붙는다.
        const std::size_t commit { index % commit_count };
        std::string sha { std::to_string(commit) };
        sha.insert(0, 40 - sha.size(), 'a');
        bytes.append(sha).append(" ").append(std::to_string(index + 1)).append(" ").append(std::to_string(index + 1)).append(" 1\n");
        if (index < commit_count)
            bytes.append(hand_details).append("filename big.txt\n");
        bytes.append("\tline number ").append(std::to_string(index)).append("\n");
    }

    git_blame_parser parser {};
    const auto begin { std::chrono::steady_clock::now() };
    // 프로세스 실행기가 넘기는 조각(최대 64 KiB)처럼, 줄 경계와 어긋나게 넣는다.
    for (std::size_t position { 0 }; position < bytes.size(); position += 65521)
        parser.feed(std::string_view { bytes }.substr(position, 65521));
    const git_blame_result result { parser.finish() };
    const auto elapsed { std::chrono::steady_clock::now() - begin };

    CHECK(result.malformed_line_count == 0);
    CHECK(result.origins.size() == commit_count);
    REQUIRE(result.lines.size() == line_count);
    CHECK(result.lines.back().origin == (line_count - 1) % commit_count);
    CHECK(result.lines.back().final_line == line_count);
    // 선형이면 Debug에서도 몇 초 안쪽이다. 이차 복사라면 수십 초가 걸린다.
    CHECK(elapsed < std::chrono::seconds { 20 });
}

TEST_CASE("git_blame_parser copes with one huge line fed byte by byte", "[git][blame][perf]")
{
    // 조각마다 꼬리 전체를 다시 찾거나 옮기면 이 입력에서 이차 시간이 걸린다.
    std::string bytes { group_header(hand_sha, 1, 1, 1, hand_details) };
    bytes.append("filename min.js\n\t");
    bytes.append(200000, 'x');
    bytes.push_back('\n');

    git_blame_parser parser {};
    const auto begin { std::chrono::steady_clock::now() };
    for (const char value : bytes)
        parser.feed(std::string_view { &value, 1 });
    const git_blame_result result { parser.finish() };
    const auto elapsed { std::chrono::steady_clock::now() - begin };

    CHECK(result.malformed_line_count == 0);
    REQUIRE(result.lines.size() == 1);
    CHECK(result.lines[0].text.size() == 200000);
    CHECK(elapsed < std::chrono::seconds { 10 });
}
