// 검증용 가짜 GitHub·GitLab API 서버다 (127.0.0.1). 앱을 UI Automation으로 돌릴 때 실제 서비스 대신 이 서버에 붙인다.
//
//   cyella_fake_hosting_server --port-file=<경로> [--flavor=github|gitlab] [--project=octo/hello] [--token=uia-token] [--user=octocat]
//                              [--request-log=<경로>] [--stop-file=<경로>] [--rate-limit-after=<N>] [--git-repo=<bare 저장소>] [--git=<git.exe>]
//
// --git-repo를 주면 `/<프로젝트>.git/info/refs`·`git-upload-pack`을 그 bare 저장소로 서비스한다(`git upload-pack --stateless-rpc`). 앱이 PR의 `pull/<n>/head`를
// 받아 오는 fetch가 가짜 호스트에서 진짜로 도는 것을 확인하려는 것이다. 인증은 묻지 않고 push는 받지 않는다(밀어 올리는 쪽은 `remote.<이름>.pushurl`을 쓴다).
//
// 포트는 임시 포트이고 열리면 --port-file에 적는다. --stop-file이 생기면 끝난다 (그 밖에는 호출한 쪽이 pid로 끝낸다).
// 요청은 한 줄 JSON으로 --request-log에 쌓인다: 토큰 값은 적지 않고 `auth`가 ok·bad·none 중 하나다.
#include "fake_hosting_api.h"

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {
    using namespace cyella::test;

    std::string slurp(const std::filesystem::path& path)
    {
        std::ifstream in { path, std::ios::binary };
        return std::string { std::istreambuf_iterator<char> { in }, std::istreambuf_iterator<char> {} };
    }

    // `git upload-pack --stateless-rpc`로 smart HTTP 요청 하나에 답한다. 임시 파일로 입출력을 잇는다.
    class git_http_backend
    {
    public:
        git_http_backend(std::string git, std::string repository)
            : git_ { std::move(git) }
            , repository_ { std::move(repository) }
            , scratch_ { std::filesystem::temp_directory_path() / ("cyella-fake-hosting-" + std::to_string(::GetCurrentProcessId())) }
        {
            std::filesystem::create_directories(scratch_);
        }

        [[nodiscard]] bool enabled() const noexcept
        {
            return repository_.empty() == false;
        }

        // 처리할 요청이 아니면 값이 없다.
        [[nodiscard]] std::optional<fake_response> answer(const fake_request& request)
        {
            const std::string path { request.path() };
            const bool advertise { request.method == "GET" && path.ends_with("/info/refs") && request.query("service") == "git-upload-pack" };
            const bool rpc { request.method == "POST" && path.ends_with("/git-upload-pack") };
            if (enabled() == false || (advertise == false && rpc == false))
                return std::nullopt;
            const std::lock_guard<std::mutex> lock { mutex_ };
            // 요청의 Git-Protocol 헤더를 git에 환경 변수로 넘긴다 (v2 협상). 이 도구는 요청을 하나씩 처리한다.
            const std::string protocol { request.header("git-protocol") };
            ::SetEnvironmentVariableA("GIT_PROTOCOL", protocol.empty() ? nullptr : protocol.c_str());
            const std::filesystem::path input { scratch_ / "in.bin" };
            const std::filesystem::path output { scratch_ / "out.bin" };
            {
                std::ofstream in { input, std::ios::binary | std::ios::trunc };
                in << request.body;
            }
            std::string command { "\"\"" + git_ + "\" upload-pack --stateless-rpc " };
            if (advertise)
                command += "--advertise-refs ";
            command += "\"" + repository_ + "\" < \"" + input.string() + "\" > \"" + output.string() + "\" 2> NUL\"";
            const int exit_code { std::system(command.c_str()) };
            fake_response response {};
            if (exit_code != 0)
            {
                response.status = 500;
                response.body = "upload-pack failed";
                response.content_type = "text/plain";
                return response;
            }
            std::string body { slurp(output) };
            if (advertise && protocol.find("version=2") == std::string::npos)
                body = std::string { "001e# service=git-upload-pack\n" } + "0000" + body;
            response.body = std::move(body);
            response.content_type = advertise ? "application/x-git-upload-pack-advertisement" : "application/x-git-upload-pack-result";
            response.headers.push_back({ "Cache-Control", "no-cache" });
            return response;
        }

    private:
        std::string git_ {};
        std::string repository_ {};
        std::filesystem::path scratch_ {};
        std::mutex mutex_ {};
    };

    std::string argument_value(const int count, char** const arguments, const std::string_view name, std::string fallback = {})
    {
        const std::string prefix { std::string { "--" } + std::string { name } + "=" };
        for (int index { 1 }; index < count; ++index)
        {
            const std::string_view argument { arguments[index] };
            if (argument.rfind(prefix, 0) == 0)
                return std::string { argument.substr(prefix.size()) };
        }
        return fallback;
    }

    fake_pull_request make_pull(const std::uint64_t number, std::string title, std::string source, std::string state, const bool draft, const char sha_digit)
    {
        fake_pull_request pull {};
        pull.number = number;
        pull.title = std::move(title);
        pull.source_branch = std::move(source);
        pull.state = std::move(state);
        pull.draft = draft;
        pull.sha = std::string(40, sha_digit);
        pull.body = "Seeded by the verification server.\nSecond line of the description.";
        return pull;
    }
} // namespace

