#include "infrastructure/hosting_service.h"

#include <algorithm>
#include <utility>

namespace cyella {
    hosting_token_store::~hosting_token_store()
    {
        forget_all();
    }

    void hosting_token_store::set(const std::string& host_key, std::string username, secret_text token)
    {
        const std::lock_guard<std::mutex> lock { mutex_ };
        entries_[host_key] = entry { std::move(username), std::make_shared<const secret_text>(std::move(token)) };
    }

    std::shared_ptr<const secret_text> hosting_token_store::token(const std::string& host_key) const
    {
        const std::lock_guard<std::mutex> lock { mutex_ };
        const auto found { entries_.find(host_key) };
        return found == entries_.end() ? nullptr : found->second.token;
    }

    std::string hosting_token_store::username(const std::string& host_key) const
    {
        const std::lock_guard<std::mutex> lock { mutex_ };
        const auto found { entries_.find(host_key) };
        return found == entries_.end() ? std::string {} : found->second.username;
    }

    bool hosting_token_store::contains(const std::string& host_key) const
    {
        const std::lock_guard<std::mutex> lock { mutex_ };
        return entries_.contains(host_key);
    }

    void hosting_token_store::forget(const std::string& host_key)
    {
        const std::lock_guard<std::mutex> lock { mutex_ };
        entries_.erase(host_key);
    }

    void hosting_token_store::forget_all()
    {
        const std::lock_guard<std::mutex> lock { mutex_ };
        entries_.clear();
    }

    std::vector<std::string> hosting_token_store::hosts() const
    {
        const std::lock_guard<std::mutex> lock { mutex_ };
        std::vector<std::string> result {};
        for (const auto& [host, value] : entries_)
            result.push_back(host);
        return result;
    }

    hosting_service::hosting_service(hosting_transport& transport, credential_source& credentials, std::function<std::int64_t()> clock)
        : transport_ { &transport }
        , credentials_ { &credentials }
        , clock_ { std::move(clock) }
    {}

    bool hosting_service::signed_in(const std::string& host_key) const
    {
        return tokens_.contains(host_key);
    }

    std::string hosting_service::signed_in_user(const std::string& host_key) const
    {
        return tokens_.username(host_key);
    }

    std::vector<std::string> hosting_service::signed_in_hosts() const
    {
        return tokens_.hosts();
    }

    void hosting_service::sign_out(const std::string& host_key)
    {
        tokens_.forget(host_key);
    }

    void hosting_service::sign_out_all()
    {
        tokens_.forget_all();
    }

    rate_limit_info hosting_service::last_rate_limit(const std::string& host_key) const
    {
        const std::lock_guard<std::mutex> lock { limit_mutex_ };
        const auto found { limits_.find(host_key) };
        return found == limits_.end() ? rate_limit_info {} : found->second;
    }

    void hosting_service::remember_rate_limit(const std::string& host_key, const rate_limit_info& reported)
    {
        rate_limit_info info { reported };
        // `Retry-After`만 온 429·503도 그 시간 동안은 보내지 않는다 (초기화 시각을 모르면 지금부터 센다).
        if (info.retry_after_seconds.has_value() && info.reset_at.has_value() == false)
        {
            info.reset_at = clock_() + *info.retry_after_seconds;
            if (info.remaining.has_value() == false)
                info.remaining = 0;
        }
        if (info.remaining.has_value() == false && info.reset_at.has_value() == false)
            return;
        const std::lock_guard<std::mutex> lock { limit_mutex_ };
        limits_[host_key] = info;
    }

    std::optional<hosting_error> hosting_service::exhausted_limit(const std::string& host_key) const
    {
        const rate_limit_info info { last_rate_limit(host_key) };
        if (info.remaining.has_value() && *info.remaining <= 0 && info.reset_at.has_value() && *info.reset_at > clock_())
        {
            hosting_error error {};
            error.kind = hosting_error_kind::rate_limited;
            error.status_code = 403;
            error.rate_limit = info;
            return error;
        }
        return std::nullopt;
    }

    credential_query hosting_service::query_of(const hosting_context& context) const
    {
        credential_query query {};
        query.protocol = context.repository.credential_protocol;
        query.host = context.repository.credential_host;
        query.working_directory = context.working_directory;
        return query;
    }

    credential_fill_result hosting_service::acquire(const hosting_context& context, const process_cancellation_token& cancellation)
    {
        const std::lock_guard<std::mutex> lock { credential_mutex_ };
        credential_fill_result filled { credentials_->fill(query_of(context), cancellation) };
        if (filled.ok)
            tokens_.set(context.repository.host_key, filled.username, secret_text { filled.token.value() });
        return filled;
    }

