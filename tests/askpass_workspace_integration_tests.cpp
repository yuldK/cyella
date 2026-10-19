// 실제 git, 실제 askpass 서버와 도우미, 실제 scheduler로 인증 흐름을 끝까지 돌린다. 원격은 이 test가
// 127.0.0.1에 여는 작은 HTTP 서버다. 모든 요청에 401(Basic)로 답하고, 맞는 Authorization 헤더가 오면
// 빈 저장소의 smart-HTTP `info/refs`를 돌려준다. 모두 `[integration]` 태그를 가지며 git이 없으면
// 건너뛴다.
#include <winsock2.h>
#include <ws2tcpip.h>

#include "application/git_workspace.h"
#include "application/task_scheduler.h"
#include "infrastructure/git_operation_runner.h"
#include "infrastructure/git_tool_discovery.h"
#include "platform/win32/askpass_bridge.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_process_runner.h"
#include "platform/win32/win32_workspace_files.h"

#include "askpass_test_support.h"
#include "fake_git_runner.h"
#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
    using namespace cyella;
    using namespace std::chrono_literals;

    // base64("alice:pw")다. git이 Basic 인증으로 보내는 값이다.
    constexpr std::string_view alice_credentials { "YWxpY2U6cHc=" };
    constexpr std::chrono::seconds step_timeout { 60 };

    struct received_request
    {
        std::string request_line {};
        // `Authorization` 헤더 값이다. 없으면 비어 있다.
        std::string authorization {};
    };

    std::string pkt_line(const std::string& payload)
    {
        char length[5] {};
        snprintf(length, sizeof(length), "%04zx", payload.size() + 4);
        return std::string { length } + payload;
    }

    // 모든 요청에 401을 돌려주고, 맞는 자격 증명이 오면 빈 저장소의 info/refs를 돌려주는 서버다.
    class basic_auth_http_server
    {
    public:
        basic_auth_http_server()
        {
            WSADATA data {};
            REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
            listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            REQUIRE(listener_ != INVALID_SOCKET);
            sockaddr_in address {};
            address.sin_family = AF_INET;
            address.sin_port = 0;
            REQUIRE(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
            REQUIRE(bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
            REQUIRE(listen(listener_, 8) == 0);
            int length { sizeof(address) };
            REQUIRE(getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &length) == 0);
            port_ = ntohs(address.sin_port);
            thread_ = std::thread { [this] { serve(); } };
        }

        basic_auth_http_server(const basic_auth_http_server&) = delete;
        basic_auth_http_server& operator=(const basic_auth_http_server&) = delete;

        ~basic_auth_http_server()
        {
            stopping_.store(true);
            if (thread_.joinable())
                thread_.join();
            closesocket(listener_);
            WSACleanup();
        }

        [[nodiscard]] std::uint16_t port() const noexcept
        {
            return port_;
        }

        [[nodiscard]] std::string url() const
        {
            return "http://127.0.0.1:" + std::to_string(port_) + "/repo.git";
        }

        [[nodiscard]] std::vector<received_request> requests()
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            return requests_;
        }

        [[nodiscard]] bool received_authorization(const std::string_view credentials)
        {
            for (const received_request& request : requests())
                if (request.authorization == "Basic " + std::string { credentials })
                    return true;
            return false;
        }

        [[nodiscard]] bool received_any_authorization()
        {
            for (const received_request& request : requests())
                if (request.authorization.empty() == false)
                    return true;
            return false;
        }

    private:
        void serve()
        {
            while (stopping_.load() == false)
            {
                fd_set readable {};
                FD_ZERO(&readable);
                FD_SET(listener_, &readable);
                timeval wait { 0, 50 * 1000 };
                if (select(0, &readable, nullptr, nullptr, &wait) <= 0)
                    continue;
                SOCKET connection { accept(listener_, nullptr, nullptr) };
                if (connection == INVALID_SOCKET)
                    continue;
                handle(connection);
                closesocket(connection);
            }
        }

        void handle(const SOCKET connection)
        {
            const DWORD receive_timeout { 3000 };
            setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receive_timeout), sizeof(receive_timeout));

            std::string head {};
            char buffer[2048] {};
            while (head.find("\r\n\r\n") == std::string::npos)
            {
                const int received { recv(connection, buffer, sizeof(buffer), 0) };
                if (received <= 0)
                    return;
                head.append(buffer, static_cast<std::size_t>(received));
                if (head.size() > 64 * 1024)
                    return;
            }

            received_request request {};
            request.request_line = head.substr(0, head.find("\r\n"));
            constexpr std::string_view header_name { "\r\nauthorization:" };
            std::string lowered { head };
            for (char& letter : lowered)
                letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
            if (const std::size_t at { lowered.find(header_name) }; at != std::string::npos)
            {
                const std::size_t start { at + header_name.size() };
                const std::size_t end { head.find("\r\n", start) };
                std::string value { head.substr(start, end - start) };
                const std::size_t first { value.find_first_not_of(' ') };
                request.authorization = first == std::string::npos ? std::string {} : value.substr(first);
            }
            {
                const std::lock_guard<std::mutex> lock { mutex_ };
                requests_.push_back(request);
            }

            std::string response {};
            const bool authorized { request.authorization == "Basic " + std::string { alice_credentials } };
            if (authorized && request.request_line.find("/info/refs?service=git-upload-pack") != std::string::npos)
            {
                // 빈 저장소의 v0 광고다. ref가 없으면 `capabilities^{}` 줄 하나만 있다.
                std::string body { pkt_line("# service=git-upload-pack\n") + "0000" };
                body += pkt_line(std::string { "0000000000000000000000000000000000000000 capabilities^{}" } + '\0' + "multi_ack side-band-64k ofs-delta agent=cyella-test\n");
                body += "0000";
                response = "HTTP/1.1 200 OK\r\nContent-Type: application/x-git-upload-pack-advertisement\r\nCache-Control: no-cache\r\nContent-Length: " + std::to_string(body.size())
                    + "\r\nConnection: close\r\n\r\n" + body;
            }
            else if (authorized)
            {
                response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            }
            else
            {
                response = "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"x\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            }
            send(connection, response.data(), static_cast<int>(response.size()), 0);
            shutdown(connection, SD_SEND);
        }

        SOCKET listener_ { INVALID_SOCKET };
        std::uint16_t port_ { 0 };
        std::atomic<bool> stopping_ { false };
        std::mutex mutex_ {};
        std::vector<received_request> requests_ {};
        std::thread thread_ {};
    };

    // 실제 구성이다. job은 scheduler의 worker에서 돌고 사건은 test 스레드가 `pump`에서 반영한다.
    class live_auth_workspace
    {
    public:
        live_auth_workspace()
            : process_ { win32::make_process_runner() }
            , files_ { win32::make_workspace_files() }
            , bridge_ { events_.sink() }
        {
            const std::unique_ptr<file_probe> probe { win32::make_file_probe() };
            tool_ = resolve_git_tool({}, win32::current_git_tool_environment(), *process_, *probe, {});
            REQUIRE(tool_.usable());
            bridge_.start(test::askpass_helper_path());
            REQUIRE(bridge_.available());

            git_operation_runner_options runner_options {};
            runner_options.askpass_helper = bridge_.helper_path();
            runner_options.askpass = bridge_.endpoint();
            executor_ = std::make_unique<git_operation_runner>(*process_, tool_.executable, *files_, runner_options);
            scheduler_ = std::make_unique<task_scheduler>();
            git_workspace_options workspace_options {};
            workspace_options.refresh_on_open = false;
            workspace_options.askpass = bridge_.service();
            task_scheduler* const scheduler { scheduler_.get() };
            workspace_
                = std::make_unique<git_workspace>(*executor_, [scheduler](operation_request request) { return scheduler->submit(std::move(request)); }, events_.sink(), nullptr, workspace_options);
        }

        live_auth_workspace(const live_auth_workspace&) = delete;
        live_auth_workspace& operator=(const live_auth_workspace&) = delete;

        // 실패한 test가 프롬프트에서 멈춘 git을 남기지 않게 모두 정리한 뒤 닫는다.
        ~live_auth_workspace()
        {
            for (const operation_id id : workspace_->operation_order())
            {
                const operation_status* const status { workspace_->find_operation(id) };
                if (status != nullptr && status->finished() == false)
                    static_cast<void>(workspace_->cancel(id));
            }
            bridge_.shutdown();
            scheduler_->shutdown();
        }

        [[nodiscard]] git_workspace& workspace() noexcept
        {
            return *workspace_;
        }

        [[nodiscard]] win32::askpass_bridge& bridge() noexcept
        {
            return bridge_;
        }

        std::string open(const std::filesystem::path& path)
        {
            return workspace_->open_repository(path.generic_u8string());
        }

        // 조건이 참이 될 때까지 사건을 반영한다. 시간 안에 되지 않으면 test를 실패시킨다.
        void pump_until(const std::function<bool()>& condition)
        {
            const auto deadline { std::chrono::steady_clock::now() + step_timeout };
            while (std::chrono::steady_clock::now() < deadline)
            {
                for (const operation_event& event : events_.take())
                    workspace_->apply(event);
                if (condition())
                    return;
                std::this_thread::sleep_for(5ms);
            }
            FAIL("timed out waiting for the operation");
        }

        void wait_for_prompt(const operation_id id, const credential_prompt_kind kind)
        {
            pump_until([this, id, kind] {
                const operation_status* const status { workspace_->find_operation(id) };
                return status != nullptr && status->state == operation_state::needs_input && status->needs_input.has_value() && status->needs_input->prompt.kind == kind;
            });
        }

        void wait_for_finish(const operation_id id)
        {
            pump_until([this, id] {
                const operation_status* const status { workspace_->find_operation(id) };
                return status != nullptr && status->finished();
            });
        }

        [[nodiscard]] const operation_status& status(const operation_id id) const
        {
            const operation_status* const found { workspace_->find_operation(id) };
            REQUIRE(found != nullptr);
            return *found;
        }

    private:
        std::unique_ptr<process_runner> process_ {};
        std::unique_ptr<workspace_files> files_ {};
        test::event_queue events_ {};
        win32::askpass_bridge bridge_;
        git_tool_info tool_ {};
        std::unique_ptr<git_operation_runner> executor_ {};
        std::unique_ptr<task_scheduler> scheduler_ {};
        std::unique_ptr<git_workspace> workspace_ {};
    };

    void git_ok(const std::filesystem::path& directory, const std::vector<std::string>& arguments)
    {
        const test::git_run_result result { test::run_git(directory, arguments) };
        INFO("stderr: " << result.standard_error);
        REQUIRE(result.exit_code == 0);
    }

    // 원격이 `http://127.0.0.1`인 빈 저장소다. 사용자의 credential helper와 프록시가 끼어들지 않게 막는다.
    std::filesystem::path make_repository(const std::filesystem::path& root, const std::string& remote_url)
    {
        const std::filesystem::path repo { root / "repo" };
        std::filesystem::create_directories(repo);
        git_ok(repo, { "init", "-q", "-b", "main" });
        git_ok(repo, { "remote", "add", "origin", remote_url });
        git_ok(repo, { "config", "credential.helper", "" });
        git_ok(repo, { "config", "http.proxy", "" });
        return repo;
    }

    std::string failure_text(const operation_status& status)
    {
        if (status.failure.has_value() == false)
            return {};
        return std::string { status.failure->summary.begin(), status.failure->summary.end() } + " | " + std::string { status.failure->detail.begin(), status.failure->detail.end() };
    }
} // namespace

