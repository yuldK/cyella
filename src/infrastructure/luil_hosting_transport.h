#pragma once

#include "application/hosting_transport.h"

#include "luil/net/http_client.h"

#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace cyella {
    // luil의 비동기 HTTP client(`luil::net::http_client`) 위에 선 `hosting_transport`다. client는 자기 thread에서 답을 `deliver`로 건네고,
    // 이 클래스는 그 답을 요청한 워커 thread에 돌려주도록 표(ticket)별로 기다린다. 앱의 logic·UI thread는 막지 않는다.
    //
    // `deliver`는 client의 thread에서 불리므로 여기서는 값을 옮기고 깨우기만 한다. `stop()`을 부르면 기다리던 호출이 모두 깨어난다
    // (client는 멈춘 뒤 `deliver`를 부르지 않으므로 이 깨우기가 없으면 워커가 영원히 기다린다).
    class luil_hosting_transport final : public hosting_transport
    {
    public:
        // client를 세운다. 실패하면 null이고 `error`에 이유가 있다.
        // `use_system_proxy`는 되돌이 주소를 쓰는 시험이 끈다.
        [[nodiscard]] static std::unique_ptr<luil_hosting_transport> create(std::u8string user_agent, bool use_system_proxy, std::u8string& error);

        luil_hosting_transport(const luil_hosting_transport&) = delete;
        luil_hosting_transport& operator=(const luil_hosting_transport&) = delete;
        ~luil_hosting_transport() override;

        [[nodiscard]] hosting_http_response execute(const hosting_http_request& request, const secret_text& bearer, const process_cancellation_token& cancellation) override;

        // 날아가 있는 요청을 모두 접는다 (종료 0단계).
        void cancel_all() noexcept;
        // client를 멈추고 기다리는 호출을 모두 깨운다. 멱등이다.
        void stop() noexcept;

    private:
        luil_hosting_transport() = default;

        void deliver(luil::net::http_response response);

        std::mutex mutex_ {};
        std::condition_variable arrived_ {};
        std::map<std::uint64_t, luil::net::http_response> completed_ {};
        bool stopped_ { false };
        std::unique_ptr<luil::net::http_client> client_ {};
    };
} // namespace cyella