int main(const int count, char** const arguments)
{
    const std::string flavor_text { argument_value(count, arguments, "flavor", "github") };
    const std::string project { argument_value(count, arguments, "project", flavor_text == "gitlab" ? "group/sub/hello" : "octo/hello") };
    const std::string token { argument_value(count, arguments, "token", "uia-token") };
    const std::string port_file { argument_value(count, arguments, "port-file") };
    const std::string stop_file { argument_value(count, arguments, "stop-file") };
    const std::string log_file { argument_value(count, arguments, "request-log") };
    if (port_file.empty())
    {
        std::cerr << "--port-file is required\n";
        return 2;
    }

    fake_hosting_api api { flavor_text == "gitlab" ? fake_hosting_api::flavor::gitlab : fake_hosting_api::flavor::github, project };
    api.accept_tokens({ token });
    api.set_user(argument_value(count, arguments, "user", flavor_text == "gitlab" ? "root" : "octocat"));
    api.set_pull_requests({
        make_pull(14, "Add the pull request section to the sidebar", "feature/pr-list", "open", false, 'a'),
        make_pull(13, "Draft: show CI marks in the commit detail", "feature/ci-mark", "open", true, 'b'),
        make_pull(12, "Fix the credential retry after a 401", "fix/credential-retry", "open", false, 'c'),
        make_pull(11, "Parse ssh remote URLs with ports", "fix/ssh-url", "merged", false, 'd'),
        make_pull(10, "Try an alternative layout", "experiment/layout", "closed", false, 'e'),
    });
    api.set_default_ci({ "success" });
    api.set_ci(std::string(40, 'c'), { "failure" });
    api.set_ci(std::string(40, 'b'), { "pending" });
    if (const std::string limit { argument_value(count, arguments, "rate-limit-after") }; limit.empty() == false)
        api.rate_limit_after(std::stoi(limit));

    git_http_backend backend { argument_value(count, arguments, "git", "git"), argument_value(count, arguments, "git-repo") };
    std::mutex log_mutex {};
    const auto serve = [&](const fake_request& request) {
        std::optional<fake_response> git_response { backend.answer(request) };
        fake_response response { git_response.has_value() ? std::move(*git_response) : api.handle(request) };
        if (log_file.empty() == false)
        {
            const std::string authorization { request.header("authorization") };
            nlohmann::json line = nlohmann::json::object();
            line["method"] = request.method;
            line["target"] = request.target;
            line["auth"] = authorization.empty() ? "none" : (authorization == "Bearer " + token ? "ok" : "bad");
            line["status"] = response.status;
            line["body"] = request.body;
            const std::lock_guard<std::mutex> lock { log_mutex };
            std::ofstream log { log_file, std::ios::app | std::ios::binary };
            log << line.dump() << "\n";
        }
        return response;
    };
    fake_http_server server { serve };
    api.set_origin(server.origin());
    {
        std::ofstream out { port_file, std::ios::binary | std::ios::trunc };
        out << server.port();
    }
    while (true)
    {
        if (stop_file.empty() == false && std::filesystem::exists(stop_file))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds { 100 });
    }
    return 0;
}
