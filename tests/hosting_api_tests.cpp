#include "infrastructure/hosting_api.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace cyella;

namespace {
    hosting_repository github_repository()
    {
        return *resolve_hosting_repository("https://github.com/octo/hello-world.git", "origin", {});
    }

    hosting_repository gitlab_repository()
    {
        return *resolve_hosting_repository("git@gitlab.com:group/sub/app.git", "origin", {});
    }

    hosting_http_response make_response(const int status, std::string body, std::vector<hosting_http_header> headers = {})
    {
        hosting_http_response response {};
        response.status_code = status;
        response.body = std::move(body);
        response.headers = std::move(headers);
        return response;
    }

    // GitHub `GET /repos/{owner}/{repo}/pulls`의 응답 모양이다 (공개 문서의 예시에서 쓰는 필드만 남겼다).
    const char* const github_pulls_json = R"([
      {
        "url": "https://api.github.com/repos/octo/hello-world/pulls/1347",
        "id": 1, "number": 1347, "state": "open", "draft": false,
        "title": "Amazing new feature",
        "user": { "login": "octocat", "id": 1 },
        "body": "Please pull these awesome changes in!",
        "created_at": "2011-01-26T19:01:12Z", "updated_at": "2011-01-26T19:01:12Z", "merged_at": null,
        "html_url": "https://github.com/octo/hello-world/pull/1347",
        "head": { "label": "octocat:new-topic", "ref": "new-topic", "sha": "6dcb09b5b57875f334f61aebed695e2e4193db5e", "repo": { "full_name": "octo/hello-world", "fork": false } },
        "base": { "label": "octo:master", "ref": "master", "sha": "6dcb09b5b57875f334f61aebed695e2e4193db5e", "repo": { "full_name": "octo/hello-world" } }
      },
      {
        "number": 1348, "state": "closed", "draft": true, "title": "Merged work",
        "user": { "login": "hubot" }, "created_at": "2011-02-01T10:00:00Z", "updated_at": "2011-02-02T10:00:00Z", "merged_at": "2011-02-02T10:00:00Z",
        "html_url": "https://github.com/octo/hello-world/pull/1348",
        "head": { "ref": "feature/x", "sha": "aaaa", "repo": { "full_name": "forker/hello-world" } },
        "base": { "ref": "main", "sha": "bbbb", "repo": { "full_name": "octo/hello-world" } }
      },
      { "number": 1349, "state": "closed", "title": "Abandoned", "user": null, "merged_at": null,
        "head": { "ref": "gone", "sha": "cccc", "repo": null }, "base": { "ref": "main", "repo": { "full_name": "octo/hello-world" } } },
      { "title": "no number" },
      7
    ])";

    // GitLab `GET /projects/:id/merge_requests`의 응답 모양이다.
    const char* const gitlab_merge_requests_json = R"([
      {
        "id": 155, "iid": 12, "project_id": 3, "title": "Add feature", "description": "Implements it",
        "state": "opened", "created_at": "2022-05-13T07:26:38.402Z", "updated_at": "2022-05-14T07:26:38.402Z",
        "source_branch": "feature", "target_branch": "main", "sha": "8888888888888888888888888888888888888888",
        "author": { "id": 1, "username": "root", "name": "Admin" },
        "draft": true, "work_in_progress": true,
        "source_project_id": 3, "target_project_id": 3,
        "web_url": "http://gitlab.example.com/group/sub/app/-/merge_requests/12",
        "head_pipeline": { "id": 7, "status": "running", "web_url": "http://gitlab.example.com/group/sub/app/-/pipelines/7" }
      },
      {
        "iid": 11, "title": "Old", "state": "merged", "source_branch": "old", "target_branch": "main", "sha": "9999",
        "author": { "username": "dev" }, "source_project_id": 4, "target_project_id": 3, "web_url": "http://gitlab.example.com/group/sub/app/-/merge_requests/11"
      },
      { "iid": 10, "title": "Locked", "state": "locked", "source_branch": "l", "target_branch": "main", "author": { "username": "dev" } },
      { "title": "no iid" }
    ])";
} // namespace

