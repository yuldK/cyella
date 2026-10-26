#include "infrastructure/hosting_api.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <utility>

namespace cyella {
    namespace {
        using json = nlohmann::json;

        constexpr std::size_t message_limit { 300 };

        std::string lower(const std::string_view text)
        {
            std::string result { text };
            std::ranges::transform(result, result.begin(), [](const char value) { return static_cast<char>(std::tolower(static_cast<unsigned char>(value))); });
            return result;
        }

        bool contains_ignoring_case(const std::string_view text, const std::string_view needle)
        {
            return lower(text).find(lower(needle)) != std::string::npos;
        }

        std::optional<std::int64_t> parse_integer(const std::string_view text)
        {
            std::string_view trimmed { text };
            while (trimmed.empty() == false && std::isspace(static_cast<unsigned char>(trimmed.front())) != 0)
                trimmed.remove_prefix(1);
            while (trimmed.empty() == false && std::isspace(static_cast<unsigned char>(trimmed.back())) != 0)
                trimmed.remove_suffix(1);
            if (trimmed.empty())
                return std::nullopt;
            std::int64_t value { 0 };
            const auto [end, error] { std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value) };
            if (error != std::errc {} || end != trimmed.data() + trimmed.size())
                return std::nullopt;
            return value;
        }

        std::string string_of(const json& object, const char* const key)
        {
            if (object.is_object())
                if (const auto found { object.find(key) }; found != object.end() && found->is_string())
                    return found->get<std::string>();
            return {};
        }

        bool bool_of(const json& object, const char* const key)
        {
            if (object.is_object())
                if (const auto found { object.find(key) }; found != object.end() && found->is_boolean())
                    return found->get<bool>();
            return false;
        }

        std::uint64_t unsigned_of(const json& object, const char* const key)
        {
            if (object.is_object())
                if (const auto found { object.find(key) }; found != object.end() && found->is_number_unsigned())
                    return found->get<std::uint64_t>();
            return 0;
        }

        const json& child_of(const json& object, const char* const key)
        {
            static const json empty {};
            if (object.is_object())
                if (const auto found { object.find(key) }; found != object.end())
                    return *found;
            return empty;
        }

        std::string truncate(std::string text)
        {
            if (text.size() > message_limit)
            {
                text.resize(message_limit);
                // UTF-8 중간에서 잘리지 않게 이어 붙는 바이트를 걷어 낸다.
                while (text.empty() == false && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80)
                    text.pop_back();
                if (text.empty() == false && static_cast<unsigned char>(text.back()) >= 0xC0)
                    text.pop_back();
                text += "…";
            }
            return text;
        }

        // 서버 오류 몸에서 사람이 읽을 이유를 뽑는다. GitHub는 `message`와 `errors[].message`, GitLab은 문자열·배열·객체인 `message`나 `error`다.
        void flatten_message(const json& value, std::string& out)
        {
            if (value.is_string())
            {
                if (out.empty() == false)
                    out += "; ";
                out += value.get<std::string>();
            }
            else if (value.is_array())
            {
                for (const json& item : value)
                    flatten_message(item, out);
            }
            else if (value.is_object())
            {
                for (const auto& [key, item] : value.items())
                {
                    if (item.is_string() || item.is_array())
                    {
                        const std::size_t before { out.size() };
                        flatten_message(item, out);
                        if (out.size() > before)
                            out.insert(before == 0 ? 0 : before + 2, key + " ");
                    }
                }
            }
        }

        std::string error_message_of(const std::string_view body)
        {
            const json root { json::parse(body.begin(), body.end(), nullptr, false) };
            if (root.is_discarded() || root.is_object() == false)
                return {};
            std::string message {};
            for (const char* const key : { "message", "error", "error_description" })
                if (const auto found { root.find(key) }; found != root.end())
                {
                    flatten_message(*found, message);
                    break;
                }
            // GitHub 422: errors[].message에 구체적인 이유가 있다.
            if (const auto errors { root.find("errors") }; errors != root.end() && errors->is_array())
            {
                std::string detail {};
                for (const json& item : *errors)
                    if (item.is_string())
                        flatten_message(item, detail);
                    else if (item.is_object())
                        flatten_message(child_of(item, "message"), detail);
                if (detail.empty() == false)
                    message += message.empty() ? detail : ": " + detail;
            }
            return truncate(std::move(message));
        }

