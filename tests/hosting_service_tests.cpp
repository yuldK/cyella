#include "application/process_runner.h"
#include "infrastructure/git_credential_source.h"
#include "infrastructure/hosting_service.h"
#include "infrastructure/secret_masking.h"

#include <catch2/catch_test_macros.hpp>

#include <deque>
#include <string>
#include <vector>

using namespace cyella;

namespace {
    // 보낸 요청과 그때 붙은 토큰을 기록하고 준비한 응답을 차례로 돌려준다.
    class scripted_transport final : public hosting_transport
    {
    public:
        struct sent
        {
            hosting_http_request request {};
            std::string bearer {};
        };

        hosting_http_response execute(const hosting_http_request& request, const secret_text& bearer, const process_cancellation_token&) override
        {
            requests.push_back({ request, bearer.value() });
            if (responses.empty())
            {
                hosting_http_response none {};
                none.transport_error = "script exhausted";
                return none;
            }
            hosting_http_response response { std::move(responses.front()) };
            responses.pop_front();
            return response;
        }

        std::vector<sent> requests {};
        std::deque<hosting_http_response> responses {};
    };

    class scripted_credentials final : public credential_source
    {
    public:
        credential_fill_result fill(const credential_query& query, const process_cancellation_token&) override
        {
            queries.push_back(query);
            credential_fill_result result {};
            if (tokens.empty())
            {
                result.error = "no helper";
                return result;
            }
            result.ok = true;
            result.username = "helper-user";
            result.token = secret_text { tokens.front() };
            tokens.pop_front();
            return result;
        }

        void reject(const credential_query&, const std::string_view username, const secret_text& token, const process_cancellation_token&) override
        {
            rejected.push_back({ std::string { username }, token.value() });
        }

        std::deque<std::string> tokens {};
        std::vector<credential_query> queries {};
        std::vector<std::pair<std::string, std::string>> rejected {};
    };

    hosting_http_response response_of(const int status, std::string body, std::vector<hosting_http_header> headers = {})
    {
        hosting_http_response response {};
        response.status_code = status;
        response.body = std::move(body);
        response.headers = std::move(headers);
        return response;
    }

    struct fixture
    {
        scripted_transport transport {};
        scripted_credentials credentials {};
        std::int64_t now { 1700000000 };
        hosting_service service { transport, credentials, [this] { return now; } };
        hosting_context context {};

        fixture()
        {
            context.repository = *resolve_hosting_repository("http://127.0.0.1:8123/octo/hello.git", "origin", { { "127.0.0.1:8123", hosting_provider::github, "http://127.0.0.1:8123" } });
            context.working_directory = "C:\\work\\hello";
        }
    };

    const char* const empty_list { "[]" };
} // namespace

TEST_CASE("without sign-in no request is sent", "[hosting]")
{
    fixture f {};
    const auto result { f.service.list_pull_requests(f.context, {}, {}) };
    REQUIRE(result.ok() == false);
    CHECK(result.error.kind == hosting_error_kind::not_signed_in);
    CHECK(f.transport.requests.empty());
    CHECK(f.credentials.queries.empty());
    CHECK(f.service.signed_in(f.context.repository.host_key) == false);
}

TEST_CASE("a remote with an unknown provider cannot be integrated", "[hosting]")
{
    fixture f {};
    f.context.repository = *resolve_hosting_repository("https://git.example.com/a/b.git", "origin", {});
    CHECK(f.service.sign_in(f.context, {}).error.kind == hosting_error_kind::unsupported);
    CHECK(f.service.list_pull_requests(f.context, {}, {}).error.kind == hosting_error_kind::unsupported);
    CHECK(f.credentials.queries.empty());
}