TEST_CASE("percent encoding replaces every reserved character", "[hosting]")
{
    CHECK(percent_encode_component("group/sub/app") == "group%2Fsub%2Fapp");
    CHECK(percent_encode_component("a b&c=d") == "a%20b%26c%3Dd");
    CHECK(percent_encode_component("AZaz09-._~") == "AZaz09-._~");
    CHECK(percent_encode_component("한") == "%ED%95%9C");
}

TEST_CASE("list requests are built for GitHub and GitLab", "[hosting]")
{
    SECTION("GitHub")
    {
        const hosting_http_request open = make_list_pull_requests_request(github_repository(), {});
        CHECK(open.method == hosting_http_method::get);
        CHECK(open.url == "https://api.github.com/repos/octo/hello-world/pulls?state=open&sort=updated&direction=desc&per_page=30&page=1");
        const hosting_http_request all = make_list_pull_requests_request(github_repository(), { .include_closed = true, .page = 3, .per_page = 500 });
        CHECK(all.url == "https://api.github.com/repos/octo/hello-world/pulls?state=all&sort=updated&direction=desc&per_page=100&page=3");
        bool has_accept { false };
        for (const hosting_http_header& header : open.headers)
            has_accept = has_accept || (header.name == "Accept" && header.value == "application/vnd.github+json");
        CHECK(has_accept);
    }
    SECTION("GitLab은 프로젝트 경로를 부호화한다")
    {
        const hosting_http_request open = make_list_pull_requests_request(gitlab_repository(), {});
        CHECK(open.url == "https://gitlab.com/api/v4/projects/group%2Fsub%2Fapp/merge_requests?state=opened&order_by=updated_at&sort=desc&per_page=30&page=1");
        const hosting_http_request all = make_list_pull_requests_request(gitlab_repository(), { .include_closed = true, .page = 2 });
        CHECK(all.url.find("state=all") != std::string::npos);
        CHECK(all.url.find("page=2") != std::string::npos);
    }
    SECTION("요청에는 토큰이 없다")
    {
        for (const hosting_http_header& header : make_list_pull_requests_request(github_repository(), {}).headers)
            CHECK(header.name != "Authorization");
    }
}

TEST_CASE("a GitHub pull request list is parsed", "[hosting]")
{
    const hosting_result<pull_request_page> result = parse_pull_request_list_response(hosting_provider::github, make_response(200, github_pulls_json), 1);
    REQUIRE(result.ok());
    const pull_request_page& page { *result.value };
    REQUIRE(page.items.size() == 3);
    CHECK(page.skipped == 2);
    CHECK(page.next_page.has_value() == false);

    const pull_request_info& first { page.items[0] };
    CHECK(first.number == 1347);
    CHECK(first.title == "Amazing new feature");
    CHECK(first.author == "octocat");
    CHECK(first.source_branch == "new-topic");
    CHECK(first.target_branch == "master");
    CHECK(first.head_sha == "6dcb09b5b57875f334f61aebed695e2e4193db5e");
    CHECK(first.state == pull_request_state::open);
    CHECK(first.draft == false);
    CHECK(first.from_fork == false);
    CHECK(first.web_url == "https://github.com/octo/hello-world/pull/1347");
    CHECK(first.body == "Please pull these awesome changes in!");
    CHECK(first.created_at == 1296068472);

    const pull_request_info& merged { page.items[1] };
    CHECK(merged.state == pull_request_state::merged);
    CHECK(merged.draft);
    CHECK(merged.from_fork);

    const pull_request_info& abandoned { page.items[2] };
    CHECK(abandoned.state == pull_request_state::closed);
    CHECK(abandoned.from_fork);
    CHECK(abandoned.author.empty());
}

