#include "domain/hosting_models.h"

#include <catch2/catch_test_macros.hpp>

using namespace cyella;

TEST_CASE("ISO 8601 timestamps become unix seconds", "[hosting]")
{
    CHECK(parse_iso8601_seconds("1970-01-01T00:00:00Z") == 0);
    CHECK(parse_iso8601_seconds("2026-10-20T09:30:12Z") == 1792488612);
    CHECK(parse_iso8601_seconds("2026-10-20T18:30:12+09:00") == 1792488612);
    CHECK(parse_iso8601_seconds("2026-10-20T09:30:12.123Z") == 1792488612);
    CHECK(parse_iso8601_seconds("2026-10-20T04:30:12-05:00") == 1792488612);
    CHECK(parse_iso8601_seconds("2000-02-29T00:00:00Z") == 951782400);
    CHECK(parse_iso8601_seconds("2026-10-20") == 1792454400);
    CHECK(parse_iso8601_seconds("") == 0);
    CHECK(parse_iso8601_seconds("yesterday") == 0);
    CHECK(parse_iso8601_seconds("2026-13-01T00:00:00Z") == 0);
}

TEST_CASE("CI states are combined into one", "[hosting]")
{
    CHECK(combine_ci_states({}) == ci_state::none);
    CHECK(combine_ci_states({ ci_state::none }) == ci_state::none);
    CHECK(combine_ci_states({ ci_state::success, ci_state::success }) == ci_state::success);
    CHECK(combine_ci_states({ ci_state::success, ci_state::pending }) == ci_state::pending);
    CHECK(combine_ci_states({ ci_state::pending, ci_state::failure, ci_state::success }) == ci_state::failure);
    CHECK(combine_ci_states({ ci_state::error, ci_state::pending }) == ci_state::error);
    CHECK(combine_ci_states({ ci_state::error, ci_state::failure }) == ci_state::failure);
}

TEST_CASE("rate limit errors state the remaining count and reset time", "[hosting]")
{
    hosting_error error {};
    error.kind = hosting_error_kind::rate_limited;
    error.status_code = 403;
    error.rate_limit.remaining = 0;
    error.rate_limit.reset_at = 1000 + 600;
    const std::string text { describe_hosting_error(error, 1000) };
    CHECK(text.find("호출 한도") != std::string::npos);
    CHECK(text.find("남은 횟수 0") != std::string::npos);
    CHECK(text.find("10분 후") != std::string::npos);

    error.rate_limit.reset_at.reset();
    error.rate_limit.retry_after_seconds = 30;
    CHECK(describe_hosting_error(error, 1000).find("30초 후") != std::string::npos);

    CHECK(describe_remaining_time(-5) == "곧");
    CHECK(describe_remaining_time(61) == "2분 후");
    CHECK(describe_remaining_time(7200) == "2시간 후");
}

TEST_CASE("error text appends the server reason", "[hosting]")
{
    hosting_error error {};
    error.kind = hosting_error_kind::rejected;
    error.message = "A pull request already exists";
    CHECK(describe_hosting_error(error, 0).find("A pull request already exists") != std::string::npos);
    CHECK(describe_hosting_error({}, 0).empty());
}
