#pragma once

#include "application/hosting_credentials.h"
#include "application/process_cancellation.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cyella {
    // 호스팅 API로 나가는 요청이다. 토큰은 여기 두지 않는다 — 헤더 표는 로그에 남겨도 되는 값만 담고,
    // `Authorization`은 전송 쪽이 토큰을 따로 받아 붙인다.
    enum class hosting_http_method : std::uint8_t
    {
        get,
        post,
    };

    struct hosting_http_header
    {
        std::string name {};
        std::string value {};
    };

    struct hosting_http_request
    {
        hosting_http_method method { hosting_http_method::get };
        std::string url {};
        std::vector<hosting_http_header> headers {};
        std::string body {};
        std::string content_type {};
    };

    struct hosting_http_response
    {
        // 0이면 상태 줄까지 오지 못했다 (`transport_error`가 이유다).
        int status_code { 0 };
        std::vector<hosting_http_header> headers {};
        std::string body {};
        std::string transport_error {};
        bool cancelled { false };

        // 이름은 대소문자를 가리지 않는다. 없으면 빈 view다.
        [[nodiscard]] std::string_view header(std::string_view name) const noexcept;
        [[nodiscard]] bool received() const noexcept
        {
            return status_code != 0;
        }
    };

    // 요청 하나를 보내고 답을 기다린다 (호출 스레드가 막힌다 — 워커에서 부른다). 구현은 luil의 비동기 HTTP client 위에 서고,
    // `bearer`가 비어 있지 않으면 `Authorization: Bearer`로 붙인다.
    class hosting_transport
    {
    public:
        hosting_transport() = default;
        hosting_transport(const hosting_transport&) = delete;
        hosting_transport& operator=(const hosting_transport&) = delete;
        virtual ~hosting_transport() = default;

        [[nodiscard]] virtual hosting_http_response execute(const hosting_http_request& request, const secret_text& bearer, const process_cancellation_token& cancellation) = 0;
    };
} // namespace cyella