TEST_CASE("a GitLab merge request list is parsed", "[hosting]")
{
    const hosting_result<pull_request_page> result
        = parse_pull_request_list_response(hosting_provider::gitlab, make_response(200, gitlab_merge_requests_json, { { "X-Next-Page", "2" }, { "X-Page", "1" } }), 1);
    REQUIRE(result.ok());
    const pull_request_page& page { *result.value };
    REQUIRE(page.items.size() == 3);
    CHECK(page.skipped == 1);
    REQUIRE(page.next_page.has_value());
    CHECK(*page.next_page == 2);

    const pull_request_info& first { page.items[0] };
    CHECK(first.number == 12);
    CHECK(first.author == "root");
    CHECK(first.source_branch == "feature");
    CHECK(first.target_branch == "main");
    CHECK(first.state == pull_request_state::open);
    CHECK(first.draft);
    CHECK(first.from_fork == false);
    CHECK(first.head_sha == "8888888888888888888888888888888888888888");
    CHECK(first.body == "Implements it");
    REQUIRE(first.ci.has_value());
    CHECK(*first.ci == ci_state::pending);
    CHECK(first.updated_at == 1652513198);

    CHECK(page.items[1].state == pull_request_state::merged);
    CHECK(page.items[1].from_fork);
    CHECK(page.items[1].ci.has_value() == false);
    CHECK(page.items[2].state == pull_request_state::closed);
}

TEST_CASE("pagination markers are read", "[hosting]")
{
    SECTION("GitHub는 Link 헤더의 rel=next다")
    {
        const std::string link_header = "<https://api.github.com/repositories/1/pulls?page=3&per_page=30>; rel=\"next\", <https://api.github.com/repositories/1/pulls?page=9>; rel=\"last\"";
        const hosting_http_response with_next = make_response(200, "[]", { { "Link", link_header } });
        const auto result = parse_pull_request_list_response(hosting_provider::github, with_next, 2);
        REQUIRE(result.ok());
        REQUIRE(result.value->next_page.has_value());
        CHECK(*result.value->next_page == 3);
        CHECK(result.value->page == 2);

        const hosting_http_response last_page { make_response(200, "[]", { { "link", "<https://api.github.com/repositories/1/pulls?page=1>; rel=\"prev\", <...>; rel=\"first\"" } }) };
        CHECK(parse_pull_request_list_response(hosting_provider::github, last_page, 9).value->next_page.has_value() == false);
    }
    SECTION("GitLab은 X-Next-Page다")
    {
        CHECK(parse_pull_request_list_response(hosting_provider::gitlab, make_response(200, "[]", { { "x-next-page", "" } }), 1).value->next_page.has_value() == false);
        const auto result = parse_pull_request_list_response(hosting_provider::gitlab, make_response(200, "[]", { { "x-next-page", "5" } }), 4);
        CHECK(*result.value->next_page == 5);
    }
}

TEST_CASE("the create request body is built", "[hosting]")
{
    const create_pull_request_params params { .title = "Add \"quote\"", .body = "줄1\n줄2", .source_branch = "feature/x", .target_branch = "main", .draft = true };
    SECTION("GitHub")
    {
        const hosting_http_request request = make_create_pull_request_request(github_repository(), params);
        CHECK(request.method == hosting_http_method::post);
        CHECK(request.url == "https://api.github.com/repos/octo/hello-world/pulls");
        CHECK(request.content_type == "application/json");
        const nlohmann::json body = nlohmann::json::parse(request.body);
        CHECK(body["title"] == "Add \"quote\"");
        CHECK(body["head"] == "feature/x");
        CHECK(body["base"] == "main");
        CHECK(body["body"] == "줄1\n줄2");
        CHECK(body["draft"] == true);
    }
    SECTION("GitLab은 초안을 제목 접두어로 보낸다")
    {
        const hosting_http_request request = make_create_pull_request_request(gitlab_repository(), params);
        CHECK(request.url == "https://gitlab.com/api/v4/projects/group%2Fsub%2Fapp/merge_requests");
        const nlohmann::json body = nlohmann::json::parse(request.body);
        CHECK(body["title"] == "Draft: Add \"quote\"");
        CHECK(body["source_branch"] == "feature/x");
        CHECK(body["target_branch"] == "main");
        CHECK(body["description"] == "줄1\n줄2");
        CHECK(body.contains("draft") == false);

        create_pull_request_params already { params };
        already.title = "Draft: keep";
        CHECK(nlohmann::json::parse(make_create_pull_request_request(gitlab_repository(), already).body)["title"] == "Draft: keep");
        already.draft = false;
        already.title = "Plain";
        already.body.clear();
        const nlohmann::json plain = nlohmann::json::parse(make_create_pull_request_request(gitlab_repository(), already).body);
        CHECK(plain["title"] == "Plain");
        CHECK(plain.contains("description") == false);
    }
}

