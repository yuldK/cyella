// 호스팅 연동을 끝까지 돌린다: 실제 git(`git credential`), 실제 luil HTTP client, 이 test가 127.0.0.1에 여는 가짜 GitHub·GitLab API.
// 실제 github.com·gitlab.com에는 닿지 않고 사용자의 자격 증명 도우미(GCM 등)도 읽지 않는다 — git의 전역·시스템 설정을 임시 파일로 갈아
// 끼운다(`GIT_CONFIG_GLOBAL`, `GIT_CONFIG_NOSYSTEM=1`). 모두 `[integration]` 태그를 가지며 git이 없으면 건너뛴다.
#include "fake_hosting_api.h"
#include "git_test_support.h"
#include "infrastructure/git_credential_source.h"
#include "infrastructure/git_execution_policy.h"
#include "infrastructure/git_tool_discovery.h"
#include "infrastructure/hosting_service.h"
#include "infrastructure/luil_hosting_transport.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_process_runner.h"
#include "process_test_support.h"
#include "temporary_directory_support.h"

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace {
    using namespace cyella;
    using namespace cyella::test;

    std::string slashes(const std::filesystem::path& path)
    {
        return path.generic_string();
    }

    // git의 사용자·시스템 설정을 임시 파일로 바꾸고 helper 줄을 적는다.
    struct isolated_git
    {
        std::unique_ptr<process_runner> runner { win32::make_process_runner() };
        std::u8string git {};
        std::filesystem::path config {};

        isolated_git(const std::filesystem::path& directory, const std::string& helper_lines)
            : config { directory / "gitconfig" }
        {
            const std::unique_ptr<file_probe> probe = win32::make_file_probe();
            const git_tool_info info = resolve_git_tool({}, win32::current_git_tool_environment(), *runner, *probe, {});
            if (info.usable())
                git = info.executable;
            write_file(config, "[credential]\n" + helper_lines);
        }

        [[nodiscard]] bool available() const noexcept
        {
            return git.empty() == false;
        }

        [[nodiscard]] git_credential_source_options options() const
        {
            git_credential_source_options options {};
            options.git_executable = git;
            options.timeout = std::chrono::seconds { 30 };
            options.environment = { { u8"GIT_CONFIG_GLOBAL", config.u8string() }, { u8"GIT_CONFIG_NOSYSTEM", u8"1" } };
            return options;
        }

        // 이 환경에서 git이 읽는 credential.helper 전부다. GCM이 끼어들지 않았는지 확인하는 데 쓴다.
        [[nodiscard]] std::string configured_helpers() const
        {
            git_request_options request_options {};
            request_options.raw_standard_output = true;
            request_options.environment = options().environment;
            const process_request request = make_git_process_request(
                git, config.parent_path().u8string(), { u8"config", u8"--show-origin", u8"--get-all", u8"credential.helper" }, git_command_class::local_query, request_options);
            collecting_sink sink {};
            static_cast<void>(runner->run(request, &sink, {}));
            return sink.standard_output_bytes;
        }
    };

    // 가짜 API 서버와 luil 전송이다.
    struct live_api
    {
        std::unique_ptr<fake_hosting_api> api {};
        std::unique_ptr<fake_http_server> server {};
        std::unique_ptr<luil_hosting_transport> transport {};
        hosting_context context {};

        explicit live_api(const fake_hosting_api::flavor flavor)
        {
            const bool github { flavor == fake_hosting_api::flavor::github };
            api = std::make_unique<fake_hosting_api>(flavor, github ? "octo/hello" : "group/sub/hello");
            fake_hosting_api* const raw { api.get() };
            server = std::make_unique<fake_http_server>([raw](const fake_request& request) { return raw->handle(request); });
            api->set_origin(server->origin());
            std::u8string error {};
            transport = luil_hosting_transport::create(u8"cyella-test", false, error);
            REQUIRE(transport != nullptr);
            const std::string host { "127.0.0.1:" + std::to_string(server->port()) };
            const std::string remote { "http://" + host + "/" + (github ? "octo/hello" : "group/sub/hello") + ".git" };
            const std::optional<hosting_repository> repository
                = resolve_hosting_repository(remote, "origin", { { host, github ? hosting_provider::github : hosting_provider::gitlab, server->origin() } });
            REQUIRE(repository.has_value());
            context.repository = *repository;
        }

        ~live_api()
        {
            transport->stop();
        }

        [[nodiscard]] std::string host() const
        {
            return "127.0.0.1:" + std::to_string(server->port());
        }
    };

    std::vector<fake_pull_request> make_pulls(const int open_count, const int closed_count)
    {
        std::vector<fake_pull_request> pulls {};
        for (int index { 1 }; index <= open_count + closed_count; ++index)
        {
            fake_pull_request pull {};
            pull.number = static_cast<std::uint64_t>(index);
            pull.title = "change " + std::to_string(index);
            pull.source_branch = "feature/" + std::to_string(index);
            pull.sha = std::string(40, static_cast<char>('a' + index % 6));
            pull.state = index <= open_count ? "open" : (index % 2 == 0 ? "merged" : "closed");
            pulls.push_back(std::move(pull));
        }
        return pulls;
    }

    std::int64_t now_seconds()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // 파일의 줄 수다 (끝의 빈 줄은 세지 않는다).
    std::size_t line_count(const std::filesystem::path& path)
    {
        const std::string text = read_file(path);
        std::size_t lines { 0 };
        for (const char value : text)
            lines += value == '\n' ? 1 : 0;
        return lines;
    }

    // `get`을 부를 때마다 tokens 파일의 다음 줄을 비밀번호로 주고, `erase`로 지워 달라는 자격 증명을 erased 파일에 쌓는다.
    void write_rotating_helper(const std::filesystem::path& directory, const std::string& tokens)
    {
        write_file(directory / "tokens", tokens);
        write_file(directory / "helper.sh", R"sh(#!/bin/sh
dir="$(dirname "$0")"
case "$1" in
  get)
    n=$(cat "$dir/counter" 2>/dev/null || echo 0)
    n=$((n+1))
    echo "$n" > "$dir/counter"
    token=$(sed -n "${n}p" "$dir/tokens")
    if [ -n "$token" ]; then
      echo "username=helper"
      echo "password=$token"
    fi
    ;;
  erase)
    cat >> "$dir/erased"
    ;;
esac
)sh");
    }
} // namespace