        ci_state map_github_check(const std::string& status, const std::string& conclusion)
        {
            if (status != "completed")
                return ci_state::pending;
            if (conclusion == "success" || conclusion == "neutral" || conclusion == "skipped")
                return ci_state::success;
            if (conclusion == "failure" || conclusion == "timed_out" || conclusion == "action_required" || conclusion == "startup_failure")
                return ci_state::failure;
            if (conclusion == "cancelled" || conclusion == "stale")
                return ci_state::error;
            return ci_state::none;
        }

        ci_state map_github_status(const std::string& state)
        {
            if (state == "success")
                return ci_state::success;
            if (state == "pending")
                return ci_state::pending;
            if (state == "failure")
                return ci_state::failure;
            if (state == "error")
                return ci_state::error;
            return ci_state::none;
        }

        ci_state map_gitlab_pipeline(const std::string& status)
        {
            if (status == "success")
                return ci_state::success;
            if (status == "failed")
                return ci_state::failure;
            if (status == "canceled" || status == "canceling")
                return ci_state::error;
            if (status == "created" || status == "waiting_for_resource" || status == "preparing" || status == "pending" || status == "running" || status == "scheduled")
                return ci_state::pending;
            // skipped·manual: 돌지 않았다.
            return ci_state::none;
        }

        hosting_error invalid_response_error(const hosting_http_response& response)
        {
            hosting_error error {};
            error.kind = hosting_error_kind::invalid_response;
            error.status_code = response.status_code;
            return error;
        }

        template<class T>
        hosting_result<T> failure(const hosting_provider provider, const hosting_http_response& response)
        {
            hosting_result<T> result {};
            result.error = classify_hosting_response(provider, response);
            result.rate_limit = read_rate_limit(response);
            return result;
        }

        bool success_status(const hosting_http_response& response) noexcept
        {
            return response.status_code >= 200 && response.status_code < 300;
        }

        pull_request_info map_github_pull_request(const json& item)
        {
            pull_request_info info {};
            info.number = unsigned_of(item, "number");
            info.title = string_of(item, "title");
            info.author = string_of(child_of(item, "user"), "login");
            const json& head { child_of(item, "head") };
            const json& base { child_of(item, "base") };
            info.source_branch = string_of(head, "ref");
            info.target_branch = string_of(base, "ref");
            info.head_sha = string_of(head, "sha");
            info.draft = bool_of(item, "draft");
            const std::string state { string_of(item, "state") };
            if (state == "open")
                info.state = pull_request_state::open;
            else
                info.state = child_of(item, "merged_at").is_string() ? pull_request_state::merged : pull_request_state::closed;
            // fork: 머리 저장소가 지워졌거나(null) 대상과 다르다.
            const json& head_repo { child_of(head, "repo") };
            const json& base_repo { child_of(base, "repo") };
            if (head.is_object() && head.contains("repo") && head_repo.is_null())
                info.from_fork = true;
            else if (head_repo.is_object() && base_repo.is_object())
                info.from_fork = string_of(head_repo, "full_name") != string_of(base_repo, "full_name");
            info.web_url = string_of(item, "html_url");
            info.body = string_of(item, "body");
            info.created_at = parse_iso8601_seconds(string_of(item, "created_at"));
            info.updated_at = parse_iso8601_seconds(string_of(item, "updated_at"));
            return info;
        }

        pull_request_info map_gitlab_merge_request(const json& item)
        {
            pull_request_info info {};
            info.number = unsigned_of(item, "iid");
            info.title = string_of(item, "title");
            info.author = string_of(child_of(item, "author"), "username");
            info.source_branch = string_of(item, "source_branch");
            info.target_branch = string_of(item, "target_branch");
            info.head_sha = string_of(item, "sha");
            info.draft = bool_of(item, "draft") || bool_of(item, "work_in_progress");
            const std::string state { string_of(item, "state") };
            if (state == "opened")
                info.state = pull_request_state::open;
            else if (state == "merged")
                info.state = pull_request_state::merged;
            else
                info.state = pull_request_state::closed;
            const std::uint64_t source_project { unsigned_of(item, "source_project_id") };
            const std::uint64_t target_project { unsigned_of(item, "target_project_id") };
            info.from_fork = source_project != 0 && target_project != 0 && source_project != target_project;
            info.web_url = string_of(item, "web_url");
            info.body = string_of(item, "description");
            info.created_at = parse_iso8601_seconds(string_of(item, "created_at"));
            info.updated_at = parse_iso8601_seconds(string_of(item, "updated_at"));
            if (const json& pipeline { child_of(item, "head_pipeline") }; pipeline.is_object())
                info.ci = map_gitlab_pipeline(string_of(pipeline, "status"));
            return info;
        }

