// 호스팅 조립부(`hosting_bridge`)를 실제 scheduler·git·luil HTTP client와 가짜 API 서버로 돌린다: 일감이 호스트별 lane의 워커에서 돌고 결과가
// `hosting_result_intent`로 돌아오며, 종료할 때 기다리는 요청이 매달리지 않고 토큰이 지워진다. `[integration]` 태그이고 git이 없으면 건너뛴다.
#include "application/cancellation_registry.h"
#include "application/hosting_messages.h"
#include "application/task_scheduler.h"
#include "infrastructure/git_tool_discovery.h"
#include "platform/win32/hosting_bridge.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_process_runner.h"

#include "fake_hosting_api.h"
#include "temporary_directory_support.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

namespace {
    using namespace cyella;
    using namespace cyella::test;
    using namespace std::chrono_literals;

    // relay 대신 결과를 모으는 수신함이다.
    class inbox
    {
    public:
        void post(luil::app_message message)
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            if (const auto* const result { message.get<hosting_result_intent>() }; result != nullptr)
                results_.push_back(*result);
            arrived_.notify_all();
        }

        [[nodiscard]] bool wait_for(const std::size_t count, const std::chrono::seconds timeout = 30s)
        {
            std::unique_lock<std::mutex> lock { mutex_ };
            return arrived_.wait_for(lock, timeout, [&] { return results_.size() >= count; });
        }

        [[nodiscard]] std::vector<hosting_result_intent> results()
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            return results_;
        }

    private:
        std::mutex mutex_ {};
        std::condition_variable arrived_ {};
        std::vector<hosting_result_intent> results_ {};
    };

    struct rig
    {
        temporary_directory directory { "hosting-bridge" };
        std::unique_ptr<process_runner> runner { win32::make_process_runner() };
        task_scheduler scheduler {};
        cancellation_registry registry {};
        inbox mailbox {};
        fake_hosting_api api { fake_hosting_api::flavor::github, "octo/hello" };
        std::unique_ptr<fake_http_server> server {};
        win32::hosting_bridge bridge {};
        hosting_context context {};
        std::filesystem::path credentials_file {};
        bool ready { false };

        rig()
        {
            server = std::make_unique<fake_http_server>([this](const fake_request& request) { return api.handle(request); });
            api.set_origin(server->origin());
            const std::string host { "127.0.0.1:" + std::to_string(server->port()) };
            credentials_file = directory.path() / "creds";
            write_file(credentials_file, "http://octocat:test-token@" + host + "\n");
            write_file(directory.path() / "gitconfig", "[credential]\n\thelper = store --file " + credentials_file.generic_string() + "\n");

            const std::unique_ptr<file_probe> probe { win32::make_file_probe() };
            const git_tool_info git { resolve_git_tool({}, win32::current_git_tool_environment(), *runner, *probe, {}) };
            if (git.usable() == false)
                return;
            win32::hosting_bridge::options options {};
            options.git_executable = git.executable;
            options.use_system_proxy = false;
            options.environment = { { u8"GIT_CONFIG_GLOBAL", (directory.path() / "gitconfig").u8string() }, { u8"GIT_CONFIG_NOSYSTEM", u8"1" } };
            options.post = [this](luil::app_message message) { mailbox.post(std::move(message)); };
            std::u8string error {};
            ready = bridge.start(*runner, &scheduler, &registry, std::move(options), error);
            context.repository = *resolve_hosting_repository("http://" + host + "/octo/hello.git", "origin", { { host, hosting_provider::github, server->origin() } });
        }

        ~rig()
        {
            bridge.cancel_all();
            registry.cancel_all();
            scheduler.shutdown();
            bridge.stop();
        }

        hosting_job job(const std::uint64_t serial, std::variant<hosting_sign_in_job, hosting_list_job, hosting_create_job, hosting_ci_job, hosting_create_repository_job> work) const
        {
            hosting_job value {};
            value.serial = serial;
            value.repository_id = "repo-1";
            value.context = context;
            value.work = std::move(work);
            return value;
        }
    };
} // namespace