TEST_CASE("integration: a store helper token drives list, create and CI against a fake GitHub", "[hosting][integration]")
{
    temporary_directory directory { "hosting-github" };
    live_api live { fake_hosting_api::flavor::github };
    const std::filesystem::path credentials_file { directory.path() / "creds" };
    write_file(credentials_file, "http://octocat:test-token@" + live.host() + "\n");
    isolated_git git { directory.path(), "\thelper = store --file " + slashes(credentials_file) + "\n" };
    if (git.available() == false)
        SKIP("git is not installed");
    // 사용자의 도우미가 끼어들지 않았음을 확인한다: 읽히는 helper는 이 임시 설정의 store 하나뿐이다.
    const std::string helpers { git.configured_helpers() };
    REQUIRE(helpers.find("store --file") != std::string::npos);
    REQUIRE(helpers.find("manager") == std::string::npos);

    git_credential_source credentials { *git.runner, git.options() };
    hosting_service service { *live.transport, credentials, now_seconds };
    live.api->accept_tokens({ "test-token" });
    live.api->set_pull_requests(make_pulls(35, 4));

    // 로그인 전에는 서버에 아무것도 가지 않는다.
    CHECK(service.list_pull_requests(live.context, {}, {}).error.kind == hosting_error_kind::not_signed_in);
    CHECK(live.api->requests().empty());

    const auto signed_in { service.sign_in(live.context, {}) };
    INFO(signed_in.error.message);
    REQUIRE(signed_in.ok());
    CHECK(*signed_in.value == "octocat");
    CHECK(service.signed_in(live.context.repository.host_key));

    SECTION("목록은 쪽으로 나뉘고 닫힌 것은 토글로 포함한다")
    {
        const auto first { service.list_pull_requests(live.context, {}, {}) };
        REQUIRE(first.ok());
        CHECK(first.value->items.size() == 30);
        REQUIRE(first.value->next_page.has_value());
        CHECK(*first.value->next_page == 2);
        CHECK(first.value->items[0].source_branch == "feature/1");
        CHECK(first.value->items[0].author == "octocat");
        CHECK(first.value->items[0].state == pull_request_state::open);

        const auto second { service.list_pull_requests(live.context, { .page = 2 }, {}) };
        REQUIRE(second.ok());
        CHECK(second.value->items.size() == 5);
        CHECK(second.value->next_page.has_value() == false);

        const auto all { service.list_pull_requests(live.context, { .include_closed = true, .page = 2 }, {}) };
        REQUIRE(all.ok());
        REQUIRE(all.value->items.size() == 9);
        CHECK(all.value->items[5].state == pull_request_state::merged);
        CHECK(all.value->items[6].state == pull_request_state::closed);
        CHECK(all.value->items.back().state == pull_request_state::closed);
    }
    SECTION("PR 생성은 요청 몸을 그대로 서버에 보낸다")
    {
        const auto created { service.create_pull_request(live.context, { .title = "새 기능", .body = "설명\n둘째 줄", .source_branch = "feature/new", .target_branch = "main", .draft = true }, {}) };
        REQUIRE(created.ok());
        CHECK(created.value->number == 40);
        CHECK(created.value->web_url == live.server->origin() + "/octo/hello/pull/40");
        const auto sent { live.api->requests().back() };
        CHECK(sent.method == "POST");
        CHECK(sent.target == "/repos/octo/hello/pulls");
        CHECK(sent.authorization == "Bearer test-token");
        const nlohmann::json body = nlohmann::json::parse(sent.body);
        CHECK(body["title"] == "새 기능");
        CHECK(body["head"] == "feature/new");
        CHECK(body["base"] == "main");
        CHECK(body["body"] == "설명\n둘째 줄");
        CHECK(body["draft"] == true);
        // 서버에 실제로 만들어졌다.
        CHECK(live.api->pull_requests().back().title == "새 기능");
    }
    SECTION("서버가 거절한 생성은 이유를 돌려준다")
    {
        live.api->reject_create_with("A pull request already exists for octo:feature/new.");
        const auto rejected = service.create_pull_request(live.context, { .title = "t", .source_branch = "feature/new", .target_branch = "main" }, {});
        REQUIRE(rejected.ok() == false);
        CHECK(rejected.error.kind == hosting_error_kind::rejected);
        CHECK(rejected.error.message.find("already exists") != std::string::npos);
    }
    SECTION("CI 상태는 check-run에서 읽는다")
    {
        const std::string sha(40, 'a');
        live.api->set_ci(sha, { "failure" });
        const auto status { service.ci_status(live.context, sha, {}) };
        REQUIRE(status.ok());
        CHECK(status.value->state == ci_state::failure);
        REQUIRE(status.value->checks.size() == 1);
        CHECK(status.value->checks[0].name == "build");
        // 서버가 모르는 커밋은 상태 없음이다.
        const auto unknown { service.ci_status(live.context, std::string(40, 'f'), {}) };
        REQUIRE(unknown.ok());
        CHECK(unknown.value->state == ci_state::none);
    }
    SECTION("호출 한도에 닿으면 남은 횟수와 초기화 시각을 말한다")
    {
        live.api->rate_limit_after(static_cast<int>(live.api->requests().size()));
        const auto limited { service.list_pull_requests(live.context, {}, {}) };
        REQUIRE(limited.ok() == false);
        CHECK(limited.error.kind == hosting_error_kind::rate_limited);
        CHECK(limited.error.rate_limit.remaining == 0);
        CHECK(limited.error.rate_limit.reset_at == 4102444800);
        CHECK(describe_hosting_error(limited.error, now_seconds()).find("남은 횟수 0") != std::string::npos);
        // 한도가 풀리기 전에는 서버를 다시 두드리지 않는다.
        const std::size_t before { live.api->requests().size() };
        CHECK(service.list_pull_requests(live.context, {}, {}).error.kind == hosting_error_kind::rate_limited);
        CHECK(live.api->requests().size() == before);
    }
    SECTION("모든 요청이 Bearer 토큰을 달았고 로그아웃하면 잊는다")
    {
        REQUIRE(service.list_pull_requests(live.context, {}, {}).ok());
        for (const fake_recorded_request& request : live.api->requests())
            CHECK(request.authorization == "Bearer test-token");
        service.sign_out(live.context.repository.host_key);
        CHECK(service.list_pull_requests(live.context, {}, {}).error.kind == hosting_error_kind::not_signed_in);
        // 도우미의 저장본은 그대로다.
        CHECK(read_file(credentials_file).find("test-token") != std::string::npos);
    }
}

