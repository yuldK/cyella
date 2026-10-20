#pragma once

#include "domain/external_diff_tool.h"
#include "domain/user_profile.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    inline constexpr std::int32_t current_app_settings_schema_version { 1 };
    // 시작 페이지가 보여 주는 최근 저장소의 상한이다. 넘으면 오래된 항목부터 버린다.
    // **고정한 항목은 이 수에 들지 않는다.** 사용자가 직접 고른 것이라 버리지 않는다.
    inline constexpr std::size_t recent_repository_capacity { 20 };
    // `%APPDATA%\cyella` 아래의 설정 파일 이름이다.
    inline constexpr std::u8string_view app_settings_file_name { u8"settings.json" };
    // git의 기본 문맥 줄 수다. 설정이 이 범위(0~25)를 벗어나면 경고하고 기본값을 쓴다.
    inline constexpr std::uint32_t default_diff_context_lines { 3 };
    inline constexpr std::uint32_t maximum_diff_context_lines { 25 };

    // 최근에 연 저장소 하나다. 시각은 도메인이 시계를 모르도록 이미 만들어진 ISO 8601 UTC
    // 문자열로 받는다.
    struct recent_repository
    {
        // 작업 트리 절대 경로다. 구분자는 `\`로 통일해 저장한다.
        std::u8string path {};
        // 경로의 마지막 구성 요소다. 저장 시점에 계산한다.
        std::u8string display_name {};
        // `2026-10-20T09:30:12Z` 형식이다. 읽을 수 없는 값은 빈 문자열로 둔다.
        std::u8string opened_at {};
        // 사용자가 고정한 항목이다. 목록 앞에 모이고 상한에 걸려도 버려지지 않는다.
        bool pinned { false };

        [[nodiscard]] bool operator==(const recent_repository&) const noexcept = default;
    };

    enum class theme_preference
    {
        system,
        light,
        dark,
    };

    struct appearance_settings
    {
        theme_preference theme { theme_preference::system };

        [[nodiscard]] bool operator==(const appearance_settings&) const noexcept = default;
    };

    // 마지막으로 앱을 닫을 때의 창 배치다. 좌표는 가상 화면 기준 물리 픽셀이다.
    struct window_placement
    {
        std::int32_t x { 0 };
        std::int32_t y { 0 };
        std::int32_t width { 0 };
        std::int32_t height { 0 };
        bool maximized { false };

        [[nodiscard]] bool operator==(const window_placement&) const noexcept = default;
        // 크기가 양수여야 복원에 쓸 수 있다. 위치는 모니터 구성이 바뀔 수 있어 적용하는
        // 쪽이 따로 검사한다.
        [[nodiscard]] bool valid() const noexcept;
    };

    // 셸 판의 폭이다 (논리 픽셀). 값이 없으면 기본 폭을 쓴다. 범위를 다듬는 것은 화면 쪽(`shell_layout`)의 몫이다.
    struct pane_widths_settings
    {
        std::optional<float> sidebar {};
        std::optional<float> detail {};

        [[nodiscard]] bool operator==(const pane_widths_settings&) const noexcept = default;
    };

    // 화면 배치 설정이다. `ui` 객체 하나에 모아 두어 알 수 없는 키도 이 안에서 보존한다.
    struct ui_settings
    {
        pane_widths_settings pane_widths {};

        [[nodiscard]] bool operator==(const ui_settings&) const noexcept = default;
    };

    enum class diff_layout_preference
    {
        inline_view,
        split,
    };

    // diff를 처음 열 때의 보기와 hunk 둘레의 문맥 줄 수다. 문맥 줄 수는 `git diff -U<n>`에 그대로 간다.
    struct diff_settings
    {
        diff_layout_preference layout { diff_layout_preference::inline_view };
        std::uint32_t context_lines { default_diff_context_lines };

        [[nodiscard]] bool operator==(const diff_settings&) const noexcept = default;
    };

    // 문서 밖에 남는 앱 단위 설정이다. `%APPDATA%\cyella\settings.json` 한 파일에 저장한다.
    struct app_settings
    {
        std::int32_t schema_version { current_app_settings_schema_version };
        // 고정한 항목이 앞에 모이고, 두 무리 안에서 각각 최근 순이다.
        std::vector<recent_repository> recent_repositories {};
        // 세션 복원용이다. 열려 있던 저장소 탭의 경로를 탭 순서대로 담는다. 시작 탭은 담지 않는다.
        std::vector<std::u8string> open_tabs {};
        // 선택돼 있던 탭의 저장소 경로다. 인덱스가 아닌 경로로 두는 이유는 복원 때 중복·없는
        // 항목이 걸러져도 가리키는 탭이 바뀌지 않게 하려는 것이다. 비면 지정 없음이다.
        std::u8string active_tab {};
        // 비면 자동 탐색이다. 값이 있으면 절대 경로여야 한다 (읽을 때 검사한다).
        std::u8string git_executable {};
        std::vector<external_diff_tool> external_diff_tools {};
        appearance_settings appearance {};
        std::optional<window_placement> window {};
        ui_settings ui {};
        diff_settings diff {};
        // ---- 새 필드는 끝에 덧붙인다.
        // 저장소별 사용자 프로필(이름·user.name·user.email·서명 키)과, 열 때 프로필을 제안하는 경로 규칙, 적용해 둔 선택이다.
        std::vector<user_profile> profiles {};
        std::vector<profile_rule> profile_rules {};
        std::vector<profile_assignment> profile_assignments {};
        // 기본값에서 바꾼 단축키다. 명령 id → 정규 표기(`Ctrl+Shift+P`) 목록이고, 목록이 비면 "키 없음"이다. 없는 id는 기본값을 쓴다.
        std::map<std::string, std::vector<std::u8string>> shortcuts {};
        // 명령 팔레트에서 쓴 명령의 id다. 가장 최근 것이 앞이고 순위 매기기에 쓴다.
        std::vector<std::string> command_history {};

        [[nodiscard]] bool operator==(const app_settings&) const noexcept = default;
    };

    // 명령 팔레트에서 쓴 명령을 기억한다. 가장 최근 것이 앞에 오고 중복은 하나로 합치며 상한을 넘으면 오래된 것부터 버린다.
    inline constexpr std::size_t command_history_capacity { 30 };
    void record_command_use(app_settings& settings, std::string_view command);

    // 최근 항목을 자기 무리(고정·비고정)의 맨 앞으로 올린다. 같은 저장소가 있으면 경로 표기와
    // 시각을 갱신하고 **고정 여부는 보존한다** — 고정한 저장소를 다시 열었다고 고정이 풀리면
    // 놀랍다. 비고정이 상한을 넘으면 뒤에서 버린다. 빈 경로는 무시한다.
    void touch_recent_repository(app_settings& settings, std::u8string_view path, std::u8string opened_at);
    // 최근 항목의 경로를 새 위치로 바꾼다 (저장소 폴더를 옮겼을 때). 고정 여부와 위치는 그대로 두고, 표시 이름은 새 경로의 마지막 구성
    // 요소로 고친다. 새 경로를 이미 가진 다른 항목이 있으면 그것을 지운다 (같은 저장소가 둘이 되면 안 된다). 옛 경로가 목록에 없으면
    // 아무것도 바꾸지 않고 false다.
    bool relocate_recent_repository(app_settings& settings, std::u8string_view old_path, std::u8string new_path);
    // 목록에서 저장소를 지운다. 지운 항목이 있으면 true다.
    bool remove_recent_repository(app_settings& settings, std::u8string_view path);
    // 고정을 켜거나 끈다. 바뀐 것이 있으면 true다. 어느 쪽으로 바꾸든 그 항목은 새 무리의
    // 맨 앞에 선다 — 고정을 풀면 가장 최근에 연 저장소로 친다.
    bool set_recent_repository_pinned(app_settings& settings, std::u8string_view path, bool pinned);
    // 손으로 고친 파일처럼 불변이 깨진 목록을 바로잡는다. 같은 저장소 중복은 먼저 나온 것만
    // 남기고, 고정을 앞으로 모으고(무리 안 순서 유지), 비고정을 상한까지만 남긴다.
    void normalize_recent_repositories(std::vector<recent_repository>& repositories);

    [[nodiscard]] std::u8string_view theme_preference_name(theme_preference theme) noexcept;
    [[nodiscard]] std::optional<theme_preference> parse_theme_preference(std::u8string_view name) noexcept;

    [[nodiscard]] std::u8string_view diff_layout_preference_name(diff_layout_preference layout) noexcept;
    [[nodiscard]] std::optional<diff_layout_preference> parse_diff_layout_preference(std::u8string_view name) noexcept;

    // `2026-10-20T09:30:12Z` 형식의 UTC 문자열이다. 변환에 실패하면 빈 값이다.
    [[nodiscard]] std::u8string format_utc_timestamp(std::chrono::system_clock::time_point time);
} // namespace cyella