    hosting_service::outcome hosting_service::send(const hosting_context& context, const hosting_http_request& request, const process_cancellation_token& cancellation)
    {
        outcome attempt {};
        const std::string& host_key { context.repository.host_key };
        if (context.repository.supported() == false)
        {
            attempt.preflight.kind = hosting_error_kind::unsupported;
            return attempt;
        }
        std::shared_ptr<const secret_text> token { tokens_.token(host_key) };
        if (token == nullptr)
        {
            attempt.preflight.kind = hosting_error_kind::not_signed_in;
            return attempt;
        }
        if (std::optional<hosting_error> limited { exhausted_limit(host_key) }; limited.has_value())
        {
            attempt.preflight = std::move(*limited);
            return attempt;
        }
        attempt.response = transport_->execute(request, *token, cancellation);
        remember_rate_limit(host_key, read_rate_limit(attempt.response));
        if (attempt.response.status_code != 401)
            return attempt;

        // 401: 이 토큰은 더는 쓸 수 없다. 도우미의 저장본을 지우고 메모리에서도 잊은 뒤 한 번 다시 묻는다.
        credentials_->reject(query_of(context), tokens_.username(host_key), *token, cancellation);
        tokens_.forget(host_key);
        token.reset();
        credential_fill_result again { acquire(context, cancellation) };
        if (again.ok == false)
        {
            // 다시 받지 못했다. 응답은 그대로 401이라 `unauthorized`다.
            attempt.preflight.kind = again.cancelled ? hosting_error_kind::cancelled : hosting_error_kind::unauthorized;
            attempt.preflight.status_code = 401;
            attempt.preflight.message = again.cancelled ? std::string {} : again.error;
            return attempt;
        }
        token = tokens_.token(host_key);
        if (token == nullptr)
        {
            attempt.preflight.kind = hosting_error_kind::unauthorized;
            return attempt;
        }
        attempt.response = transport_->execute(request, *token, cancellation);
        remember_rate_limit(host_key, read_rate_limit(attempt.response));
        if (attempt.response.status_code == 401)
        {
            credentials_->reject(query_of(context), tokens_.username(host_key), *token, cancellation);
            tokens_.forget(host_key);
        }
        return attempt;
    }

    template<class T>
    hosting_result<T> hosting_service::preflight_failure(const outcome& attempt) const
    {
        hosting_result<T> result {};
        result.error = attempt.preflight;
        return result;
    }

    hosting_result<std::string> hosting_service::sign_in(const hosting_context& context, const process_cancellation_token& cancellation)
    {
        hosting_result<std::string> result {};
        if (context.repository.supported() == false)
        {
            result.error.kind = hosting_error_kind::unsupported;
            return result;
        }
        const std::string& host_key { context.repository.host_key };
        credential_fill_result filled { acquire(context, cancellation) };
        if (filled.ok == false)
        {
            result.error.kind = filled.cancelled ? hosting_error_kind::cancelled : hosting_error_kind::credential_unavailable;
            result.error.message = filled.error;
            return result;
        }
        // 받은 토큰이 쓸 수 있는지 `/user`로 확인한다. 401이면 `send`가 reject·재질문까지 한다.
        const outcome attempt { send(context, make_current_user_request(context.repository), cancellation) };
        if (attempt.preflight.empty() == false)
        {
            tokens_.forget(host_key);
            result.error = attempt.preflight;
            return result;
        }
        hosting_result<std::string> user { parse_current_user_response(context.repository.provider, attempt.response) };
        if (user.ok() == false)
        {
            // 확인하지 못한 토큰은 로그인으로 치지 않는다 (거절, 연결 실패, 취소 모두). 남겨 두면 다음 호출마다 같은 오류가 난다.
            tokens_.forget(host_key);
            return user;
        }
        // 계정 이름은 서버가 답한 것을 쓴다 (도우미의 `username`은 `oauth2` 같은 자리표일 수 있다).
        std::shared_ptr<const secret_text> token { tokens_.token(host_key) };
        if (token != nullptr)
            tokens_.set(host_key, *user.value, secret_text { token->value() });
        return user;
    }

    hosting_result<pull_request_page> hosting_service::list_pull_requests(const hosting_context& context, const pull_request_query& query, const process_cancellation_token& cancellation)
    {
        const outcome attempt { send(context, make_list_pull_requests_request(context.repository, query), cancellation) };
        if (attempt.preflight.empty() == false)
            return preflight_failure<pull_request_page>(attempt);
        return parse_pull_request_list_response(context.repository.provider, attempt.response, query.page);
    }

    hosting_result<created_pull_request> hosting_service::create_pull_request(const hosting_context& context, const create_pull_request_params& params, const process_cancellation_token& cancellation)
    {
        const outcome attempt { send(context, make_create_pull_request_request(context.repository, params), cancellation) };
        if (attempt.preflight.empty() == false)
            return preflight_failure<created_pull_request>(attempt);
        return parse_create_pull_request_response(context.repository.provider, attempt.response);
    }

    hosting_result<ci_status_info> hosting_service::ci_status(const hosting_context& context, const std::string& sha, const process_cancellation_token& cancellation)
    {
        std::vector<hosting_http_response> responses {};
        for (const hosting_http_request& request : make_ci_status_requests(context.repository, sha))
        {
            const outcome attempt { send(context, request, cancellation) };
            if (attempt.preflight.empty() == false)
                return preflight_failure<ci_status_info>(attempt);
            responses.push_back(attempt.response);
        }
        return parse_ci_status_responses(context.repository, sha, responses);
    }
} // namespace cyella