TEST_CASE("fetch asks for the username and password through the app and sends them to git", "[askpass][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");

    basic_auth_http_server server {};
    const test::temp_directory directory {};
    const std::filesystem::path repo { make_repository(directory.path(), server.url()) };
    live_auth_workspace live {};
    const std::string repository { live.open(repo) };

    const operation_id id { live.workspace().submit(repository, fetch_request {}) };
    REQUIRE(id != 0);

    live.wait_for_prompt(id, credential_prompt_kind::https_username);
    {
        const operation_status& status { live.status(id) };
        REQUIRE(status.needs_input.has_value());
        CHECK(status.needs_input->kind == input_kind::credentials);
        CHECK_FALSE(status.needs_input->secret);
        CHECK(status.needs_input->prompt.raw_text.find(u8"Username for") != std::u8string::npos);
        const std::string port_text { std::to_string(server.port()) };
        CHECK(status.needs_input->prompt.url.find(u8"127.0.0.1:" + std::u8string { port_text.begin(), port_text.end() }) != std::u8string::npos);
        CHECK_FALSE(status.finished());
        CHECK(status.cancellable);
        REQUIRE(live.workspace().answer_input(id, status.needs_input->prompt_id, u8"alice"));
        CHECK(live.status(id).state == operation_state::running);
    }

    live.wait_for_prompt(id, credential_prompt_kind::https_password);
    {
        const operation_status& status { live.status(id) };
        REQUIRE(status.needs_input.has_value());
        CHECK(status.needs_input->secret);
        CHECK(status.needs_input->prompt.user_name == u8"alice");
        REQUIRE(live.workspace().answer_input(id, status.needs_input->prompt_id, u8"pw"));
    }

    live.wait_for_finish(id);
    INFO(failure_text(live.status(id)));
    // git이 받은 답으로 Basic 인증을 보냈다. 서버 로그의 Authorization 헤더가 증거다.
    CHECK(server.received_authorization(alice_credentials));
    CHECK(live.status(id).state == operation_state::succeeded);
    // 답은 상태 어디에도 남지 않는다.
    for (const operation_command& command : live.status(id).commands)
    {
        CHECK(command.command_line.find(u8"alice:pw") == std::u8string::npos);
        CHECK(command.command_line.find(u8"YWxpY2U6cHc=") == std::u8string::npos);
    }
}