TEST_CASE("sign-in takes a token from the helper and verifies the account", "[hosting]")
{
    fixture f {};
    f.credentials.tokens = { "tok-1" };
    f.transport.responses.push_back(response_of(200, R"({"login": "octocat"})"));
    const auto result { f.service.sign_in(f.context, {}) };
    REQUIRE(result.ok());
    CHECK(*result.value == "octocat");

    // 도우미에는 프로토콜·호스트·저장소 폴더만 줬다.
    REQUIRE(f.credentials.queries.size() == 1);
    CHECK(f.credentials.queries[0].protocol == "http");
    CHECK(f.credentials.queries[0].host == "127.0.0.1:8123");
    CHECK(f.credentials.queries[0].working_directory == "C:\\work\\hello");
    REQUIRE(f.transport.requests.size() == 1);
    CHECK(f.transport.requests[0].request.url == "http://127.0.0.1:8123/user");
    CHECK(f.transport.requests[0].bearer == "tok-1");

    CHECK(f.service.signed_in(f.context.repository.host_key));
    // 도우미가 준 이름이 아니라 서버가 답한 계정이다.
    CHECK(f.service.signed_in_user(f.context.repository.host_key) == "octocat");

    // 이후 호출은 도우미에 묻지 않고 메모리의 토큰을 쓴다.
    f.transport.responses.push_back(response_of(200, empty_list));
    REQUIRE(f.service.list_pull_requests(f.context, {}, {}).ok());
    CHECK(f.credentials.queries.size() == 1);
    CHECK(f.transport.requests[1].bearer == "tok-1");
}

TEST_CASE("sign-in stays signed out when the helper gives no token", "[hosting]")
{
    fixture f {};
    const auto result { f.service.sign_in(f.context, {}) };
    REQUIRE(result.ok() == false);
    CHECK(result.error.kind == hosting_error_kind::credential_unavailable);
    CHECK(result.error.message == "no helper");
    CHECK(f.transport.requests.empty());
    CHECK(f.service.signed_in(f.context.repository.host_key) == false);
}

TEST_CASE("a rejected sign-in check forgets the token", "[hosting]")
{
    fixture f {};
    f.credentials.tokens = { "bad-1", "bad-2" };
    f.transport.responses.push_back(response_of(401, R"({"message": "Bad credentials"})"));
    f.transport.responses.push_back(response_of(401, R"({"message": "Bad credentials"})"));
    const auto result { f.service.sign_in(f.context, {}) };
    REQUIRE(result.ok() == false);
    CHECK(result.error.kind == hosting_error_kind::unauthorized);
    CHECK(f.service.signed_in(f.context.repository.host_key) == false);
    // 거절된 토큰은 모두 도우미에서 지워 달라고 했다.
    REQUIRE(f.credentials.rejected.size() == 2);
    CHECK(f.credentials.rejected[0].second == "bad-1");
    CHECK(f.credentials.rejected[1].second == "bad-2");
}

TEST_CASE("a 401 rejects the credential and asks once more", "[hosting]")
{
    fixture f {};
    f.credentials.tokens = { "old", "new" };
    f.transport.responses.push_back(response_of(200, R"({"login": "octocat"})"));
    REQUIRE(f.service.sign_in(f.context, {}).ok());

    f.transport.responses.push_back(response_of(401, R"({"message": "Bad credentials"})"));
    f.transport.responses.push_back(response_of(200, R"([{"number": 5, "title": "T", "state": "open", "user": {"login": "a"}, "head": {"ref": "h"}, "base": {"ref": "b"}}])"));
    const auto result { f.service.list_pull_requests(f.context, {}, {}) };
    REQUIRE(result.ok());
    REQUIRE(result.value->items.size() == 1);

    // 요청은 old로 한 번, new로 한 번이다.
    REQUIRE(f.transport.requests.size() == 3);
    CHECK(f.transport.requests[1].bearer == "old");
    CHECK(f.transport.requests[2].bearer == "new");
    CHECK(f.transport.requests[1].request.url == f.transport.requests[2].request.url);
    REQUIRE(f.credentials.rejected.size() == 1);
    CHECK(f.credentials.rejected[0].second == "old");
    CHECK(f.credentials.rejected[0].first == "octocat");
    CHECK(f.credentials.queries.size() == 2);
    CHECK(f.service.signed_in(f.context.repository.host_key));
}

