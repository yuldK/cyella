#pragma once

#include "domain/hosting_models.h"
#include "domain/hosting_remote.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cyella {
    // GitHub·GitLab 화면이 읽는 값이다. `hosting_controller`가 상태에서 짓는 순수 값이고, 사이드바 구역·PR 상세·커밋 상세의 CI 표시·
    // 환경 설정 "호스팅" 화면이 이것만 읽는다.

    enum class hosting_support : std::uint8_t
    {
        // 저장소에 원격이 없다 (또는 URL을 읽을 수 없다).
        no_remote,
        // GitHub·GitLab으로 알아보지 못한 호스트다. 환경 설정의 호스팅 표에 더하면 된다.
        unsupported,
        supported,
    };

    // CI 표시 하나다. 커밋 상세의 한 줄과 PR 상세의 요약이 같은 값을 쓴다.
    struct ci_check_view
    {
        std::u8string name {};
        ci_state state { ci_state::none };
        std::string url {};
    };

    struct ci_mark_view
    {
        ci_state state { ci_state::none };
        // "CI 통과", "CI 실패", "CI 진행 중", "CI 오류", "CI 상태 없음", "CI 확인 중…" 같은 한 줄이다.
        std::u8string text {};
        // 확인 중이다 (요청을 보냈고 아직 답이 없다).
        bool loading { false };
        // 읽지 못했을 때의 이유다.
        std::u8string error {};
        std::string web_url {};
        std::vector<ci_check_view> checks {};
        // 이 커밋의 CI를 읽은 서비스 이름이다 ("GitHub").
        std::u8string source {};

        [[nodiscard]] bool operator==(const ci_mark_view&) const noexcept = default;
    };

    struct pull_request_detail_view
    {
        std::uint64_t number { 0 };
        std::u8string title {};
        std::u8string author {};
        std::u8string source_branch {};
        std::u8string target_branch {};
        // "열림", "초안", "병합됨", "닫힘"이다.
        std::u8string state_text {};
        pull_request_state state { pull_request_state::open };
        bool draft { false };
        bool from_fork { false };
        std::string head_sha {};
        std::string web_url {};
        std::u8string body {};
        std::u8string updated_text {};
        ci_mark_view ci {};
        // "PR"·"MR"이다.
        std::u8string abbreviation {};
        // 체크아웃할 로컬 브랜치 이름이다 (`pr/7`, `mr/7`). 이미 그 브랜치에 있으면 `checked_out`이다.
        std::u8string local_branch {};
        bool checked_out { false };
        bool can_checkout { false };
        // 방금 만들었다면 그 주소다. 상세 맨 위에 보인다.
        bool just_created { false };
    };

    // 한 저장소 탭의 호스팅 값 전체다. 목록은 상태가 쥔 벡터를 공유한다 (frame마다 복사하지 않는다).
    struct hosting_repository_view
    {
        hosting_support support { hosting_support::no_remote };
        hosting_provider provider { hosting_provider::unknown };
        std::u8string provider_name {};
        // 토큰을 두는 열쇠이자 환경 설정 표의 호스트다 (`github.com`).
        std::string host_key {};
        std::u8string abbreviation { u8"PR" };
        bool signed_in { false };
        bool signing_in { false };
        std::u8string user {};
        bool loading { false };
        bool loaded { false };
        bool include_closed { false };
        bool has_more { false };
        std::u8string error_text {};
        std::shared_ptr<const std::vector<pull_request_info>> pulls {};
        // 열린 PR 상세다.
        std::optional<pull_request_detail_view> detail {};
        // PR 만들기를 쓸 수 있는가와 막힌 이유다.
        bool can_create { false };
        std::u8string create_blocked_reason {};
        // "API 남은 호출 4999/5000"처럼 호출 한도를 알 때만 있다.
        std::u8string rate_limit_text {};
        // 목록 행마다의 CI 표시다 (목록 응답이나 이미 읽어 둔 CI가 있을 때만).
        std::vector<std::pair<std::uint64_t, ci_state>> list_ci {};
    };

    // 환경 설정 "호스팅" 화면의 호스트 한 줄이다. 로그인 상태는 메모리 값이다.
    struct hosting_host_row_view
    {
        std::string host {};
        hosting_provider provider { hosting_provider::github };
        std::string api_base {};
        // 내장(github.com·gitlab.com)이면 표에서 지울 수 없다.
        bool builtin { false };
        bool signed_in { false };
        std::u8string user {};
        std::u8string rate_limit_text {};
    };
} // namespace cyella