TEST_CASE("cancelling the username prompt ends the fetch as auth_cancelled without sending credentials", "[askpass][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");

    basic_auth_http_server server {};
    const test::temp_directory directory {};
    const std::filesystem::path repo { make_repository(directory.path(), server.url()) };
    live_auth_workspace live {};
    const std::string repository { live.open(repo) };

    const operation_id id { live.workspace().submit(repository, fetch_request {}) };
    REQUIRE(id != 0);
    live.wait_for_prompt(id, credential_prompt_kind::https_username);
    const std::uint64_t prompt_id { live.status(id).needs_input->prompt_id };

    REQUIRE(live.workspace().cancel_input(id, prompt_id));
    live.wait_for_finish(id);

    const operation_status& status { live.status(id) };
    INFO(failure_text(status));
    CHECK(status.state == operation_state::failed);
    REQUIRE(status.failure.has_value());
    CHECK(status.failure->kind == failure_kind::auth_cancelled);
    CHECK_FALSE(server.received_any_authorization());
}

TEST_CASE("cancelling the operation while it waits for a prompt releases git and the helper", "[askpass][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");

    basic_auth_http_server server {};
    const test::temp_directory directory {};
    const std::filesystem::path repo { make_repository(directory.path(), server.url()) };
    live_auth_workspace live {};
    const std::string repository { live.open(repo) };

    const operation_id id { live.workspace().submit(repository, fetch_request {}) };
    REQUIRE(id != 0);
    live.wait_for_prompt(id, credential_prompt_kind::https_username);

    REQUIRE(live.workspace().cancel(id));
    live.wait_for_finish(id);

    CHECK(live.status(id).state == operation_state::cancelled);
    CHECK_FALSE(live.status(id).needs_input.has_value());
    CHECK_FALSE(server.received_any_authorization());
}