TEST_CASE("two 401s in a row forget the token and end as unauthorized", "[hosting]")
{
    fixture f {};
    f.credentials.tokens = { "old", "again", "never-asked" };
    f.transport.responses.push_back(response_of(200, R"({"login": "octocat"})"));
    REQUIRE(f.service.sign_in(f.context, {}).ok());

    f.transport.responses.push_back(response_of(401, "{}"));
    f.transport.responses.push_back(response_of(401, "{}"));
    const auto result { f.service.list_pull_requests(f.context, {}, {}) };
    REQUIRE(result.ok() == false);
    CHECK(result.error.kind == hosting_error_kind::unauthorized);
    CHECK(f.service.signed_in(f.context.repository.host_key) == false);
    // 세 번째 질문은 없다: 재시도는 한 번이다.
    CHECK(f.credentials.queries.size() == 2);
    CHECK(f.credentials.tokens.size() == 1);
    CHECK(f.transport.requests.size() == 3);
    REQUIRE(f.credentials.rejected.size() == 2);
    CHECK(f.credentials.rejected[1].second == "again");
}

TEST_CASE("a helper that gives nothing after a 401 ends as unauthorized", "[hosting]")
{
    fixture f {};
    f.credentials.tokens = { "only" };
    f.transport.responses.push_back(response_of(200, R"({"login": "octocat"})"));
    REQUIRE(f.service.sign_in(f.context, {}).ok());
    f.transport.responses.push_back(response_of(401, "{}"));
    const auto result { f.service.list_pull_requests(f.context, {}, {}) };
    REQUIRE(result.ok() == false);
    CHECK(result.error.kind == hosting_error_kind::unauthorized);
    CHECK(f.service.signed_in(f.context.repository.host_key) == false);
    CHECK(f.transport.requests.size() == 2);
}

TEST_CASE("an exhausted rate limit blocks requests until the reset time", "[hosting]")
{
    fixture f {};
    f.credentials.tokens = { "tok" };
    f.transport.responses.push_back(response_of(200, R"({"login": "octocat"})"));
    REQUIRE(f.service.sign_in(f.context, {}).ok());

    f.transport.responses.push_back(
        response_of(403, R"({"message": "API rate limit exceeded"})", { { "x-ratelimit-limit", "60" }, { "x-ratelimit-remaining", "0" }, { "x-ratelimit-reset", std::to_string(f.now + 900) } }));
    const auto limited { f.service.list_pull_requests(f.context, {}, {}) };
    REQUIRE(limited.ok() == false);
    CHECK(limited.error.kind == hosting_error_kind::rate_limited);
    CHECK(limited.error.rate_limit.remaining == 0);
    CHECK(limited.error.rate_limit.reset_at == f.now + 900);
    CHECK(describe_hosting_error(limited.error, f.now).find("15분 후") != std::string::npos);
    CHECK(f.service.last_rate_limit(f.context.repository.host_key).remaining == 0);

    // 한도가 풀리기 전에는 서버를 두드리지 않는다.
    const std::size_t sent { f.transport.requests.size() };
    const auto blocked { f.service.list_pull_requests(f.context, {}, {}) };
    CHECK(blocked.error.kind == hosting_error_kind::rate_limited);
    CHECK(f.transport.requests.size() == sent);

    // 시각이 지나면 다시 보낸다.
    f.now += 901;
    f.transport.responses.push_back(response_of(200, empty_list, { { "x-ratelimit-remaining", "59" }, { "x-ratelimit-reset", std::to_string(f.now + 3600) } }));
    CHECK(f.service.list_pull_requests(f.context, {}, {}).ok());
    CHECK(f.transport.requests.size() == sent + 1);
    CHECK(f.service.last_rate_limit(f.context.repository.host_key).remaining == 59);
}

TEST_CASE("sign-out forgets only the in-memory token", "[hosting]")
{
    fixture f {};
    f.credentials.tokens = { "tok" };
    f.transport.responses.push_back(response_of(200, R"({"login": "octocat"})"));
    REQUIRE(f.service.sign_in(f.context, {}).ok());
    REQUIRE(f.service.signed_in_hosts().size() == 1);

    f.service.sign_out(f.context.repository.host_key);
    CHECK(f.service.signed_in(f.context.repository.host_key) == false);
    CHECK(f.service.signed_in_hosts().empty());
    // 도우미의 저장본은 건드리지 않는다.
    CHECK(f.credentials.rejected.empty());
    CHECK(f.service.list_pull_requests(f.context, {}, {}).error.kind == hosting_error_kind::not_signed_in);

    f.credentials.tokens = { "tok-2" };
    f.transport.responses.push_back(response_of(200, R"({"login": "octocat"})"));
    REQUIRE(f.service.sign_in(f.context, {}).ok());
    f.service.sign_out_all();
    CHECK(f.service.signed_in_hosts().empty());
}

