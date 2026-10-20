#pragma once

#include <cstdint>
#include <string>

namespace cyella {
    // GitHub·GitLab 화면이 내는 화면 메시지다. `hosting_controller`가 처리한다. 사이드바 PULL REQUESTS 행을 두 번 누르거나 Enter를 치거나
    // UI Automation으로 Invoke하면 `activate_sidebar_item_intent{pull_request, path}`가 오고, `path`가 행의 동작이다 (`hosting_row_*`).

    // 사이드바 행의 동작 열쇠(`sidebar_row_view::path`)다. 숫자만이면 그 번호의 PR·MR을 연다.
    inline constexpr const char* hosting_row_login { "login" };
    inline constexpr const char* hosting_row_refresh { "refresh" };
    inline constexpr const char* hosting_row_toggle_closed { "toggle_closed" };
    inline constexpr const char* hosting_row_more { "more" };
    inline constexpr const char* hosting_row_create { "create" };

    enum class hosting_detail_action : std::uint8_t
    {
        // 브라우저에서 PR·MR 페이지를 연다.
        open_browser,
        // `fetch <remote> pull/<n>/head:pr/<n>` 뒤 체크아웃한다.
        checkout,
        // 상세를 닫는다.
        close,
        // CI 상태를 다시 읽는다.
        refresh_ci,
    };

    // PR 상세 판의 단추다.
    struct hosting_detail_action_intent
    {
        hosting_detail_action action { hosting_detail_action::close };
    };

    // 현재 탭의 원격 호스트에 로그인한다 (자격 증명 도우미에 토큰을 묻는다).
    struct hosting_sign_in_intent
    {};

    // 환경 설정 "호스팅" 화면의 [로그아웃]이다. 이 실행의 메모리 토큰만 잊는다.
    struct hosting_sign_out_intent
    {
        std::string host_key {};
    };
} // namespace cyella