TEST_CASE("integration: bridge jobs run on a worker and post results with the sign-in state", "[hosting][integration]")
{
    rig test {};
    if (test.ready == false)
        SKIP("git is not installed");
    test.api.accept_tokens({ "test-token" });
    std::vector<fake_pull_request> pulls {};
    pulls.push_back({ .number = 1, .title = "first", .source_branch = "feature/a", .sha = std::string(40, 'a') });
    test.api.set_pull_requests(pulls);

    // 로그인 전의 목록 일감은 서버에 닿지 않고 not_signed_in으로 돌아온다.
    test.bridge.submit(test.job(1, hosting_list_job {}));
    REQUIRE(test.mailbox.wait_for(1));
    const hosting_result_intent early { test.mailbox.results()[0] };
    REQUIRE(std::holds_alternative<hosting_result<pull_request_page>>(early.payload));
    CHECK(std::get<hosting_result<pull_request_page>>(early.payload).error.kind == hosting_error_kind::not_signed_in);
    CHECK(early.signed_in == false);
    CHECK(test.api.requests().empty());

    test.bridge.submit(test.job(2, hosting_sign_in_job {}));
    test.bridge.submit(test.job(3, hosting_list_job {}));
    test.bridge.submit(test.job(4, hosting_ci_job { std::string(40, 'a') }));
    REQUIRE(test.mailbox.wait_for(4));
    const std::vector<hosting_result_intent> results { test.mailbox.results() };
    // 같은 호스트의 일감은 차례로 돈다.
    CHECK(results[1].serial == 2);
    CHECK(results[2].serial == 3);
    CHECK(results[3].serial == 4);
    const auto& signed_in { std::get<hosting_result<std::string>>(results[1].payload) };
    REQUIRE(signed_in.ok());
    CHECK(*signed_in.value == "octocat");
    CHECK(results[1].signed_in);
    CHECK(results[1].user == "octocat");
    CHECK(results[1].host_key == test.context.repository.host_key);
    const auto& list { std::get<hosting_result<pull_request_page>>(results[2].payload) };
    REQUIRE(list.ok());
    REQUIRE(list.value->items.size() == 1);
    CHECK(list.value->items[0].title == "first");
    CHECK(results[2].rate_limit.remaining == 4999);
    CHECK(std::holds_alternative<hosting_result<ci_status_info>>(results[3].payload));

    // 로그아웃은 메모리 토큰만 잊고 도우미의 저장본은 그대로다.
    test.bridge.sign_out({ test.context.repository.host_key });
    test.bridge.submit(test.job(5, hosting_list_job {}));
    REQUIRE(test.mailbox.wait_for(5));
    CHECK(std::get<hosting_result<pull_request_page>>(test.mailbox.results()[4].payload).error.kind == hosting_error_kind::not_signed_in);
    CHECK(test.mailbox.results()[4].signed_in == false);
    CHECK(read_file(test.credentials_file).find("test-token") != std::string::npos);
}

TEST_CASE("integration: shutdown does not hang on a request that never gets an answer", "[hosting][integration]")
{
    // 연결은 받지만 답하지 않는 서버다.
    std::mutex gate_mutex {};
    std::condition_variable gate {};
    bool release { false };
    std::atomic<int> reached { 0 };
    const auto answer = [&](const fake_request& request) {
        if (request.path() == "/user")
        {
            ++reached;
            std::unique_lock<std::mutex> lock { gate_mutex };
            gate.wait_for(lock, 20s, [&] { return release; });
        }
        return fake_response { .status = 200, .body = R"({"login": "octocat"})" };
    };
    fake_http_server slow { answer };
    {
        temporary_directory directory { "hosting-bridge-slow" };
        std::unique_ptr<process_runner> runner { win32::make_process_runner() };
        const std::unique_ptr<file_probe> probe { win32::make_file_probe() };
        const git_tool_info git { resolve_git_tool({}, win32::current_git_tool_environment(), *runner, *probe, {}) };
        if (git.usable() == false)
        {
            {
                const std::lock_guard<std::mutex> lock { gate_mutex };
                release = true;
            }
            gate.notify_all();
            SKIP("git is not installed");
        }
        const std::string host { "127.0.0.1:" + std::to_string(slow.port()) };
        write_file(directory.path() / "creds", "http://octocat:tok@" + host + "\n");
        write_file(directory.path() / "gitconfig", "[credential]\n\thelper = store --file " + (directory.path() / "creds").generic_string() + "\n");
        task_scheduler scheduler {};
        cancellation_registry registry {};
        inbox mailbox {};
        win32::hosting_bridge bridge {};
        win32::hosting_bridge::options options {};
        options.git_executable = git.executable;
        options.use_system_proxy = false;
        options.environment = { { u8"GIT_CONFIG_GLOBAL", (directory.path() / "gitconfig").u8string() }, { u8"GIT_CONFIG_NOSYSTEM", u8"1" } };
        options.post = [&mailbox](luil::app_message message) { mailbox.post(std::move(message)); };
        std::u8string error {};
        REQUIRE(bridge.start(*runner, &scheduler, &registry, std::move(options), error));
        hosting_job job {};
        job.serial = 1;
        job.repository_id = "repo-1";
        job.context.repository = *resolve_hosting_repository("http://" + host + "/octo/hello.git", "origin", { { host, hosting_provider::github, "http://" + host } });
        job.work = hosting_sign_in_job {};
        bridge.submit(job);
        const auto deadline { std::chrono::steady_clock::now() + 20s };
        while (reached.load() == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(20ms);
        REQUIRE(reached.load() > 0);

        // 종료 순서: 요청을 접고 워커를 멈추고 전송을 닫는다. 서버는 아직 답하지 않았다.
        const auto started { std::chrono::steady_clock::now() };
        bridge.cancel_all();
        registry.cancel_all();
        scheduler.shutdown();
        bridge.stop();
        CHECK(std::chrono::steady_clock::now() - started < 10s);
        // 워커는 취소된 결과를 남기고 끝났다.
        REQUIRE(mailbox.wait_for(1, 5s));
        CHECK(mailbox.results()[0].signed_in == false);
    }
    {
        const std::lock_guard<std::mutex> lock { gate_mutex };
        release = true;
    }
    gate.notify_all();
}