        std::string github_project_path(const hosting_repository& repository)
        {
            return repository.api_base + "/repos/" + percent_encode_component(repository.owner) + "/" + percent_encode_component(repository.name);
        }

        std::string gitlab_project_path(const hosting_repository& repository)
        {
            return repository.api_base + "/projects/" + percent_encode_component(repository.full_name());
        }

        hosting_http_request base_request(const hosting_provider provider, const hosting_http_method method, std::string url)
        {
            hosting_http_request request {};
            request.method = method;
            request.url = std::move(url);
            if (provider == hosting_provider::github)
            {
                request.headers.push_back({ "Accept", "application/vnd.github+json" });
                request.headers.push_back({ "X-GitHub-Api-Version", "2022-11-28" });
            }
            else
                request.headers.push_back({ "Accept", "application/json" });
            return request;
        }

        bool starts_with_draft(const std::string& title)
        {
            const std::string head { lower(title.substr(0, 6)) };
            return head == "draft:" || lower(title.substr(0, 7)) == "[draft]" || lower(title.substr(0, 4)) == "wip:";
        }
    } // namespace

    std::string percent_encode_component(const std::string_view text)
    {
        static constexpr char digits[] { "0123456789ABCDEF" };
        std::string result {};
        for (const char value : text)
        {
            const auto byte { static_cast<unsigned char>(value) };
            if (std::isalnum(byte) != 0 || value == '-' || value == '.' || value == '_' || value == '~')
                result.push_back(value);
            else
            {
                result.push_back('%');
                result.push_back(digits[byte >> 4]);
                result.push_back(digits[byte & 0x0F]);
            }
        }
        return result;
    }

    hosting_http_request make_current_user_request(const hosting_repository& repository)
    {
        return base_request(repository.provider, hosting_http_method::get, repository.api_base + "/user");
    }

    hosting_http_request make_list_pull_requests_request(const hosting_repository& repository, const pull_request_query& query)
    {
        const std::uint32_t per_page { std::clamp<std::uint32_t>(query.per_page, 1, 100) };
        const std::string paging { "&per_page=" + std::to_string(per_page) + "&page=" + std::to_string(std::max<std::uint32_t>(query.page, 1)) };
        if (repository.provider == hosting_provider::gitlab)
            return base_request(repository.provider, hosting_http_method::get,
                gitlab_project_path(repository) + "/merge_requests?state=" + (query.include_closed ? "all" : "opened") + "&order_by=updated_at&sort=desc" + paging);
        return base_request(
            repository.provider, hosting_http_method::get, github_project_path(repository) + "/pulls?state=" + (query.include_closed ? "all" : "open") + "&sort=updated&direction=desc" + paging);
    }

    hosting_http_request make_create_pull_request_request(const hosting_repository& repository, const create_pull_request_params& params)
    {
        json body { json::object() };
        hosting_http_request request {};
        if (repository.provider == hosting_provider::gitlab)
        {
            request = base_request(repository.provider, hosting_http_method::post, gitlab_project_path(repository) + "/merge_requests");
            // GitLab의 생성 API에는 draft 필드가 없다. 제목의 `Draft:` 접두어가 초안이다.
            body["title"] = params.draft && starts_with_draft(params.title) == false ? "Draft: " + params.title : params.title;
            body["source_branch"] = params.source_branch;
            body["target_branch"] = params.target_branch;
            if (params.body.empty() == false)
                body["description"] = params.body;
        }
        else
        {
            request = base_request(repository.provider, hosting_http_method::post, github_project_path(repository) + "/pulls");
            body["title"] = params.title;
            body["head"] = params.source_branch;
            body["base"] = params.target_branch;
            body["body"] = params.body;
            body["draft"] = params.draft;
        }
        request.body = body.dump(-1, ' ', false, json::error_handler_t::replace);
        request.content_type = "application/json";
        return request;
    }

    hosting_http_request make_create_repository_request(const hosting_repository& repository, const create_repository_params& params, const std::optional<std::uint64_t> namespace_id)
    {
        json body { json::object() };
        hosting_http_request request {};
        if (repository.provider == hosting_provider::gitlab)
        {
            request = base_request(repository.provider, hosting_http_method::post, repository.api_base + "/projects");
            body["name"] = params.name;
            body["path"] = params.name;
            body["visibility"] = params.is_private ? "private" : "public";
            if (namespace_id.has_value())
                body["namespace_id"] = *namespace_id;
            if (params.description.empty() == false)
                body["description"] = params.description;
        }
        else
        {
            const std::string url { params.owner.empty() ? repository.api_base + "/user/repos" : repository.api_base + "/orgs/" + percent_encode_component(params.owner) + "/repos" };
            request = base_request(repository.provider, hosting_http_method::post, url);
            body["name"] = params.name;
            body["private"] = params.is_private;
            if (params.description.empty() == false)
                body["description"] = params.description;
        }
        request.body = body.dump(-1, ' ', false, json::error_handler_t::replace);
        request.content_type = "application/json";
        return request;
    }

    hosting_http_request make_namespace_request(const hosting_repository& repository, const std::string_view path)
    {
        return base_request(repository.provider, hosting_http_method::get, repository.api_base + "/namespaces/" + percent_encode_component(path));
    }

    std::vector<hosting_http_request> make_ci_status_requests(const hosting_repository& repository, const std::string_view sha)
    {
        std::vector<hosting_http_request> requests {};
        if (repository.provider == hosting_provider::gitlab)
        {
            requests.push_back(
                base_request(repository.provider, hosting_http_method::get, gitlab_project_path(repository) + "/pipelines?sha=" + percent_encode_component(sha) + "&order_by=id&sort=desc&per_page=5"));
            return requests;
        }
        const std::string commit { github_project_path(repository) + "/commits/" + percent_encode_component(sha) };
        requests.push_back(base_request(repository.provider, hosting_http_method::get, commit + "/status?per_page=100"));
        requests.push_back(base_request(repository.provider, hosting_http_method::get, commit + "/check-runs?per_page=100"));
        return requests;
    }

    rate_limit_info read_rate_limit(const hosting_http_response& response)
    {
        rate_limit_info info {};
        // GitHub는 소문자 `x-ratelimit-*`, GitLab은 `RateLimit-*`다. 둘 다 찾는다.
        for (const char* const name : { "x-ratelimit-limit", "ratelimit-limit" })
            if (const std::string_view value { response.header(name) }; value.empty() == false)
            {
                info.limit = parse_integer(value);
                break;
            }
        for (const char* const name : { "x-ratelimit-remaining", "ratelimit-remaining" })
            if (const std::string_view value { response.header(name) }; value.empty() == false)
            {
                info.remaining = parse_integer(value);
                break;
            }
        for (const char* const name : { "x-ratelimit-reset", "ratelimit-reset" })
            if (const std::string_view value { response.header(name) }; value.empty() == false)
            {
                info.reset_at = parse_integer(value);
                break;
            }
        info.retry_after_seconds = parse_integer(response.header("retry-after"));
        return info;
    }

    hosting_error classify_hosting_response(const hosting_provider provider, const hosting_http_response& response)
    {
        hosting_error error {};
        error.status_code = response.status_code;
        error.rate_limit = read_rate_limit(response);
        if (response.cancelled)
        {
            error.kind = hosting_error_kind::cancelled;
            return error;
        }
        if (response.received() == false)
        {
            error.kind = hosting_error_kind::network;
            error.message = truncate(response.transport_error);
            return error;
        }
        const int status { response.status_code };
        error.message = error_message_of(response.body);
        if (status == 401)
            error.kind = hosting_error_kind::unauthorized;
        else if (status == 429)
            error.kind = hosting_error_kind::rate_limited;
        else if (status == 403)
        {
            const bool exhausted { error.rate_limit.remaining.has_value() && *error.rate_limit.remaining == 0 };
            const bool secondary { error.rate_limit.retry_after_seconds.has_value() || contains_ignoring_case(error.message, "rate limit") };
            error.kind = provider == hosting_provider::github && (exhausted || secondary) ? hosting_error_kind::rate_limited : hosting_error_kind::forbidden;
        }
        else if (status == 404)
            error.kind = hosting_error_kind::not_found;
        else if (status == 400 || status == 409 || status == 422)
            error.kind = hosting_error_kind::rejected;
        else if (status >= 500)
            error.kind = hosting_error_kind::server_error;
        else
            error.kind = hosting_error_kind::rejected;
        return error;
    }

    hosting_result<std::string> parse_current_user_response(const hosting_provider provider, const hosting_http_response& response)
    {
        if (success_status(response) == false)
            return failure<std::string>(provider, response);
        hosting_result<std::string> result {};
        result.rate_limit = read_rate_limit(response);
        const json root { json::parse(response.body.begin(), response.body.end(), nullptr, false) };
        const std::string name { string_of(root, provider == hosting_provider::gitlab ? "username" : "login") };
        if (name.empty())
        {
            result.error = invalid_response_error(response);
            return result;
        }
        result.value = name;
        return result;
    }

    hosting_result<pull_request_page> parse_pull_request_list_response(const hosting_provider provider, const hosting_http_response& response, const std::uint32_t page)
    {
        if (success_status(response) == false)
            return failure<pull_request_page>(provider, response);
        hosting_result<pull_request_page> result {};
        result.rate_limit = read_rate_limit(response);
        const json root { json::parse(response.body.begin(), response.body.end(), nullptr, false) };
        if (root.is_array() == false)
        {
            result.error = invalid_response_error(response);
            return result;
        }
        pull_request_page parsed {};
        parsed.page = page;
        for (const json& item : root)
        {
            pull_request_info info { provider == hosting_provider::gitlab ? map_gitlab_merge_request(item) : map_github_pull_request(item) };
            if (item.is_object() == false || info.number == 0)
            {
                ++parsed.skipped;
                continue;
            }
            parsed.items.push_back(std::move(info));
        }
        if (provider == hosting_provider::gitlab)
        {
            if (const std::optional<std::int64_t> next { parse_integer(response.header("x-next-page")) }; next.has_value() && *next > 0)
                parsed.next_page = static_cast<std::uint32_t>(*next);
        }
        else
        {
            // `Link: <https://api.github.com/...&page=2>; rel="next", <...>; rel="last"`
            const std::string link { lower(response.header("link")) };
            if (link.find("rel=\"next\"") != std::string::npos)
                parsed.next_page = page + 1;
        }
        result.value = std::move(parsed);
        return result;
    }

    hosting_result<created_pull_request> parse_create_pull_request_response(const hosting_provider provider, const hosting_http_response& response)
    {
        if (success_status(response) == false)
            return failure<created_pull_request>(provider, response);
        hosting_result<created_pull_request> result {};
        result.rate_limit = read_rate_limit(response);
        const json root { json::parse(response.body.begin(), response.body.end(), nullptr, false) };
        created_pull_request created {};
        if (provider == hosting_provider::gitlab)
        {
            created.number = unsigned_of(root, "iid");
            created.web_url = string_of(root, "web_url");
            created.head_sha = string_of(root, "sha");
        }
        else
        {
            created.number = unsigned_of(root, "number");
            created.web_url = string_of(root, "html_url");
            created.head_sha = string_of(child_of(root, "head"), "sha");
        }
        created.title = string_of(root, "title");
        if (created.number == 0)
        {
            result.error = invalid_response_error(response);
            return result;
        }
        result.value = std::move(created);
        return result;
    }

    hosting_result<created_repository> parse_create_repository_response(const hosting_provider provider, const hosting_http_response& response)
    {
        if (success_status(response) == false)
            return failure<created_repository>(provider, response);
        hosting_result<created_repository> result {};
        result.rate_limit = read_rate_limit(response);
        const json root { json::parse(response.body.begin(), response.body.end(), nullptr, false) };
        created_repository created {};
        if (provider == hosting_provider::gitlab)
        {
            created.full_name = string_of(root, "path_with_namespace");
            created.web_url = string_of(root, "web_url");
            created.https_url = string_of(root, "http_url_to_repo");
            created.ssh_url = string_of(root, "ssh_url_to_repo");
        }
        else
        {
            created.full_name = string_of(root, "full_name");
            created.web_url = string_of(root, "html_url");
            created.https_url = string_of(root, "clone_url");
            created.ssh_url = string_of(root, "ssh_url");
        }
        // 원격으로 더할 주소가 없으면 게시를 이을 수 없다.
        if (created.https_url.empty() && created.ssh_url.empty())
        {
            result.error = invalid_response_error(response);
            return result;
        }
        result.value = std::move(created);
        return result;
    }

    hosting_result<std::uint64_t> parse_namespace_response(const hosting_provider provider, const hosting_http_response& response)
    {
        if (success_status(response) == false)
            return failure<std::uint64_t>(provider, response);
        hosting_result<std::uint64_t> result {};
        result.rate_limit = read_rate_limit(response);
        const json root { json::parse(response.body.begin(), response.body.end(), nullptr, false) };
        const std::uint64_t id { unsigned_of(root, "id") };
        if (id == 0)
        {
            result.error = invalid_response_error(response);
            return result;
        }
        result.value = id;
        return result;
    }

    hosting_result<ci_status_info> parse_ci_status_responses(const hosting_repository& repository, const std::string_view sha, const std::vector<hosting_http_response>& responses)
    {
        hosting_result<ci_status_info> result {};
        ci_status_info info {};
        info.sha = std::string { sha };
        const hosting_provider provider { repository.provider };
        if (responses.empty())
        {
            result.error = invalid_response_error({});
            return result;
        }
        for (const hosting_http_response& response : responses)
        {
            const rate_limit_info limit { read_rate_limit(response) };
            if (limit.remaining.has_value() || limit.reset_at.has_value())
                result.rate_limit = limit;
        }

        if (provider == hosting_provider::gitlab)
        {
            const hosting_http_response& response { responses.front() };
            if (success_status(response) == false)
            {
                // 서버가 모르는 커밋은 파이프라인이 없는 것과 같다.
                if (response.status_code == 404)
                {
                    result.value = std::move(info);
                    return result;
                }
                return failure<ci_status_info>(provider, response);
            }
            const json root { json::parse(response.body.begin(), response.body.end(), nullptr, false) };
            if (root.is_array() == false)
            {
                result.error = invalid_response_error(response);
                return result;
            }
            // 목록은 최신이 앞이다. 가장 새 파이프라인이 이 커밋의 상태다.
            for (const json& pipeline : root)
            {
                const ci_state state { map_gitlab_pipeline(string_of(pipeline, "status")) };
                info.checks.push_back({ "pipeline #" + std::to_string(unsigned_of(pipeline, "id")), state, string_of(pipeline, "web_url") });
                if (info.checks.size() == 1)
                {
                    info.state = state;
                    info.web_url = string_of(pipeline, "web_url");
                }
            }
            result.value = std::move(info);
            return result;
        }

        std::vector<ci_state> states {};
        std::optional<hosting_error> first_error {};
        const auto read_failure = [&](const hosting_http_response& response) {
            // 서버가 모르는 커밋(404, GitHub는 422 "No commit found")은 오류가 아니라 상태 없음이다.
            if (response.status_code == 404 || response.status_code == 422)
                return;
            if (first_error.has_value() == false)
                first_error = classify_hosting_response(provider, response);
        };

        const hosting_http_response& status_response { responses[0] };
        if (success_status(status_response))
        {
            const json root { json::parse(status_response.body.begin(), status_response.body.end(), nullptr, false) };
            if (root.is_object())
            {
                // 상태가 하나도 없을 때 combined state는 "pending"이다. 개수가 0이면 쓰지 않는다.
                if (const json& statuses { child_of(root, "statuses") }; statuses.is_array())
                    for (const json& item : statuses)
                    {
                        const ci_state state { map_github_status(string_of(item, "state")) };
                        info.checks.push_back({ string_of(item, "context"), state, string_of(item, "target_url") });
                        states.push_back(state);
                    }
            }
        }
        else
            read_failure(status_response);

        if (responses.size() > 1)
        {
            const hosting_http_response& checks_response { responses[1] };
            if (success_status(checks_response))
            {
                const json root { json::parse(checks_response.body.begin(), checks_response.body.end(), nullptr, false) };
                if (const json& runs { child_of(root, "check_runs") }; runs.is_array())
                    for (const json& item : runs)
                    {
                        const ci_state state { map_github_check(string_of(item, "status"), string_of(item, "conclusion")) };
                        info.checks.push_back({ string_of(item, "name"), state, string_of(item, "html_url") });
                        states.push_back(state);
                    }
            }
            else
                read_failure(checks_response);
        }

        if (states.empty() && first_error.has_value())
        {
            result.error = std::move(*first_error);
            return result;
        }
        info.state = combine_ci_states(states);
        info.web_url = repository.web_url() + "/commit/" + info.sha;
        result.value = std::move(info);
        return result;
    }
} // namespace cyella
