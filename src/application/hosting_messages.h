#pragma once

#include "domain/hosting_models.h"
#include "domain/hosting_remote.h"

#include <cstdint>
#include <string>
#include <variant>

namespace cyella {
    // GitHub·GitLab 호출은 git operation이 아니다: 프로세스를 띄우는 git operation과 달리 HTTP 응답을 기다리고, 사용자의 로그인
    // 상태를 메모리에 둔다. 그래서 `operation_kind`를 건드리지 않고 아래 일감(job)과 결과 메시지로 오간다.
    // 화면(controller)이 일감을 내면 driver가 워커에서 `hosting_service`로 돌리고, 결과는 relay를 거쳐 logic thread의
    // `hosting_result_intent`로 돌아온다. 늦은 결과는 `serial`로 가려 버린다.

    struct hosting_sign_in_job
    {};

    struct hosting_list_job
    {
        pull_request_query query {};
    };

    struct hosting_create_job
    {
        create_pull_request_params params {};
    };

    struct hosting_ci_job
    {
        std::string sha {};
    };

    struct hosting_job
    {
        // 결과가 같은 값으로 돌아온다. controller가 매번 올린다.
        std::uint64_t serial { 0 };
        // 이 일감을 낸 탭의 저장소 id다.
        std::string repository_id {};
        hosting_context context {};
        std::variant<hosting_sign_in_job, hosting_list_job, hosting_create_job, hosting_ci_job> work {};
    };

    using hosting_payload = std::variant<hosting_result<std::string>, hosting_result<pull_request_page>, hosting_result<created_pull_request>, hosting_result<ci_status_info>>;

    // 일감의 결과다. 워커가 relay로 올리고 driver가 controller에 넘긴다.
    struct hosting_result_intent
    {
        std::uint64_t serial { 0 };
        std::string repository_id {};
        std::string host_key {};
        // 일감의 종류와 같은 갈래다 (sign_in → string, list → page, create → created, ci → status).
        hosting_payload payload {};
        // 호출이 끝난 뒤 이 호스트에 로그인한 상태인가와 그 계정이다. 401로 토큰을 잊었으면 거짓이다.
        bool signed_in { false };
        std::string user {};
        // 호스트가 마지막으로 알려 준 호출 한도다.
        rate_limit_info rate_limit {};
        // 결과를 만든 때(유닉스 초)다.
        std::int64_t at { 0 };
    };

    // 로그아웃: 이 실행 안의 토큰만 잊는다. driver가 서비스에 전한다.
    struct hosting_sign_out_request
    {
        std::string host_key {};
    };
} // namespace cyella