TEST_CASE("create sends a JSON body and returns the number and url", "[hosting]")
{
    fixture f {};
    f.credentials.tokens = { "tok" };
    f.transport.responses.push_back(response_of(200, R"({"login": "octocat"})"));
    REQUIRE(f.service.sign_in(f.context, {}).ok());

    f.transport.responses.push_back(response_of(201, R"({"number": 9, "title": "T", "html_url": "http://127.0.0.1:8123/octo/hello/pull/9", "head": {"sha": "abc"}})"));
    const auto created { f.service.create_pull_request(f.context, { .title = "T", .body = "B", .source_branch = "feature", .target_branch = "main", .draft = true }, {}) };
    REQUIRE(created.ok());
    CHECK(created.value->number == 9);
    CHECK(created.value->web_url == "http://127.0.0.1:8123/octo/hello/pull/9");
    const auto& request { f.transport.requests.back().request };
    CHECK(request.method == hosting_http_method::post);
    CHECK(request.url == "http://127.0.0.1:8123/repos/octo/hello/pulls");
    CHECK(request.body.find("\"head\":\"feature\"") != std::string::npos);
}

TEST_CASE("CI status sends every needed request", "[hosting]")
{
    fixture f {};
    f.credentials.tokens = { "tok" };
    f.transport.responses.push_back(response_of(200, R"({"login": "octocat"})"));
    REQUIRE(f.service.sign_in(f.context, {}).ok());
    f.transport.responses.push_back(response_of(200, R"({"state": "success", "total_count": 1, "statuses": [{"state": "success", "context": "ci"}]})"));
    f.transport.responses.push_back(response_of(200, R"({"total_count": 0, "check_runs": []})"));
    const auto status { f.service.ci_status(f.context, "abc", {}) };
    REQUIRE(status.ok());
    CHECK(status.value->state == ci_state::success);
    CHECK(f.transport.requests.size() == 3);
}

TEST_CASE("the token is never part of the URL, headers or body of a request", "[hosting][secrets]")
{
    fixture f {};
    f.credentials.tokens = { "SECRET-TOKEN-VALUE" };
    f.transport.responses.push_back(response_of(200, R"({"login": "octocat"})"));
    f.transport.responses.push_back(response_of(200, empty_list));
    REQUIRE(f.service.sign_in(f.context, {}).ok());
    REQUIRE(f.service.list_pull_requests(f.context, {}, {}).ok());
    for (const auto& sent : f.transport.requests)
    {
        CHECK(sent.bearer == "SECRET-TOKEN-VALUE");
        CHECK(sent.request.url.find("SECRET") == std::string::npos);
        CHECK(sent.request.body.find("SECRET") == std::string::npos);
        for (const hosting_http_header& header : sent.request.headers)
        {
            CHECK(header.value.find("SECRET") == std::string::npos);
            CHECK(header.name != "Authorization");
        }
    }
}

// ---------------------------------------------------------------- git credential 어댑터

namespace {
    // 실행한 요청을 기록하고 준비한 표준 출력을 돌려주는 process runner다.
    class scripted_runner final : public process_runner
    {
    public:
        process_result run(const process_request& request, process_output_sink* sink, const process_cancellation_token&) noexcept override
        {
            requests.push_back(request);
            if (sink != nullptr && standard_output.empty() == false)
                sink->on_chunk({ .sequence = 1, .stream = process_stream::standard_output, .bytes = standard_output });
            process_result result {};
            result.completion = completion;
            if (completion == process_completion::exited)
                result.exit_code = exit_code;
            return result;
        }

        std::vector<process_request> requests {};
        std::string standard_output {};
        process_completion completion { process_completion::exited };
        std::int32_t exit_code { 0 };
    };

    const process_environment_override* find_environment(const process_request& request, const std::u8string_view name)
    {
        for (const process_environment_override& entry : request.environment_overrides)
            if (entry.name == name)
                return &entry;
        return nullptr;
    }
} // namespace

TEST_CASE("credential input blocks line breaks and writes fields in order", "[hosting][secrets]")
{
    credential_query query {};
    query.host = "github.com";
    CHECK(encode_credential_input(query) == "protocol=https\nhost=github.com\n\n");
    CHECK(encode_credential_input(query, "u", "p") == "protocol=https\nhost=github.com\nusername=u\npassword=p\n\n");
    query.host = "github.com\nusername=evil";
    CHECK(encode_credential_input(query).has_value() == false);
    query.host = "github.com";
    CHECK(encode_credential_input(query, "u", "p\nhost=x").has_value() == false);
    query.host.clear();
    CHECK(encode_credential_input(query).has_value() == false);
}

TEST_CASE("only the password of git credential fill output is kept as a secret", "[hosting][secrets]")
{
    parsed_credential_output parsed { parse_credential_output("protocol=https\r\nhost=github.com\r\nusername=alice\r\npassword=gho_abc\r\noauth_refresh_token=refresh\r\npassword_expiry_utc=1\r\n") };
    CHECK(parsed.username == "alice");
    CHECK(parsed.has_password);
    CHECK(parsed.password.value() == "gho_abc");
    CHECK(parse_credential_output("username=alice\n").has_password == false);
    CHECK(parse_credential_output("password=\n").has_password == false);
    CHECK(parse_credential_output("").has_password == false);

    secret_text moved { std::move(parsed.password) };
    CHECK(moved.value() == "gho_abc");
    CHECK(parsed.password.empty());
    moved.wipe();
    CHECK(moved.empty());
}

TEST_CASE("fill neither leaks to the terminal nor puts the token on the command line", "[hosting][secrets]")
{
    scripted_runner runner {};
    runner.standard_output = "username=alice\npassword=SECRET-TOKEN-VALUE\n";
    git_credential_source source { runner, { .git_executable = u8"C:\\git\\git.exe", .environment = { { u8"GIT_CONFIG_GLOBAL", u8"C:\\temp\\gitconfig" } } } };
    credential_query query {};
    query.host = "github.com";
    query.working_directory = "C:\\work\\repo";
    credential_fill_result result { source.fill(query, {}) };
    REQUIRE(result.ok);
    CHECK(result.username == "alice");
    CHECK(result.token.value() == "SECRET-TOKEN-VALUE");

    REQUIRE(runner.requests.size() == 1);
    const process_request& request { runner.requests[0] };
    CHECK(request.executable == u8"C:\\git\\git.exe");
    CHECK(request.working_directory == u8"C:\\work\\repo");
    REQUIRE(request.arguments.size() >= 2);
    CHECK(request.arguments[request.arguments.size() - 2] == u8"credential");
    CHECK(request.arguments.back() == u8"fill");
    for (const std::u8string& argument : request.arguments)
        CHECK(argument.find(u8"SECRET") == std::u8string::npos);
    // 터미널 질문을 막고 사용자 환경의 askpass를 지운다 (cyella 인증 대화 상자로도 새지 않는다).
    REQUIRE(find_environment(request, u8"GIT_TERMINAL_PROMPT") != nullptr);
    CHECK(find_environment(request, u8"GIT_TERMINAL_PROMPT")->value == u8"0");
    for (const char8_t* const name : { u8"GIT_ASKPASS", u8"SSH_ASKPASS" })
    {
        const process_environment_override* entry { find_environment(request, name) };
        REQUIRE(entry != nullptr);
        CHECK(entry->value.has_value() == false);
    }
    REQUIRE(find_environment(request, u8"GIT_CONFIG_GLOBAL") != nullptr);
    CHECK(request.standard_output_mode == process_output_mode::raw);
}

TEST_CASE("fill failures come back with a reason", "[hosting][secrets]")
{
    scripted_runner runner {};
    git_credential_source source { runner, { .git_executable = u8"C:\\git\\git.exe" } };
    credential_query query {};
    query.host = "github.com";

    runner.exit_code = 128;
    CHECK(source.fill(query, {}).ok == false);
    runner.exit_code = 0;
    runner.standard_output = "username=alice\n";
    CHECK(source.fill(query, {}).error.find("비밀번호") != std::string::npos);
    runner.completion = process_completion::cancelled;
    const credential_fill_result cancelled { source.fill(query, {}) };
    CHECK(cancelled.cancelled);
    CHECK(cancelled.ok == false);
    runner.completion = process_completion::timed_out;
    CHECK(source.fill(query, {}).error.find("제때") != std::string::npos);
    query.host = "bad\nhost";
    CHECK(source.fill(query, {}).ok == false);
}

TEST_CASE("reject passes the refused credential through standard input only", "[hosting][secrets]")
{
    scripted_runner runner {};
    git_credential_source source { runner, { .git_executable = u8"C:\\git\\git.exe" } };
    credential_query query {};
    query.host = "github.com";
    const secret_text token { "SECRET-TOKEN-VALUE" };
    source.reject(query, "alice", token, {});
    REQUIRE(runner.requests.size() == 1);
    CHECK(runner.requests[0].arguments.back() == u8"reject");
    for (const std::u8string& argument : runner.requests[0].arguments)
        CHECK(argument.find(u8"SECRET") == std::u8string::npos);
    // 입력은 실행 뒤 지워진다 (기록해 둔 사본이 아니라 요청 값 자체를 보는 것은 runner가 참조로 받기 때문이다).
}

TEST_CASE("masking hides hosting tokens wherever they could reach a log or a command line", "[hosting][secrets]")
{
    // 요청 헤더, 서비스별 token 접두어, URL의 사용자 정보, `git credential`의 입출력 줄은 모두 값을 가린다.
    CHECK(mask_secrets(u8"Authorization: Bearer gho_0123456789abcdef") == u8"Authorization: ***");
    CHECK(mask_secrets(u8"PRIVATE-TOKEN: glpat-abc_DEF-123") == u8"PRIVATE-TOKEN: ***");
    CHECK(mask_secrets(u8"fetch https://oauth2:0123456789abcdef@gitlab.com/g/p.git") == u8"fetch https://oauth2:***@gitlab.com/g/p.git");
    CHECK(mask_secrets(u8"token ghp_0123456789abcdef used").find(u8"0123456789abcdef") == std::u8string::npos);
    CHECK(mask_secrets(u8"protocol=https\nhost=github.com\nusername=alice\npassword=SECRET-VALUE\n\n") == u8"protocol=https\nhost=github.com\nusername=alice\npassword=***\n\n");
    CHECK(mask_secrets(u8"oauth_refresh_token=REFRESH-VALUE") == u8"oauth_refresh_token=***");
    // 비밀이 아닌 글은 그대로 둔다.
    CHECK(mask_secrets(u8"git credential fill") == u8"git credential fill");
    CHECK(mask_secrets(u8"passwordless sign-in") == u8"passwordless sign-in");
}

TEST_CASE("a throttled response with only Retry-After also pauses requests", "[hosting]")
{
    fixture f {};
    f.credentials.tokens = { "tok" };
    f.transport.responses.push_back(response_of(200, R"({"login": "octocat"})"));
    REQUIRE(f.service.sign_in(f.context, {}).ok());
    f.transport.responses.push_back(response_of(429, "Retry later", { { "Retry-After", "30" } }));
    const auto limited { f.service.list_pull_requests(f.context, {}, {}) };
    REQUIRE(limited.ok() == false);
    CHECK(limited.error.kind == hosting_error_kind::rate_limited);
    CHECK(limited.error.rate_limit.retry_after_seconds == 30);

    const std::size_t sent { f.transport.requests.size() };
    const auto blocked { f.service.list_pull_requests(f.context, {}, {}) };
    CHECK(blocked.error.kind == hosting_error_kind::rate_limited);
    CHECK(f.transport.requests.size() == sent);
    CHECK(describe_hosting_error(blocked.error, f.now).find("30초 후") != std::string::npos);

    f.now += 31;
    f.transport.responses.push_back(response_of(200, empty_list));
    CHECK(f.service.list_pull_requests(f.context, {}, {}).ok());
}