TEST_CASE("integration: a 401 erases the credential from the helper and retries once with a new token", "[hosting][integration]")
{
    temporary_directory directory { "hosting-rotate" };
    live_api live { fake_hosting_api::flavor::github };
    write_rotating_helper(directory.path(), "old-token\nnew-token\nthird-token\n");
    isolated_git git { directory.path(), "\thelper = !sh \"" + slashes(directory.path() / "helper.sh") + "\"\n" };
    if (git.available() == false)
        SKIP("git is not installed");
    REQUIRE(git.configured_helpers().find("helper.sh") != std::string::npos);
    REQUIRE(git.configured_helpers().find("manager") == std::string::npos);

    git_credential_source credentials { *git.runner, git.options() };
    hosting_service service { *live.transport, credentials, now_seconds };

    SECTION("첫 토큰이 거절되고 둘째가 통과한다")
    {
        live.api->accept_tokens({ "new-token" });
        const auto signed_in { service.sign_in(live.context, {}) };
        INFO(signed_in.error.message);
        REQUIRE(signed_in.ok());
        CHECK(*signed_in.value == "octocat");
        const std::vector<fake_recorded_request> requests { live.api->requests() };
        REQUIRE(requests.size() == 2);
        CHECK(requests[0].authorization == "Bearer old-token");
        CHECK(requests[1].authorization == "Bearer new-token");
        // 도우미는 두 번 물렸고, 지워 달라고 받은 것은 거절된 old-token 하나다.
        CHECK(read_file(directory.path() / "counter").find('2') != std::string::npos);
        const std::string erased = read_file(directory.path() / "erased");
        CHECK(erased.find("password=old-token") != std::string::npos);
        CHECK(erased.find("new-token") == std::string::npos);
        CHECK(erased.find("host=" + live.host()) != std::string::npos);
    }
    SECTION("둘 다 거절되면 세 번째는 묻지 않는다")
    {
        live.api->accept_tokens({ "never" });
        const auto signed_in { service.sign_in(live.context, {}) };
        REQUIRE(signed_in.ok() == false);
        CHECK(signed_in.error.kind == hosting_error_kind::unauthorized);
        CHECK(service.signed_in(live.context.repository.host_key) == false);
        CHECK(live.api->requests().size() == 2);
        CHECK(read_file(directory.path() / "counter").find('3') == std::string::npos);
        const std::string erased = read_file(directory.path() / "erased");
        CHECK(erased.find("password=old-token") != std::string::npos);
        CHECK(erased.find("password=new-token") != std::string::npos);
    }
    SECTION("로그인 뒤 토큰이 만료되면 목록 호출 안에서 한 번 갱신한다")
    {
        live.api->accept_tokens({ "old-token" });
        REQUIRE(service.sign_in(live.context, {}).ok());
        live.api->set_pull_requests(make_pulls(2, 0));
        live.api->accept_tokens({ "new-token" });
        const auto list { service.list_pull_requests(live.context, {}, {}) };
        REQUIRE(list.ok());
        CHECK(list.value->items.size() == 2);
        CHECK(service.signed_in(live.context.repository.host_key));
        CHECK(read_file(directory.path() / "erased").find("password=old-token") != std::string::npos);
    }
}

