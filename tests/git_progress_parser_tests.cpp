#include "infrastructure/git_progress_parser.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace cyella;

TEST_CASE("progress parser reads a percentage with counts and speed", "[progress]")
{
    const auto progress { parse_git_progress(u8"Receiving objects:  45% (450/1000), 1.2 MiB | 2.0 MiB/s") };

    REQUIRE(progress.has_value());
    CHECK(progress->phase_text == u8"Receiving objects");
    REQUIRE(progress->fraction.has_value());
    CHECK(*progress->fraction == Catch::Approx(0.45));
    CHECK_FALSE(progress->remote);
}

TEST_CASE("progress parser treats a finished phase as complete", "[progress]")
{
    const auto progress { parse_git_progress(u8"Resolving deltas: 100% (20/20), done.") };

    REQUIRE(progress.has_value());
    CHECK(progress->phase_text == u8"Resolving deltas");
    REQUIRE(progress->fraction.has_value());
    CHECK(*progress->fraction == Catch::Approx(1.0));
}

TEST_CASE("progress parser strips the remote prefix", "[progress]")
{
    const auto progress { parse_git_progress(u8"remote: Counting objects:  10% (1/10)") };

    REQUIRE(progress.has_value());
    CHECK(progress->phase_text == u8"Counting objects");
    CHECK(progress->remote);
    REQUIRE(progress->fraction.has_value());
    CHECK(*progress->fraction == Catch::Approx(0.1));
}

TEST_CASE("progress parser reads an enumeration with and without done", "[progress]")
{
    const auto finished { parse_git_progress(u8"Enumerating objects: 5, done.") };
    REQUIRE(finished.has_value());
    CHECK(finished->phase_text == u8"Enumerating objects");
    REQUIRE(finished->fraction.has_value());
    CHECK(*finished->fraction == Catch::Approx(1.0));

    // 전체 개수를 아직 모르는 열거는 단계 이름만 알린다.
    const auto running { parse_git_progress(u8"Enumerating objects: 5") };
    REQUIRE(running.has_value());
    CHECK(running->phase_text == u8"Enumerating objects");
    CHECK_FALSE(running->fraction.has_value());
}

TEST_CASE("progress parser reads the writing phase", "[progress]")
{
    const auto progress { parse_git_progress(u8"Writing objects:  33% (1/3)") };

    REQUIRE(progress.has_value());
    CHECK(progress->phase_text == u8"Writing objects");
    REQUIRE(progress->fraction.has_value());
    CHECK(*progress->fraction == Catch::Approx(1.0 / 3.0));
}

TEST_CASE("progress parser prefers the exact counts over the rounded percent", "[progress]")
{
    const auto progress { parse_git_progress(u8"Receiving objects:   0% (1/1000)") };

    REQUIRE(progress.has_value());
    REQUIRE(progress->fraction.has_value());
    CHECK(*progress->fraction == Catch::Approx(0.001));
}

TEST_CASE("progress parser accepts line terminators and a percentage without counts", "[progress]")
{
    const auto progress { parse_git_progress(u8"Updating files:  50%\r") };

    REQUIRE(progress.has_value());
    CHECK(progress->phase_text == u8"Updating files");
    REQUIRE(progress->fraction.has_value());
    CHECK(*progress->fraction == Catch::Approx(0.5));
}

TEST_CASE("progress parser clamps an out of range percentage", "[progress]")
{
    const auto progress { parse_git_progress(u8"Checking out files: 250%") };

    REQUIRE(progress.has_value());
    REQUIRE(progress->fraction.has_value());
    CHECK(*progress->fraction == Catch::Approx(1.0));
}

TEST_CASE("progress parser ignores lines that are not progress", "[progress]")
{
    CHECK_FALSE(parse_git_progress(u8"").has_value());
    CHECK_FALSE(parse_git_progress(u8"fatal: repository not found").has_value());
    CHECK_FALSE(parse_git_progress(u8"error: 50% of the files failed").has_value());
    CHECK_FALSE(parse_git_progress(u8"From github.com:user/repo").has_value());
    CHECK_FALSE(parse_git_progress(u8" * [new branch]      main       -> origin/main").has_value());
    CHECK_FALSE(parse_git_progress(u8"remote: Total 3 (delta 0), reused 0 (delta 0)").has_value());
    CHECK_FALSE(parse_git_progress(u8"Cloning into 'repo'...").has_value());
    CHECK_FALSE(parse_git_progress(u8"Receiving objects: soon").has_value());
    CHECK_FALSE(parse_git_progress(u8"Counting: 5 files").has_value());
    CHECK_FALSE(parse_git_progress(u8"remote: ").has_value());
}
