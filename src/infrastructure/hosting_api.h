#pragma once

#include "application/hosting_transport.h"
#include "domain/hosting_models.h"
#include "domain/hosting_remote.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // GitHub REST v3·GitLab REST v4 요청을 짓고 응답을 읽는 순수 함수들이다. 네트워크도 토큰도 모른다 — 요청 값과 응답 값만 오간다.
    // 응답 모양은 두 서비스의 공개 API 문서를 따른다 (GitHub: Pulls, Commit statuses, Check runs / GitLab: Merge requests, Pipelines).

    // 퍼센트 부호화한다. 영문자·숫자·`-._~`만 그대로 둔다 (GitLab 프로젝트 경로의 `/`도 `%2F`가 된다).
    [[nodiscard]] std::string percent_encode_component(std::string_view text);

    [[nodiscard]] hosting_http_request make_current_user_request(const hosting_repository& repository);
    [[nodiscard]] hosting_http_request make_list_pull_requests_request(const hosting_repository& repository, const pull_request_query& query);
    [[nodiscard]] hosting_http_request make_create_pull_request_request(const hosting_repository& repository, const create_pull_request_params& params);
    // GitHub는 combined status와 check-runs 둘, GitLab은 파이프라인 목록 하나다.
    [[nodiscard]] std::vector<hosting_http_request> make_ci_status_requests(const hosting_repository& repository, std::string_view sha);

    // 헤더에서 호출 한도를 읽는다 (`x-ratelimit-*`·`RateLimit-*`·`Retry-After`). 대소문자를 가리지 않는다.
    [[nodiscard]] rate_limit_info read_rate_limit(const hosting_http_response& response);
    // 2xx가 아닌 응답(또는 전송 실패)을 오류로 바꾼다. 403·429가 호출 한도인지는 헤더와 메시지로 가린다.
    [[nodiscard]] hosting_error classify_hosting_response(hosting_provider provider, const hosting_http_response& response);

    [[nodiscard]] hosting_result<std::string> parse_current_user_response(hosting_provider provider, const hosting_http_response& response);
    [[nodiscard]] hosting_result<pull_request_page> parse_pull_request_list_response(hosting_provider provider, const hosting_http_response& response, std::uint32_t page);
    [[nodiscard]] hosting_result<created_pull_request> parse_create_pull_request_response(hosting_provider provider, const hosting_http_response& response);
    // `responses`는 `make_ci_status_requests`와 같은 순서다.
    [[nodiscard]] hosting_result<ci_status_info> parse_ci_status_responses(const hosting_repository& repository, std::string_view sha, const std::vector<hosting_http_response>& responses);
} // namespace cyella