TEST_CASE("shutting the askpass server down releases a worker that waits for a prompt", "[askpass][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");

    basic_auth_http_server server {};
    const test::temp_directory directory {};
    const std::filesystem::path repo { make_repository(directory.path(), server.url()) };
    live_auth_workspace live {};
    const std::string repository { live.open(repo) };

    const operation_id id { live.workspace().submit(repository, fetch_request {}) };
    REQUIRE(id != 0);
    live.wait_for_prompt(id, credential_prompt_kind::https_username);

    // 종료 순서의 첫 단계다. 대기 질문이 취소로 답하면 git이 인증 실패로 끝나 worker가 풀린다.
    live.bridge().shutdown();
    live.wait_for_finish(id);

    CHECK(live.status(id).state == operation_state::failed);
    CHECK_FALSE(server.received_any_authorization());
}

TEST_CASE("ls-remote also asks through the app", "[askpass][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not installed");

    basic_auth_http_server server {};
    const test::temp_directory directory {};
    const std::filesystem::path repo { make_repository(directory.path(), server.url()) };
    live_auth_workspace live {};
    const std::string repository { live.open(repo) };

    const operation_id id { live.workspace().submit(repository, refresh_remote_tags_request { { "origin" } }) };
    REQUIRE(id != 0);
    live.wait_for_prompt(id, credential_prompt_kind::https_username);
    REQUIRE(live.workspace().answer_input(id, live.status(id).needs_input->prompt_id, u8"alice"));
    live.wait_for_prompt(id, credential_prompt_kind::https_password);
    REQUIRE(live.workspace().answer_input(id, live.status(id).needs_input->prompt_id, u8"pw"));
    live.wait_for_finish(id);

    INFO(failure_text(live.status(id)));
    CHECK(server.received_authorization(alice_credentials));
}