TEST_CASE("the create response is parsed", "[hosting]")
{
    const auto github = parse_create_pull_request_response(
        hosting_provider::github, make_response(201, R"({"number": 42, "title": "T", "html_url": "https://github.com/octo/hello-world/pull/42", "head": {"sha": "abc"}})"));
    REQUIRE(github.ok());
    CHECK(github.value->number == 42);
    CHECK(github.value->web_url == "https://github.com/octo/hello-world/pull/42");
    CHECK(github.value->head_sha == "abc");

    const auto gitlab
        = parse_create_pull_request_response(hosting_provider::gitlab, make_response(201, R"({"iid": 7, "id": 99, "title": "T", "web_url": "https://gitlab.com/group/sub/app/-/merge_requests/7"})"));
    REQUIRE(gitlab.ok());
    CHECK(gitlab.value->number == 7);
    CHECK(gitlab.value->web_url.find("merge_requests/7") != std::string::npos);

    CHECK(parse_create_pull_request_response(hosting_provider::github, make_response(201, "{}")).error.kind == hosting_error_kind::invalid_response);
    CHECK(parse_create_pull_request_response(hosting_provider::github, make_response(201, "<html>")).error.kind == hosting_error_kind::invalid_response);
}

TEST_CASE("a rejected create carries the server reason", "[hosting]")
{
    const auto github = parse_create_pull_request_response(hosting_provider::github,
        make_response(422, R"({"message": "Validation Failed", "errors": [{"resource": "PullRequest", "code": "custom", "message": "A pull request already exists for octo:feature."}]})"));
    REQUIRE(github.ok() == false);
    CHECK(github.error.kind == hosting_error_kind::rejected);
    CHECK(github.error.status_code == 422);
    CHECK(github.error.message == "Validation Failed: A pull request already exists for octo:feature.");

    const auto gitlab = parse_create_pull_request_response(hosting_provider::gitlab, make_response(409, R"({"message": ["Another open merge request already exists for this source branch: !5"]})"));
    CHECK(gitlab.error.kind == hosting_error_kind::rejected);
    CHECK(gitlab.error.message == "Another open merge request already exists for this source branch: !5");

    const auto nested = parse_create_pull_request_response(hosting_provider::gitlab, make_response(400, R"({"message": {"title": ["can't be blank"]}})"));
    CHECK(nested.error.message == "title can't be blank");
}

TEST_CASE("rate limit headers are read in both service shapes", "[hosting]")
{
    const rate_limit_info github = read_rate_limit(
        make_response(200, "[]", { { "x-ratelimit-limit", "5000" }, { "x-ratelimit-remaining", "4999" }, { "x-ratelimit-reset", "1700000000" }, { "x-ratelimit-resource", "core" } }));
    CHECK(github.limit == 5000);
    CHECK(github.remaining == 4999);
    CHECK(github.reset_at == 1700000000);
    CHECK(github.retry_after_seconds.has_value() == false);

    const rate_limit_info gitlab
        = read_rate_limit(make_response(429, "Retry later", { { "RateLimit-Limit", "60" }, { "RateLimit-Remaining", "0" }, { "RateLimit-Reset", "1700000100" }, { "Retry-After", "30" } }));
    CHECK(gitlab.limit == 60);
    CHECK(gitlab.remaining == 0);
    CHECK(gitlab.reset_at == 1700000100);
    CHECK(gitlab.retry_after_seconds == 30);

    const rate_limit_info none = read_rate_limit(make_response(200, "[]"));
    CHECK(none.remaining.has_value() == false);
    CHECK(read_rate_limit(make_response(200, "[]", { { "x-ratelimit-remaining", "abc" } })).remaining.has_value() == false);
}

TEST_CASE("403 and 429 are split into rate limits and permission errors", "[hosting]")
{
    SECTION("GitHub 1차 한도: remaining이 0")
    {
        const hosting_error error = classify_hosting_response(hosting_provider::github,
            make_response(403, R"({"message": "API rate limit exceeded for user ID 1.", "documentation_url": "https://docs.github.com/rest/overview/rate-limits-for-the-rest-api"})",
                { { "x-ratelimit-limit", "60" }, { "x-ratelimit-remaining", "0" }, { "x-ratelimit-reset", "1700000600" } }));
        CHECK(error.kind == hosting_error_kind::rate_limited);
        CHECK(error.rate_limit.remaining == 0);
        CHECK(error.rate_limit.reset_at == 1700000600);
        const std::string text = describe_hosting_error(error, 1700000000);
        CHECK(text.find("남은 횟수 0") != std::string::npos);
        CHECK(text.find("10분 후 초기화") != std::string::npos);
    }
    SECTION("GitHub 2차 한도: retry-after나 메시지")
    {
        CHECK(classify_hosting_response(hosting_provider::github, make_response(403, R"({"message": "x"})", { { "retry-after", "60" } })).kind == hosting_error_kind::rate_limited);
        CHECK(classify_hosting_response(hosting_provider::github, make_response(403, R"({"message": "You have exceeded a secondary rate limit."})")).kind == hosting_error_kind::rate_limited);
        CHECK(classify_hosting_response(hosting_provider::github, make_response(429, "{}")).kind == hosting_error_kind::rate_limited);
    }
    SECTION("한도가 남았으면 권한 오류다")
    {
        const hosting_error error
            = classify_hosting_response(hosting_provider::github, make_response(403, R"({"message": "Resource not accessible by personal access token"})", { { "x-ratelimit-remaining", "4000" } }));
        CHECK(error.kind == hosting_error_kind::forbidden);
        CHECK(error.message == "Resource not accessible by personal access token");
    }
    SECTION("GitLab은 429다")
    {
        const hosting_error error = classify_hosting_response(hosting_provider::gitlab, make_response(429, "Retry later", { { "RateLimit-Remaining", "0" }, { "Retry-After", "45" } }));
        CHECK(error.kind == hosting_error_kind::rate_limited);
        CHECK(error.rate_limit.retry_after_seconds == 45);
        CHECK(classify_hosting_response(hosting_provider::gitlab, make_response(403, R"({"message": "403 Forbidden"})")).kind == hosting_error_kind::forbidden);
    }
}

TEST_CASE("other statuses and transport failures are classified", "[hosting]")
{
    CHECK(classify_hosting_response(hosting_provider::github, make_response(401, R"({"message": "Bad credentials"})")).kind == hosting_error_kind::unauthorized);
    CHECK(classify_hosting_response(hosting_provider::github, make_response(404, R"({"message": "Not Found"})")).kind == hosting_error_kind::not_found);
    CHECK(classify_hosting_response(hosting_provider::gitlab, make_response(404, R"({"message": "404 Project Not Found"})")).message == "404 Project Not Found");
    CHECK(classify_hosting_response(hosting_provider::github, make_response(503, "")).kind == hosting_error_kind::server_error);
    hosting_http_response offline {};
    offline.transport_error = "cannot connect";
    CHECK(classify_hosting_response(hosting_provider::github, offline).kind == hosting_error_kind::network);
    hosting_http_response cancelled {};
    cancelled.cancelled = true;
    CHECK(classify_hosting_response(hosting_provider::github, cancelled).kind == hosting_error_kind::cancelled);
    // 아주 긴 이유는 잘린다.
    const std::string huge { R"({"message": ")" + std::string(2000, 'x') + R"("})" };
    CHECK(classify_hosting_response(hosting_provider::github, make_response(422, huge)).message.size() < 400);
}

TEST_CASE("GitHub CI merges the combined status and the check runs", "[hosting]")
{
    const hosting_repository repository = github_repository();
    const std::vector<hosting_http_request> requests = make_ci_status_requests(repository, "abc123");
    REQUIRE(requests.size() == 2);
    CHECK(requests[0].url == "https://api.github.com/repos/octo/hello-world/commits/abc123/status?per_page=100");
    CHECK(requests[1].url == "https://api.github.com/repos/octo/hello-world/commits/abc123/check-runs?per_page=100");

    const hosting_http_response status = make_response(200, R"({"state": "failure", "sha": "abc123", "total_count": 2, "statuses": [
        {"state": "success", "context": "ci/legacy", "target_url": "https://ci.example/1"},
        {"state": "failure", "context": "ci/lint", "target_url": "https://ci.example/2"}]})");
    const hosting_http_response checks = make_response(200, R"({"total_count": 3, "check_runs": [
        {"name": "build", "status": "completed", "conclusion": "success", "html_url": "https://github.com/octo/hello-world/runs/1"},
        {"name": "test", "status": "in_progress", "conclusion": null, "html_url": "https://github.com/octo/hello-world/runs/2"},
        {"name": "docs", "status": "completed", "conclusion": "skipped", "html_url": "https://github.com/octo/hello-world/runs/3"}]})");

    const auto both = parse_ci_status_responses(repository, "abc123", { status, checks });
    REQUIRE(both.ok());
    CHECK(both.value->state == ci_state::failure);
    REQUIRE(both.value->checks.size() == 5);
    CHECK(both.value->checks[0].name == "ci/legacy");
    CHECK(both.value->checks[3].state == ci_state::pending);
    CHECK(both.value->web_url == "https://github.com/octo/hello-world/commit/abc123");

    const auto only_checks = parse_ci_status_responses(repository, "abc123", { make_response(200, R"({"state": "pending", "total_count": 0, "statuses": []})"), checks });
    REQUIRE(only_checks.ok());
    // 상태가 하나도 없을 때 combined가 pending이어도 쓰지 않는다. check-run이 진행 중이라 pending이다.
    CHECK(only_checks.value->state == ci_state::pending);

    const hosting_http_response no_statuses = make_response(200, R"({"state": "pending", "total_count": 0, "statuses": []})");
    const hosting_http_response one_success = make_response(200, R"({"total_count": 1, "check_runs": [{"name": "build", "status": "completed", "conclusion": "success"}]})");
    const hosting_http_response no_runs = make_response(200, R"({"total_count": 0, "check_runs": []})");
    const auto success = parse_ci_status_responses(repository, "abc123", { no_statuses, one_success });
    CHECK(success.value->state == ci_state::success);

    const auto none = parse_ci_status_responses(repository, "abc123", { no_statuses, no_runs });
    CHECK(none.value->state == ci_state::none);
}

TEST_CASE("GitHub conclusions map to CI states", "[hosting]")
{
    const hosting_repository repository = github_repository();
    const auto state_of = [&repository](const char* const status, const char* const conclusion) {
        const std::string body { std::string { R"({"check_runs": [{"name": "n", "status": ")" } + status + R"(", "conclusion": )" + conclusion + "}]}" };
        return parse_ci_status_responses(repository, "s", { make_response(404, ""), make_response(200, body) }).value->state;
    };
    CHECK(state_of("completed", "\"success\"") == ci_state::success);
    CHECK(state_of("completed", "\"neutral\"") == ci_state::success);
    CHECK(state_of("completed", "\"failure\"") == ci_state::failure);
    CHECK(state_of("completed", "\"timed_out\"") == ci_state::failure);
    CHECK(state_of("completed", "\"action_required\"") == ci_state::failure);
    CHECK(state_of("completed", "\"cancelled\"") == ci_state::error);
    CHECK(state_of("queued", "null") == ci_state::pending);
    CHECK(state_of("in_progress", "null") == ci_state::pending);
}

TEST_CASE("an unknown commit has no CI status and other errors surface", "[hosting]")
{
    const hosting_repository repository = github_repository();
    const auto unknown
        = parse_ci_status_responses(repository, "deadbeef", { make_response(422, R"({"message": "No commit found for SHA: deadbeef"})"), make_response(422, R"({"message": "No commit found"})") });
    REQUIRE(unknown.ok());
    CHECK(unknown.value->state == ci_state::none);

    const auto forbidden = parse_ci_status_responses(repository, "abc", { make_response(403, R"({"message": "no"})"), make_response(403, R"({"message": "no"})") });
    REQUIRE(forbidden.ok() == false);
    CHECK(forbidden.error.kind == hosting_error_kind::forbidden);
}

TEST_CASE("GitLab CI is the newest pipeline", "[hosting]")
{
    const hosting_repository repository = gitlab_repository();
    const std::vector<hosting_http_request> requests = make_ci_status_requests(repository, "abc123");
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].url == "https://gitlab.com/api/v4/projects/group%2Fsub%2Fapp/pipelines?sha=abc123&order_by=id&sort=desc&per_page=5");

    const hosting_http_response pipelines = make_response(200, R"([
        {"id": 48, "iid": 12, "project_id": 3, "status": "failed", "source": "push", "ref": "feature", "sha": "abc123", "web_url": "https://gitlab.com/group/sub/app/-/pipelines/48"},
        {"id": 47, "status": "success", "ref": "feature", "sha": "abc123", "web_url": "https://gitlab.com/group/sub/app/-/pipelines/47"}])");
    const auto result = parse_ci_status_responses(repository, "abc123", { pipelines });
    REQUIRE(result.ok());
    CHECK(result.value->state == ci_state::failure);
    CHECK(result.value->web_url == "https://gitlab.com/group/sub/app/-/pipelines/48");
    REQUIRE(result.value->checks.size() == 2);
    CHECK(result.value->checks[0].name == "pipeline #48");

    CHECK(parse_ci_status_responses(repository, "abc", { make_response(200, "[]") }).value->state == ci_state::none);
    CHECK(parse_ci_status_responses(repository, "abc", { make_response(404, "{}") }).value->state == ci_state::none);
    struct pipeline_case
    {
        const char* status;
        ci_state expected;
    };
    const pipeline_case cases[] {
        { "success", ci_state::success },
        { "running", ci_state::pending },
        { "pending", ci_state::pending },
        { "created", ci_state::pending },
        { "canceled", ci_state::error },
        { "skipped", ci_state::none },
        { "manual", ci_state::none },
    };
    for (const pipeline_case& entry : cases)
    {
        const hosting_http_response response = make_response(200, std::string { R"([{"id": 1, "status": ")" } + entry.status + R"("}])");
        CHECK(parse_ci_status_responses(repository, "abc", { response }).value->state == entry.expected);
    }
}

TEST_CASE("the current account response is parsed", "[hosting]")
{
    CHECK(*parse_current_user_response(hosting_provider::github, make_response(200, R"({"login": "octocat", "id": 1})")).value == "octocat");
    CHECK(*parse_current_user_response(hosting_provider::gitlab, make_response(200, R"({"username": "root", "id": 1})")).value == "root");
    CHECK(parse_current_user_response(hosting_provider::github, make_response(200, "{}")).error.kind == hosting_error_kind::invalid_response);
    CHECK(parse_current_user_response(hosting_provider::github, make_response(401, R"({"message": "Bad credentials"})")).error.kind == hosting_error_kind::unauthorized);
}
