#pragma once

#include "application/hosting_credentials.h"
#include "application/hosting_transport.h"
#include "domain/hosting_models.h"
#include "domain/hosting_remote.h"
#include "infrastructure/hosting_api.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace cyella {
    // 호스트마다 로그인 상태를 메모리에만 두는 토큰 보관소다 (ADR-007). 파일·설정·로그에 쓰지 않는다. 지우면(`forget`) 마지막 사용자가
    // 놓는 순간 내용이 지워지고, 소멸할 때 모두 지운다.
    class hosting_token_store
    {
    public:
        hosting_token_store() = default;
        hosting_token_store(const hosting_token_store&) = delete;
        hosting_token_store& operator=(const hosting_token_store&) = delete;
        ~hosting_token_store();

        void set(const std::string& host_key, std::string username, secret_text token);
        // 쓰는 동안 지워지지 않도록 공유 손잡이를 준다. 없으면 null이다.
        [[nodiscard]] std::shared_ptr<const secret_text> token(const std::string& host_key) const;
        [[nodiscard]] std::string username(const std::string& host_key) const;
        [[nodiscard]] bool contains(const std::string& host_key) const;
        void forget(const std::string& host_key);
        void forget_all();
        [[nodiscard]] std::vector<std::string> hosts() const;

    private:
        struct entry
        {
            std::string username {};
            std::shared_ptr<const secret_text> token {};
        };

        mutable std::mutex mutex_ {};
        std::map<std::string, entry> entries_ {};
    };

    // GitHub·GitLab API 호출의 조립이다. 워커에서 부르며 호출 스레드가 응답까지 막힌다 (네트워크 전송은 luil의 비동기 HTTP client가 하고,
    // 전송 구현이 그 완료를 기다려 돌려준다). 상태는 토큰 보관소와 호스트별 마지막 호출 한도뿐이고 모두 mutex로 지킨다.
    //
    // 규칙
    //  - 로그인은 명시적이다. `sign_in`만 자격 증명 도우미(`git credential fill`)에 묻고, 다른 호출은 토큰이 없으면
    //    요청을 보내지 않고 `not_signed_in`으로 돌려준다.
    //  - 401을 받으면 그 토큰을 `git credential reject`로 도우미에서 지우고 메모리에서도 잊은 뒤, 도우미에 한 번 다시 묻고 같은 요청을
    //    한 번 다시 보낸다. 다시 받은 응답도 401이면 토큰을 잊고 `unauthorized`로 끝낸다.
    //  - 호출 한도가 남지 않았다고 알려 준 호스트는 초기화 시각까지 요청을 보내지 않는다.
    class hosting_service
    {
    public:
        hosting_service(hosting_transport& transport, credential_source& credentials, std::function<std::int64_t()> clock);
        hosting_service(const hosting_service&) = delete;
        hosting_service& operator=(const hosting_service&) = delete;

        [[nodiscard]] bool signed_in(const std::string& host_key) const;
        [[nodiscard]] std::string signed_in_user(const std::string& host_key) const;
        [[nodiscard]] std::vector<std::string> signed_in_hosts() const;
        // 이 실행 안에서만 토큰을 잊는다. 도우미가 가진 자격 증명은 그대로다.
        void sign_out(const std::string& host_key);
        void sign_out_all();
        // 호스트가 마지막으로 알려 준 호출 한도다. 아직 응답이 없으면 빈 값이다.
        [[nodiscard]] rate_limit_info last_rate_limit(const std::string& host_key) const;

        // 도우미에서 토큰을 받고 `/user`로 확인한다. 성공하면 계정 이름을 돌려준다. 도우미나 서버가 거절하면 토큰을 잊는다.
        [[nodiscard]] hosting_result<std::string> sign_in(const hosting_context& context, const process_cancellation_token& cancellation);
        [[nodiscard]] hosting_result<pull_request_page> list_pull_requests(const hosting_context& context, const pull_request_query& query, const process_cancellation_token& cancellation);
        [[nodiscard]] hosting_result<created_pull_request> create_pull_request(
            const hosting_context& context, const create_pull_request_params& params, const process_cancellation_token& cancellation);
        [[nodiscard]] hosting_result<ci_status_info> ci_status(const hosting_context& context, const std::string& sha, const process_cancellation_token& cancellation);

    private:
        struct outcome
        {
            hosting_http_response response {};
            // 요청을 보내지 못했을 때의 이유다.
            hosting_error preflight {};
        };

        [[nodiscard]] outcome send(const hosting_context& context, const hosting_http_request& request, const process_cancellation_token& cancellation);
        [[nodiscard]] credential_query query_of(const hosting_context& context) const;
        // 도우미에 묻는다. 성공하면 보관소에 넣는다.
        [[nodiscard]] credential_fill_result acquire(const hosting_context& context, const process_cancellation_token& cancellation);
        void remember_rate_limit(const std::string& host_key, const rate_limit_info& info);
        [[nodiscard]] std::optional<hosting_error> exhausted_limit(const std::string& host_key) const;
        // 응답 하나의 결과를 `hosting_result`의 오류 칸으로 옮긴다.
        template<class T>
        [[nodiscard]] hosting_result<T> preflight_failure(const outcome& attempt) const;

        hosting_transport* transport_ { nullptr };
        credential_source* credentials_ { nullptr };
        std::function<std::int64_t()> clock_ {};
        hosting_token_store tokens_ {};
        // 도우미를 동시에 두 번 부르지 않게 한다 (창이 두 번 뜨는 일을 막는다).
        std::mutex credential_mutex_ {};
        mutable std::mutex limit_mutex_ {};
        std::map<std::string, rate_limit_info> limits_ {};
    };
} // namespace cyella