TEST_CASE("integration: the store helper entry is erased after a 401", "[hosting][integration]")
{
    temporary_directory directory { "hosting-store-reject" };
    live_api live { fake_hosting_api::flavor::github };
    const std::filesystem::path credentials_file { directory.path() / "creds" };
    write_file(credentials_file, "http://octocat:stale-token@" + live.host() + "\n");
    isolated_git git { directory.path(), "\thelper = store --file " + slashes(credentials_file) + "\n" };
    if (git.available() == false)
        SKIP("git is not installed");

    git_credential_source credentials { *git.runner, git.options() };
    hosting_service service { *live.transport, credentials, now_seconds };
    live.api->accept_tokens({ "fresh-token" });
    const auto signed_in { service.sign_in(live.context, {}) };
    REQUIRE(signed_in.ok() == false);
    CHECK(signed_in.error.kind == hosting_error_kind::unauthorized);
    CHECK(read_file(credentials_file).find("stale-token") == std::string::npos);
    CHECK(service.signed_in(live.context.repository.host_key) == false);
    // 서버에는 낡은 토큰으로 한 번만 갔다. 다시 물었을 때 도우미에 남은 것이 없어 재시도는 보내지 못했다.
    CHECK(live.api->requests().size() == 1);
}

TEST_CASE("integration: without a helper sign-in fails at once without prompting", "[hosting][integration]")
{
    temporary_directory directory { "hosting-nohelper" };
    live_api live { fake_hosting_api::flavor::github };
    isolated_git git { directory.path(), "\thelper =\n" };
    if (git.available() == false)
        SKIP("git is not installed");
    git_credential_source credentials { *git.runner, git.options() };
    hosting_service service { *live.transport, credentials, now_seconds };
    const auto signed_in { service.sign_in(live.context, {}) };
    REQUIRE(signed_in.ok() == false);
    CHECK(signed_in.error.kind == hosting_error_kind::credential_unavailable);
    CHECK(live.api->requests().empty());
    CHECK(service.signed_in(live.context.repository.host_key) == false);
}

