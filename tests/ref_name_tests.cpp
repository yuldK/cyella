#include "domain/ref_name.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>

namespace {
    using cyella::find_ref_name_problem;
    using cyella::find_revision_problem;
    using cyella::ref_name_problem;
} // namespace

TEST_CASE("find_ref_name_problem accepts ordinary branch and tag names", "[ref_name]")
{
    for (const std::string_view name : { "main", "feature/login", "release-1.2.3", "v1.0", "user/ann/topic_2", "a.b", "한글-브랜치", "x@y", "a-b" })
        CHECK_FALSE(find_ref_name_problem(name).has_value());
}

TEST_CASE("find_ref_name_problem rejects names git would reject", "[ref_name]")
{
    CHECK(find_ref_name_problem("") == ref_name_problem::empty);
    CHECK(find_ref_name_problem("-main") == ref_name_problem::leading_dash);
    CHECK(find_ref_name_problem("--force") == ref_name_problem::leading_dash);
    CHECK(find_ref_name_problem("/main") == ref_name_problem::leading_slash);
    CHECK(find_ref_name_problem("main/") == ref_name_problem::trailing_slash);
    CHECK(find_ref_name_problem("a//b") == ref_name_problem::consecutive_slashes);
    CHECK(find_ref_name_problem(".hidden") == ref_name_problem::component_starts_with_dot);
    CHECK(find_ref_name_problem("a/.hidden") == ref_name_problem::component_starts_with_dot);
    CHECK(find_ref_name_problem("topic.lock") == ref_name_problem::component_ends_with_lock);
    CHECK(find_ref_name_problem("dir.lock/topic") == ref_name_problem::component_ends_with_lock);
    CHECK(find_ref_name_problem("topic.") == ref_name_problem::trailing_dot);
    CHECK(find_ref_name_problem("a..b") == ref_name_problem::contains_double_dot);
    CHECK(find_ref_name_problem("a@{b") == ref_name_problem::contains_at_brace);
    CHECK(find_ref_name_problem("@") == ref_name_problem::is_single_at);
    CHECK(find_ref_name_problem("HEAD") == ref_name_problem::is_head);
    CHECK(find_ref_name_problem("a b") == ref_name_problem::forbidden_character);
    CHECK(find_ref_name_problem("a~1") == ref_name_problem::forbidden_character);
    CHECK(find_ref_name_problem("a^") == ref_name_problem::forbidden_character);
    CHECK(find_ref_name_problem("a:b") == ref_name_problem::forbidden_character);
    CHECK(find_ref_name_problem("a?") == ref_name_problem::forbidden_character);
    CHECK(find_ref_name_problem("a*") == ref_name_problem::forbidden_character);
    CHECK(find_ref_name_problem("a[0]") == ref_name_problem::forbidden_character);
    CHECK(find_ref_name_problem("a\\b") == ref_name_problem::backslash);
    CHECK(find_ref_name_problem(std::string_view { "a\tb" }) == ref_name_problem::control_character);
    CHECK(find_ref_name_problem(std::string_view { "a\0b", 3 }) == ref_name_problem::control_character);
    CHECK(find_ref_name_problem("a\x7F") == ref_name_problem::control_character);
}

TEST_CASE("find_revision_problem only guards against option injection and control characters", "[ref_name]")
{
    for (const std::string_view revision : { "HEAD", "HEAD~3", "origin/main", "c1bf80d6a7260e533f88e475f0d4c611194766c2", "main^2", "stash@{0}", "v1.0^{commit}" })
        CHECK_FALSE(find_revision_problem(revision).has_value());

    CHECK(find_revision_problem("") == ref_name_problem::empty);
    CHECK(find_revision_problem("--upload-pack=evil") == ref_name_problem::leading_dash);
    CHECK(find_revision_problem("-1") == ref_name_problem::leading_dash);
    CHECK(find_revision_problem("a\nb") == ref_name_problem::control_character);
}

TEST_CASE("ref_name_problem_text describes every problem in ASCII", "[ref_name]")
{
    for (int value { 0 }; value <= static_cast<int>(ref_name_problem::backslash); ++value)
    {
        const std::string_view text { cyella::ref_name_problem_text(static_cast<ref_name_problem>(value)) };
        CHECK_FALSE(text.empty());
        for (const char character : text)
            CHECK(static_cast<unsigned char>(character) < 0x80u);
    }
}
