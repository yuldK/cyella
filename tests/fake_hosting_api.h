#pragma once

// GitHub REST v3·GitLab REST v4의 일부(PR·MR 목록과 생성, CI 상태, 현재 계정)를 흉내 내는 시험용 API다. `fake_http_server`의 핸들러로 꽂는다.
// 실제 서비스에는 닿지 않는다. 응답 모양은 두 서비스의 공개 문서를 따른다.
#include "fake_hosting_server.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cyella::test {
    struct fake_pull_request
    {
        std::uint64_t number { 1 };
        std::string title {};
        std::string author { "octocat" };
        std::string source_branch {};
        std::string target_branch { "main" };
        // "open", "closed", "merged"
        std::string state { "open" };
        bool draft { false };
        std::string sha {};
        std::string body {};
    };

    struct fake_ci_fixture
    {
        // GitHub: check-run의 status/conclusion을 이 값에서 만든다. "success", "failure", "pending".
        std::string state { "success" };
    };

    struct fake_recorded_request
    {
        std::string method {};
        std::string target {};
        std::string authorization {};
        std::string body {};
    };

    class fake_hosting_api
    {
    public:
        enum class flavor
        {
            github,
            gitlab,
        };

        // `project`는 `owner/name`(GitLab은 `group/sub/name`)이다.
        fake_hosting_api(const flavor kind, std::string project)
            : kind_ { kind }
            , project_ { std::move(project) }
        {}

        // ---- 설정 (서버가 도는 동안 불러도 된다)
        // 서버가 포트를 얻은 뒤 출처(`http://127.0.0.1:port`)를 알려 준다. 응답의 `html_url`·`web_url`이 쓴다.
        void set_origin(std::string origin)
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            origin_ = std::move(origin);
        }
        void accept_tokens(std::set<std::string> tokens)
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            tokens_ = std::move(tokens);
        }
        void set_user(std::string user)
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            user_ = std::move(user);
        }
        void set_pull_requests(std::vector<fake_pull_request> pulls)
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            pulls_ = std::move(pulls);
        }
        void set_ci(const std::string& sha, fake_ci_fixture fixture)
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            ci_[sha] = std::move(fixture);
        }
        // 이 횟수만큼 요청을 받은 뒤부터 호출 한도 오류(GitHub 403, GitLab 429)를 돌려준다. 0이면 끈다.
        void rate_limit_after(const int requests)
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            rate_limit_after_ = requests;
        }
        // 이름을 따로 정하지 않은 커밋에 돌려줄 CI 상태다. 검증 앱이 어떤 커밋을 골라도 CI 줄이 서게 한다.
        void set_default_ci(fake_ci_fixture fixture)
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            default_ci_ = std::move(fixture);
        }
        // 설정하면 PR 생성이 이 메시지로 거절된다 (422·409).
        void reject_create_with(std::string message)
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            create_error_ = std::move(message);
        }

        // 저장소 만들기가 돌려줄 https 주소의 뿌리다. 비우면 서버 출처(`http://127.0.0.1:port/<이름>.git`)다. 검증 도구는 로컬 폴더를 주고
        // `on_repository_created`에서 그 자리에 bare 저장소를 만들어 실제 push를 받는다.
        void set_clone_root(std::string root)
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            clone_root_ = std::move(root);
        }
        void on_repository_created(std::function<void(const std::string& full_name)> callback)
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            repository_created_ = std::move(callback);
        }
        // GitLab 그룹 경로와 id다. 여기 없는 그룹은 404다.
        void add_namespace(std::string path, const std::uint64_t id)
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            namespaces_[std::move(path)] = id;
        }
        [[nodiscard]] std::vector<std::string> created_repositories() const
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            return created_repositories_;
        }

        // ---- 기록
        [[nodiscard]] std::vector<fake_recorded_request> requests() const
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            return requests_;
        }
        [[nodiscard]] std::size_t count_requests(const std::string& method, const std::string& path_prefix) const
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            return static_cast<std::size_t>(
                std::ranges::count_if(requests_, [&](const fake_recorded_request& request) { return request.method == method && request.target.rfind(path_prefix, 0) == 0; }));
        }
        [[nodiscard]] std::vector<fake_pull_request> pull_requests() const
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            return pulls_;
        }

        [[nodiscard]] fake_response handle(const fake_request& request)
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            requests_.push_back({ request.method, request.target, request.header("authorization"), request.body });
            ++seen_;
            const bool github { kind_ == flavor::github };
            if (rate_limit_after_ > 0 && seen_ > static_cast<std::size_t>(rate_limit_after_))
            {
                fake_response limited {};
                limited.status = github ? 403 : 429;
                limited.body
                    = github ? R"({"message": "API rate limit exceeded for user ID 1.", "documentation_url": "https://docs.github.com/rest/overview/rate-limits-for-the-rest-api"})" : "Retry later";
                limited.content_type = github ? "application/json" : "text/plain";
                limited.headers = github
                    ? std::vector<std::pair<std::string, std::string>> { { "x-ratelimit-limit", "60" }, { "x-ratelimit-remaining", "0" }, { "x-ratelimit-reset", "4102444800" } }
                    : std::vector<std::pair<std::string, std::string>> { { "RateLimit-Limit", "60" }, { "RateLimit-Remaining", "0" }, { "RateLimit-Reset", "4102444800" }, { "Retry-After", "30" } };
                return limited;
            }
            const std::string authorization { request.header("authorization") };
            if (authorization.rfind("Bearer ", 0) != 0 || tokens_.contains(authorization.substr(7)) == false)
            {
                fake_response denied {};
                denied.status = 401;
                denied.body = github ? R"({"message": "Bad credentials"})" : R"({"message": "401 Unauthorized"})";
                return denied;
            }
            fake_response response { route(request) };
            response.headers.push_back(github ? std::pair<std::string, std::string> { "x-ratelimit-remaining", "4999" } : std::pair<std::string, std::string> { "RateLimit-Remaining", "599" });
            return response;
        }

    private:
        [[nodiscard]] static fake_response json_response(const nlohmann::json& body, const int status = 200)
        {
            fake_response response {};
            response.status = status;
            response.body = body.dump();
            return response;
        }

        [[nodiscard]] static fake_response not_found()
        {
            return json_response({ { "message", "Not Found" } }, 404);
        }

        [[nodiscard]] static std::string encode_project(const std::string& project)
        {
            std::string result {};
            for (const char value : project)
                result += value == '/' ? std::string { "%2F" } : std::string { value };
            return result;
        }

        [[nodiscard]] nlohmann::json render(const fake_pull_request& pull) const
        {
            nlohmann::json item = nlohmann::json::object();
            if (kind_ == flavor::github)
            {
                item["number"] = pull.number;
                item["title"] = pull.title;
                item["state"] = pull.state == "open" ? "open" : "closed";
                item["draft"] = pull.draft;
                item["user"]["login"] = pull.author;
                item["body"] = pull.body;
                item["created_at"] = "2026-10-01T09:00:00Z";
                item["updated_at"] = "2026-10-02T09:00:00Z";
                item["merged_at"] = pull.state == "merged" ? nlohmann::json("2026-10-02T09:00:00Z") : nlohmann::json(nullptr);
                item["html_url"] = origin_ + "/" + project_ + "/pull/" + std::to_string(pull.number);
                item["head"]["ref"] = pull.source_branch;
                item["head"]["sha"] = pull.sha;
                item["head"]["repo"]["full_name"] = project_;
                item["base"]["ref"] = pull.target_branch;
                item["base"]["sha"] = "0000000000000000000000000000000000000000";
                item["base"]["repo"]["full_name"] = project_;
                return item;
            }
            item["iid"] = pull.number;
            item["id"] = 1000 + pull.number;
            item["title"] = pull.title;
            item["description"] = pull.body;
            item["state"] = pull.state == "open" ? std::string { "opened" } : pull.state;
            item["draft"] = pull.draft;
            item["author"]["username"] = pull.author;
            item["source_branch"] = pull.source_branch;
            item["target_branch"] = pull.target_branch;
            item["sha"] = pull.sha;
            item["created_at"] = "2026-10-01T09:00:00.000Z";
            item["updated_at"] = "2026-10-02T09:00:00.000Z";
            item["source_project_id"] = 3;
            item["target_project_id"] = 3;
            item["web_url"] = origin_ + "/" + project_ + "/-/merge_requests/" + std::to_string(pull.number);
            return item;
        }

        [[nodiscard]] fake_response route(const fake_request& request)
        {
            const std::string path { request.path() };
            if (path == "/user")
                return json_response(kind_ == flavor::github ? nlohmann::json { { "login", user_ }, { "id", 1 } } : nlohmann::json { { "username", user_ }, { "id", 1 } });
            if (kind_ == flavor::github && request.method == "POST" && path == "/user/repos")
                return create_repository(request, user_);
            if (kind_ == flavor::github && request.method == "POST" && path.rfind("/orgs/", 0) == 0 && path.ends_with("/repos"))
                return create_repository(request, path.substr(6, path.size() - 6 - 6));
            if (kind_ == flavor::gitlab && request.method == "POST" && path == "/projects")
                return create_repository(request, {});
            if (kind_ == flavor::gitlab && request.method == "GET" && path.rfind("/namespaces/", 0) == 0)
            {
                const std::string wanted { decode_path(path.substr(12)) };
                const auto found { namespaces_.find(wanted) };
                if (found == namespaces_.end())
                    return json_response({ { "message", "404 Namespace Not Found" } }, 404);
                return json_response({ { "id", found->second }, { "full_path", found->first }, { "kind", "group" } });
            }
            const std::string base { kind_ == flavor::github ? "/repos/" + project_ : "/projects/" + encode_project(project_) };
            if (path == base + (kind_ == flavor::github ? "/pulls" : "/merge_requests"))
                return request.method == "POST" ? create(request) : list(request);
            if (kind_ == flavor::github)
            {
                if (path.rfind(base + "/commits/", 0) == 0)
                {
                    const std::string rest { path.substr((base + "/commits/").size()) };
                    const std::size_t slash { rest.find('/') };
                    const std::string sha { rest.substr(0, slash) };
                    const std::string what { slash == std::string::npos ? std::string {} : rest.substr(slash + 1) };
                    const auto found { find_ci(sha) };
                    if (what == "status")
                    {
                        if (found == ci_.end())
                            return json_response({ { "message", "No commit found for SHA: " + sha } }, 422);
                        // 옛 상태 API는 비워 둔다. 상태는 check-run이 말한다.
                        return json_response({ { "state", "pending" }, { "sha", sha }, { "total_count", 0 }, { "statuses", nlohmann::json::array() } });
                    }
                    if (what == "check-runs")
                    {
                        if (found == ci_.end())
                            return json_response({ { "message", "No commit found for SHA: " + sha } }, 422);
                        const std::string state { found->second.state };
                        nlohmann::json run = nlohmann::json::object();
                        run["name"] = "build";
                        run["status"] = state == "pending" ? "in_progress" : "completed";
                        run["conclusion"] = state == "pending" ? nlohmann::json(nullptr) : nlohmann::json(state);
                        run["html_url"] = origin_ + "/" + project_ + "/runs/1";
                        return json_response({ { "total_count", 1 }, { "check_runs", nlohmann::json::array({ run }) } });
                    }
                }
            }
            else if (path == base + "/pipelines")
            {
                nlohmann::json list = nlohmann::json::array();
                const std::string sha { request.query("sha") };
                if (const auto found { find_ci(sha) }; found != ci_.end())
                {
                    const std::string state { found->second.state };
                    nlohmann::json pipeline = nlohmann::json::object();
                    pipeline["id"] = 77;
                    pipeline["status"] = state == "pending" ? "running" : state == "failure" ? "failed" : "success";
                    pipeline["sha"] = sha;
                    pipeline["ref"] = "feature";
                    pipeline["web_url"] = origin_ + "/" + project_ + "/-/pipelines/77";
                    list.push_back(std::move(pipeline));
                }
                return json_response(list);
            }
            return not_found();
        }

        [[nodiscard]] static std::string decode_path(const std::string& text)
        {
            std::string result {};
            for (std::size_t index { 0 }; index < text.size(); ++index)
                if (text[index] == '%' && index + 2 < text.size())
                {
                    result.push_back(static_cast<char>(std::stoi(text.substr(index + 1, 2), nullptr, 16)));
                    index += 2;
                }
                else
                    result.push_back(text[index]);
            return result;
        }

        // GitHub는 `owner`가 URL에서 오고, GitLab은 본문의 `namespace_id`(없으면 내 계정)에서 온다. 첫 커밋을 만들지 않는 빈 저장소다.
        [[nodiscard]] fake_response create_repository(const fake_request& request, std::string owner)
        {
            const nlohmann::json body { nlohmann::json::parse(request.body, nullptr, false) };
            if (body.is_discarded() || body.is_object() == false || body.contains("name") == false)
                return json_response({ { "message", "Problems parsing JSON" } }, 400);
            const std::string name { body.value("name", "") };
            const bool github { kind_ == flavor::github };
            if (github == false)
            {
                owner = user_;
                if (const auto id { body.find("namespace_id") }; id != body.end() && id->is_number_unsigned())
                    for (const auto& [group, group_id] : namespaces_)
                        if (group_id == id->get<std::uint64_t>())
                            owner = group;
            }
            const std::string full { owner + "/" + name };
            if (std::ranges::find(created_repositories_, full) != created_repositories_.end())
            {
                if (github)
                {
                    nlohmann::json error = nlohmann::json::object();
                    error["resource"] = "Repository";
                    error["code"] = "custom";
                    error["field"] = "name";
                    error["message"] = "name already exists on this account";
                    nlohmann::json rejected = nlohmann::json::object();
                    rejected["message"] = "Repository creation failed.";
                    rejected["errors"] = nlohmann::json::array({ error });
                    return json_response(rejected, 422);
                }
                nlohmann::json reasons = nlohmann::json::object();
                reasons["name"] = nlohmann::json::array({ "has already been taken" });
                reasons["path"] = nlohmann::json::array({ "has already been taken" });
                return json_response({ { "message", reasons } }, 400);
            }
            created_repositories_.push_back(full);
            if (repository_created_ != nullptr)
                repository_created_(full);
            const std::string https { (clone_root_.empty() ? origin_ : clone_root_) + "/" + full + ".git" };
            const std::string ssh { "ssh://git@127.0.0.1/" + full + ".git" };
            const std::string web { origin_ + "/" + full };
            const bool is_private { github ? body.value("private", false) : body.value("visibility", "private") == "private" };
            nlohmann::json created = nlohmann::json::object();
            created["name"] = name;
            if (github)
            {
                created["full_name"] = full;
                created["private"] = is_private;
                created["html_url"] = web;
                created["clone_url"] = https;
                created["ssh_url"] = ssh;
            }
            else
            {
                created["path_with_namespace"] = full;
                created["visibility"] = is_private ? "private" : "public";
                created["web_url"] = web;
                created["http_url_to_repo"] = https;
                created["ssh_url_to_repo"] = ssh;
            }
            return json_response(created, 201);
        }

        // 이름을 정한 커밋이 아니면 기본 CI를 그 커밋의 것으로 돌려준다.
        [[nodiscard]] std::map<std::string, fake_ci_fixture>::iterator find_ci(const std::string& sha)
        {
            auto found { ci_.find(sha) };
            if (found == ci_.end() && default_ci_.has_value())
                found = ci_.emplace(sha, *default_ci_).first;
            return found;
        }

        [[nodiscard]] fake_response list(const fake_request& request) const
        {
            const std::string state { request.query("state") };
            const bool all { state == "all" };
            std::vector<const fake_pull_request*> selected {};
            for (const fake_pull_request& pull : pulls_)
                if (all || pull.state == "open")
                    selected.push_back(&pull);
            const std::size_t per_page { request.query("per_page").empty() ? 30 : static_cast<std::size_t>(std::stoul(request.query("per_page"))) };
            const std::size_t page { request.query("page").empty() ? 1 : static_cast<std::size_t>(std::stoul(request.query("page"))) };
            nlohmann::json body = nlohmann::json::array();
            const std::size_t first { (page - 1) * per_page };
            for (std::size_t index { first }; index < std::min(selected.size(), first + per_page); ++index)
                body.push_back(render(*selected[index]));
            fake_response response { json_response(body) };
            const bool more { selected.size() > first + per_page };
            if (kind_ == flavor::github)
            {
                if (more)
                    response.headers.push_back({ "Link", "<" + origin_ + request.path() + "?page=" + std::to_string(page + 1) + ">; rel=\"next\"" });
            }
            else
                response.headers.push_back({ "X-Next-Page", more ? std::to_string(page + 1) : std::string {} });
            return response;
        }

        [[nodiscard]] fake_response create(const fake_request& request)
        {
            const nlohmann::json body { nlohmann::json::parse(request.body, nullptr, false) };
            if (create_error_.empty() == false)
            {
                if (kind_ == flavor::github)
                    return json_response(
                        { { "message", "Validation Failed" }, { "errors", nlohmann::json::array({ { { "resource", "PullRequest" }, { "code", "custom" }, { "message", create_error_ } } }) } }, 422);
                return json_response({ { "message", nlohmann::json::array({ create_error_ }) } }, 409);
            }
            if (body.is_discarded() || body.is_object() == false)
                return json_response({ { "message", "Problems parsing JSON" } }, 400);
            fake_pull_request pull {};
            pull.number = 1;
            for (const fake_pull_request& existing : pulls_)
                pull.number = std::max(pull.number, existing.number + 1);
            if (kind_ == flavor::github)
            {
                pull.title = body.value("title", "");
                pull.source_branch = body.value("head", "");
                pull.target_branch = body.value("base", "");
                pull.body = body.value("body", "");
                pull.draft = body.value("draft", false);
            }
            else
            {
                pull.title = body.value("title", "");
                pull.source_branch = body.value("source_branch", "");
                pull.target_branch = body.value("target_branch", "");
                pull.body = body.value("description", "");
                pull.draft = pull.title.rfind("Draft:", 0) == 0;
            }
            pull.author = user_;
            pull.sha = "c0ffee0000000000000000000000000000000000";
            pulls_.push_back(pull);
            return json_response(render(pull), 201);
        }

        flavor kind_ { flavor::github };
        std::string project_ {};
        std::string origin_ {};
        mutable std::mutex mutex_ {};
        std::set<std::string> tokens_ { "test-token" };
        std::string user_ { "octocat" };
        std::vector<fake_pull_request> pulls_ {};
        std::map<std::string, fake_ci_fixture> ci_ {};
        std::optional<fake_ci_fixture> default_ci_ {};
        std::vector<fake_recorded_request> requests_ {};
        std::size_t seen_ { 0 };
        int rate_limit_after_ { 0 };
        std::string create_error_ {};
        std::string clone_root_ {};
        std::function<void(const std::string&)> repository_created_ {};
        std::map<std::string, std::uint64_t> namespaces_ {};
        std::vector<std::string> created_repositories_ {};
    };
} // namespace cyella::test
