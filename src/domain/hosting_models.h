#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // GitHub PR·GitLab MR, CI 상태와 호스팅 API 오류의 값 타입이다. 두 서비스의 응답을 같은 모양으로 맞춘 뒤의 값이라
    // 화면과 controller는 서비스 종류를 몰라도 읽을 수 있다. 네트워크와 JSON을 모르는 순수 값이다.

    enum class pull_request_state : std::uint8_t
    {
        open,
        closed,
        merged,
    };

    // CI의 종합 상태다. `none`은 상태가 하나도 없다는 뜻이다 (CI를 쓰지 않거나 서버가 모르는 커밋).
    enum class ci_state : std::uint8_t
    {
        none,
        pending,
        success,
        failure,
        // 실행이 취소되었거나 서버 오류로 끝났다. 실패와 구별해 보인다.
        error,
    };

    struct pull_request_info
    {
        // GitHub의 `number`, GitLab의 `iid`다. 저장소 안에서 보이는 번호다.
        std::uint64_t number { 0 };
        std::string title {};
        std::string author {};
        std::string source_branch {};
        std::string target_branch {};
        pull_request_state state { pull_request_state::open };
        bool draft { false };
        // 다른 저장소(fork)의 브랜치에서 온 것이다. 이 경우 `pull/<n>/head`로만 받을 수 있다.
        bool from_fork { false };
        std::string head_sha {};
        std::string web_url {};
        std::string body {};
        // 유닉스 초다. 읽을 수 없으면 0이다.
        std::int64_t created_at { 0 };
        std::int64_t updated_at { 0 };
        // 목록 응답에 CI가 실려 있을 때만 있다 (GitLab의 `head_pipeline`).
        std::optional<ci_state> ci {};

        [[nodiscard]] bool operator==(const pull_request_info&) const noexcept = default;
    };

    struct ci_check_info
    {
        std::string name {};
        ci_state state { ci_state::none };
        std::string url {};

        [[nodiscard]] bool operator==(const ci_check_info&) const noexcept = default;
    };

    struct ci_status_info
    {
        std::string sha {};
        ci_state state { ci_state::none };
        std::vector<ci_check_info> checks {};
        // 파이프라인(GitLab)이나 상태 페이지의 웹 주소다. 없으면 비어 있다.
        std::string web_url {};

        [[nodiscard]] bool operator==(const ci_status_info&) const noexcept = default;
    };

    // 응답 헤더에서 읽은 호출 한도다. 서비스가 알려 주지 않은 값은 없다.
    struct rate_limit_info
    {
        std::optional<std::int64_t> limit {};
        std::optional<std::int64_t> remaining {};
        // 한도가 초기화되는 때(유닉스 초)다.
        std::optional<std::int64_t> reset_at {};
        // `Retry-After`가 알린 대기 초다.
        std::optional<std::int64_t> retry_after_seconds {};

        [[nodiscard]] bool operator==(const rate_limit_info&) const noexcept = default;
    };

    enum class hosting_error_kind : std::uint8_t
    {
        none,
        // 아직 로그인하지 않았다. 요청을 보내지 않았다.
        not_signed_in,
        // 자격 증명 도우미가 토큰을 주지 않았다 (도우미 없음, 취소, 빈 답).
        credential_unavailable,
        // 401이다. 한 번 다시 물은 뒤에도 거절되었다.
        unauthorized,
        // 403이다 (권한 없음). 호출 한도는 `rate_limited`다.
        forbidden,
        rate_limited,
        not_found,
        // 422·409·400: 서버가 요청을 받지 않았고 이유를 `message`에 적었다 (이미 같은 PR이 있음 등).
        rejected,
        server_error,
        // 연결하지 못했거나 시간이 지났다.
        network,
        // 2xx인데 JSON을 읽을 수 없다.
        invalid_response,
        cancelled,
        // 이 저장소는 연동할 수 없다 (서비스를 모르는 호스트).
        unsupported,
    };

    struct hosting_error
    {
        hosting_error_kind kind { hosting_error_kind::none };
        int status_code { 0 };
        // 서버가 준 이유(`message`)다. 토큰을 담지 않는다.
        std::string message {};
        rate_limit_info rate_limit {};

        [[nodiscard]] bool empty() const noexcept
        {
            return kind == hosting_error_kind::none;
        }
    };

    // 한 호출의 결과다. 성공이면 `value`가 있고, 아니면 `error`가 이유다. 호출 한도는 성공 응답에서도 읽어 둔다.
    template<class T>
    struct hosting_result
    {
        std::optional<T> value {};
        hosting_error error {};
        rate_limit_info rate_limit {};

        [[nodiscard]] bool ok() const noexcept
        {
            return value.has_value();
        }
    };

    struct pull_request_query
    {
        // 거짓이면 열린 것만, 참이면 닫힌 것과 병합된 것까지 모두다.
        bool include_closed { false };
        std::uint32_t page { 1 };
        std::uint32_t per_page { 30 };
    };

    struct pull_request_page
    {
        std::vector<pull_request_info> items {};
        std::uint32_t page { 1 };
        // 다음 쪽이 있으면 그 번호다 (GitHub의 `Link: rel="next"`, GitLab의 `X-Next-Page`).
        std::optional<std::uint32_t> next_page {};
        // 읽지 못해 건너뛴 항목 수다.
        std::uint32_t skipped { 0 };
    };

    struct create_pull_request_params
    {
        std::string title {};
        std::string body {};
        std::string source_branch {};
        std::string target_branch {};
        bool draft { false };
    };

    struct created_pull_request
    {
        std::uint64_t number { 0 };
        std::string title {};
        std::string web_url {};
        std::string head_sha {};
    };

    // 호스팅 서버에 빈 저장소를 새로 만든다 (로컬 저장소를 게시하기 전). 첫 커밋(README)을 만들지 않는다 — 서버에 커밋이 생기면
    // 이어서 낼 push가 non-fast-forward로 거절된다.
    struct create_repository_params
    {
        std::string name {};
        std::string description {};
        // GitHub의 조직, GitLab의 그룹 경로(`group/sub`)다. 비우거나 내 계정 이름이면 내 계정 아래다.
        std::string owner {};
        bool is_private { true };
    };

    struct created_repository
    {
        // `owner/name`(GitLab은 `group/sub/name`)이다.
        std::string full_name {};
        std::string web_url {};
        std::string https_url {};
        std::string ssh_url {};
    };

    // 사람에게 보일 한 줄이다. `now`(유닉스 초)로 한도 초기화까지 남은 시간을 말한다 (호출 한도 오류가 아니면 쓰지 않는다).
    [[nodiscard]] std::string describe_hosting_error(const hosting_error& error, std::int64_t now);
    // "N분 후"처럼 남은 시간을 말한다. 이미 지났으면 "곧"이다.
    [[nodiscard]] std::string describe_remaining_time(std::int64_t seconds);

    // `2026-10-20T09:30:12Z`·`...+09:00`·소수 초가 붙은 ISO 8601을 유닉스 초로 바꾼다. 읽을 수 없으면 0이다.
    [[nodiscard]] std::int64_t parse_iso8601_seconds(std::string_view text) noexcept;

    [[nodiscard]] std::string_view ci_state_name(ci_state state) noexcept;
    // 상태 여럿을 하나로 합친다. 실패·오류가 있으면 실패, 아니면 진행 중이 있으면 진행 중, 성공이 하나라도 있으면 성공이다. 비었으면 none이다.
    [[nodiscard]] ci_state combine_ci_states(const std::vector<ci_state>& states) noexcept;
} // namespace cyella