TEST_CASE("integration: a fake GitLab serves merge request list, create and pipelines", "[hosting][integration]")
{
    temporary_directory directory { "hosting-gitlab" };
    live_api live { fake_hosting_api::flavor::gitlab };
    const std::filesystem::path credentials_file { directory.path() / "creds" };
    write_file(credentials_file, "http://oauth2:glpat-test@" + live.host() + "\n");
    isolated_git git { directory.path(), "\thelper = store --file " + slashes(credentials_file) + "\n" };
    if (git.available() == false)
        SKIP("git is not installed");
    git_credential_source credentials { *git.runner, git.options() };
    hosting_service service { *live.transport, credentials, now_seconds };
    live.api->accept_tokens({ "glpat-test" });
    live.api->set_user("root");
    live.api->set_pull_requests(make_pulls(32, 3));

    const auto signed_in { service.sign_in(live.context, {}) };
    INFO(signed_in.error.message);
    REQUIRE(signed_in.ok());
    CHECK(*signed_in.value == "root");

    const auto first { service.list_pull_requests(live.context, {}, {}) };
    REQUIRE(first.ok());
    CHECK(first.value->items.size() == 30);
    REQUIRE(first.value->next_page.has_value());
    const auto second { service.list_pull_requests(live.context, { .page = *first.value->next_page }, {}) };
    REQUIRE(second.ok());
    CHECK(second.value->items.size() == 2);
    CHECK(second.value->next_page.has_value() == false);
    CHECK(first.value->items[0].number == 1);
    CHECK(first.value->items[0].web_url == live.server->origin() + "/group/sub/hello/-/merge_requests/1");

    const auto created = service.create_pull_request(live.context, { .title = "새 MR", .body = "본문", .source_branch = "feature/mr", .target_branch = "main", .draft = true }, {});
    REQUIRE(created.ok());
    CHECK(created.value->number == 36);
    const nlohmann::json body = nlohmann::json::parse(live.api->requests().back().body);
    CHECK(body["title"] == "Draft: 새 MR");
    CHECK(body["source_branch"] == "feature/mr");
    CHECK(live.api->requests().back().target == "/projects/group%2Fsub%2Fhello/merge_requests");

    live.api->set_ci(std::string(40, 'b'), { "success" });
    const auto pipeline { service.ci_status(live.context, std::string(40, 'b'), {}) };
    REQUIRE(pipeline.ok());
    CHECK(pipeline.value->state == ci_state::success);
    CHECK(pipeline.value->web_url == live.server->origin() + "/group/sub/hello/-/pipelines/77");

    live.api->rate_limit_after(static_cast<int>(live.api->requests().size()));
    const auto limited { service.list_pull_requests(live.context, {}, {}) };
    REQUIRE(limited.ok() == false);
    CHECK(limited.error.kind == hosting_error_kind::rate_limited);
    CHECK(limited.error.status_code == 429);
    CHECK(limited.error.rate_limit.retry_after_seconds == 30);
}
