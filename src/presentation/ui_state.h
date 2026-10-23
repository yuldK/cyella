#pragma once

#include "application/diff_view_protocol.h"
#include "application/git_operation.h"
#include "application/repository_tabs.h"
#include "domain/operation_status.h"
#include "presentation/dialog_state.h"
#include "presentation/palette/palette_state.h"
#include "presentation/selector_model.h"
#include "presentation/shell_layout.h"

#include "luil/text/text_edit.h"
#include "luil/ui/ui_interaction.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cyella {
    // 화면만 아는 상태(선택·스크롤·판 폭·접힘·입력 초안)다. driver가 소유하고 logic thread에서만 바꾼다.
    // git 상태의 정본은 workspace·세션이고, 탭 목록의 정본은 `repository_tabs`다. 여기 있는 것은
    // 껐다 켜면 잃어도 되는 것들이다.

    // 텍스트 입력 칸의 대상이다. luil `text_input_target`에 값 그대로 실린다 (0은 luil의 none이라 쓰지 않는다).
    enum class text_target : std::uint32_t
    {
        none = 0,
        start_search,
        sidebar_filter,
        commit_subject,
        commit_body,
        clone_url,
        clone_path,
        // 환경 설정 화면의 입력 칸이다 (`settings_controller`의 초안).
        settings_git_path,
        settings_tool_extensions,
        settings_tool_executable,
        settings_tool_arguments,
        settings_key_name,
        settings_key_comment,
        settings_test_host,
        // 입력 대화 상자(브랜치·태그·push·인증)의 칸이다. 대화 상자마다 최대 셋이고 어느 상자의 것인지는 `form_inputs`가 안다.
        form_field_0 = 100,
        form_field_1,
        form_field_2,
        // 인터랙티브 rebase 편집기의 메시지 칸이다. 다른 칸과 겹치지 않게 200부터 쓴다. 본문은 여러 줄이다.
        rebase_message_subject = 200,
        rebase_message_body,
        // 툴바 저장소·브랜치 선택 상자 popup의 거르기 칸이다.
        selector_filter = 110,
        // 환경 설정 "호스팅" 화면의 호스트·API 주소 칸이다. 다른 칸과 겹치지 않게 500부터 쓴다.
        settings_hosting_host = 500,
        settings_hosting_api_base,
        // 명령 팔레트의 거르기 칸, 환경 설정의 단축키 입력 칸, 사용자 프로필·규칙 입력 칸이다. 다른 칸과 겹치지 않게 300부터 쓴다.
        palette_filter = 300,
        settings_shortcut_input,
        settings_profile_name,
        settings_profile_user_name,
        settings_profile_user_email,
        settings_profile_signing_key,
        settings_rule_prefix,
        // 환경 설정 "서명" 쪽의 프로그램 경로·allowed signers 입력 칸이다. 팔레트·단축키·프로필 칸의 300대와 겹치지 않게 400부터 쓴다.
        settings_signing_gpg_program = 400,
        settings_signing_ssh_program,
        settings_signing_allowed_signers,
        settings_ui_font = 600,
        settings_code_font,
    };

    // 사이드바 구역이다. 순서가 곧 화면 순서다.
    enum class sidebar_section : std::uint8_t
    {
        local,
        remote,
        tags,
        stashes,
        // submodule·worktree·Git LFS 구역이다. 뒤에 덧붙여 기존 비트 번호를 지킨다.
        submodules,
        worktrees,
        lfs,
        // GitHub·GitLab의 PR·MR 구역이다. 여덟째이자 `collapsed_sections`(uint8)의 마지막 비트다.
        pull_requests,
    };

    inline constexpr std::size_t sidebar_section_count { 8 };

    // 상세 패널이 보이는 것이다.
    enum class detail_mode
    {
        // 고른 것이 없다.
        none,
        commit,
        // WIP 행을 골랐다. 작업 영역(unstaged·staged·커밋 상자)이다.
        working,
        // 사이드바에서 연 PR·MR이다. 커밋·WIP를 고르면 닫힌다.
        pull_request,
    };

    // 여러 커밋을 고른 비교의 종류다.
    enum class commit_compare_kind : std::uint8_t
    {
        // Ctrl+클릭: 두 커밋을 비교한다 (`git diff 오래된 새것`).
        pair,
        // Shift+클릭: 한쪽이 다른 쪽의 조상이라 그 사이 범위를 합친 변경이다 (`git diff 오래된^ 새것`).
        range,
        // Shift+클릭인데 조상 관계를 git에 묻는 중이다. 답이 오면 pair나 range로 정한다.
        undecided,
    };

    // 그래프에서 여러 커밋을 고른 비교다. 기준 커밋과 마지막에 누른 커밋(`tab_ui_state::selected_sha`) 둘로 정해진다.
    struct commit_compare_state
    {
        // 기준 커밋이다. Ctrl·Shift+클릭을 거듭해도 그대로이고 마지막에 누른 커밋만 바뀐다.
        std::string anchor {};
        commit_compare_kind kind { commit_compare_kind::pair };
        // 낸 비교 요청이다. 세션의 결과가 이 요청의 것일 때만 상세 패널에 보인다. `undecided`면 아직 내지 않았다.
        show_range_request request {};
        // `undecided`일 때 조상 관계를 묻는 operation이다. 답 없이 끝나면 두 커밋 비교로 정한다.
        operation_id ancestry_query { 0 };
    };

    struct tab_ui_state
    {
        // 고른 커밋의 SHA다. WIP 행이면 `graph_wip_sha`다.
        std::string selected_sha {};
        // 변경 사항에서 이력으로 돌아갈 때 이전 커밋과 그래프 위치를 되살린다.
        std::string history_sha {};
        // 사이드바에서 고른 항목의 키다.
        std::u8string selected_sidebar_key {};
        float graph_scroll { 0.0f };
        float sidebar_scroll { 0.0f };
        float detail_scroll { 0.0f };
        // 작은 높이에서 변경 파일과 커밋 작성 영역을 함께 흘리는 위치다.
        float working_scroll { 0.0f };
        // 커밋 상세의 메시지·메타 칸이다. 커밋을 바꾸면 처음으로 돌아간다.
        float commit_meta_scroll { 0.0f };
        // 변경 파일 트리에서 접은 폴더다 (끝에 `/`가 없는 경로). 커밋을 바꿔도 같은 폴더는 접힌 채다.
        std::set<std::string> collapsed_folders {};
        float diff_scroll { 0.0f };
        // 접힌 구역이다 (`sidebar_section` 비트).
        std::uint8_t collapsed_sections { 0 };
        luil::text::text_edit_state sidebar_filter {};
        luil::text::text_edit_state commit_subject {};
        luil::text::text_edit_state commit_body {};
        bool amend { false };
        // 커밋 상자의 "서명" 토글을 사용자가 바꾼 값이다. 값이 없으면 설정 `commit.gpgsign`을 따른다.
        std::optional<bool> sign {};
        // 상세 패널에서 고른 파일의 diff 요청이다. 있으면 그래프 자리를 diff가 덮는다.
        std::optional<diff_file_request> diff {};
        // 낸 커밋 operation이다. 성공하면 커밋 상자를 비운다.
        operation_id pending_commit { 0 };
        // 사용자가 다룬 입력 필요 operation이다 (체크아웃 정책을 골랐다). 그보다 새 것만 탭에 🔔를 단다.
        operation_id dismissed_input { 0 };
        // 체크아웃 정책 대화 상자를 닫은 operation이다. 그보다 새 것만 대화 상자로 뜬다.
        operation_id dismissed_dialog { 0 };
        // 체크아웃 정책 대화 상자의 선택이다.
        std::optional<dirty_checkout_state> dirty {};
        // 여러 커밋을 고른 비교다. 그냥 클릭·WIP·Esc로 풀린다. `selected_sha`는 그동안에도 마지막에 누른 커밋이다.
        std::optional<commit_compare_state> compare {};

        [[nodiscard]] bool section_collapsed(const sidebar_section section) const noexcept
        {
            return (collapsed_sections & (1u << static_cast<unsigned>(section))) != 0;
        }
    };

    // Clone 대화 상자다. 열려 있는 동안만 값이 있다.
    struct clone_dialog_state
    {
        luil::text::text_edit_state url {};
        luil::text::text_edit_state path {};
        // 낸 clone operation이다. 0이면 아직 입력 중이다.
        operation_id operation { 0 };
        std::u8string error {};
    };

    // 알림의 조치 단추다 ("Pull 후 다시 push", 프로필 적용, 커밋 전 경고). 눌리면 `notification_action_intent`가 키를 나른다.
    struct notification_action
    {
        std::u8string key {};
        std::u8string label {};

        [[nodiscard]] bool operator==(const notification_action&) const noexcept = default;
    };

    enum class notification_severity : std::uint8_t
    {
        info,
        warning,
        error,
    };

    // 알림 센터가 보이는 알림의 종류다. 경고·오류는 그 심각도만 남긴다.
    enum class notification_filter : std::uint8_t
    {
        all,
        warning,
        error,
    };

    // 알림 하나다. 오른쪽 아래 토스트로 잠깐 뜨고, 상태 줄의 종(알림 센터)에 남는다.
    struct notification
    {
        std::uint64_t id { 0 };
        notification_severity severity { notification_severity::info };
        // 실패한 작업의 머리말이다 ("Push 실패"). 안내는 비어 있다.
        std::u8string title {};
        std::u8string text {};
        // [자세히]가 펴는 줄이다. 실행한 명령줄과 git 원문이다.
        std::vector<std::u8string> details {};
        std::vector<notification_action> actions {};
        // 실패 알림의 operation이다. 조치가 다시 낼 요청을 여기서 찾는다.
        operation_id operation { 0 };
        // 이 알림이 가리키는 저장소다. 조치는 그 저장소 탭으로 옮겨 실행한다. 비면 저장소와 무관하다.
        std::string repository_id {};
        std::chrono::system_clock::time_point at {};
        // 토스트로 떠 있다. 닫히거나 시간이 지나 내려가도 알림 센터에는 남는다.
        bool toast { true };
        // 토스트가 떠오르기 시작한 시각이다. 이때부터 `toast_enter_duration` 동안 아래에서 떠오르며 짙어진다.
        std::chrono::steady_clock::time_point toast_shown_at {};
        // 토스트가 저절로 내려갈 시각이다. 없으면 사용자가 닫을 때까지 남는다 (조치가 달린 알림).
        std::optional<std::chrono::steady_clock::time_point> toast_until {};
        // 토스트가 물러나기 시작한 시각이다. `toast_leave_duration` 동안 흐려지며 자리를 접고, 끝나면 `toast`가 거짓이 된다.
        std::optional<std::chrono::steady_clock::time_point> toast_leaving_at {};
        // 물러남이 끝나면 알림 센터에서도 지운다 (×로 닫았다).
        bool remove_after_leave { false };
        bool expanded { false };
        // 알림 센터를 연 뒤에 들어온 것만 거짓이다. 종 아이콘의 점을 정한다.
        bool read { false };
    };

    // 알림 센터가 기억하는 개수다. 넘으면 오래된 것부터 버린다.
    inline constexpr std::size_t notification_capacity { 100 };
    // 토스트가 떠오르고 물러나는 시간이다. 들어올 때를 조금 길게 잡아 눈이 따라가게 한다.
    inline constexpr std::chrono::milliseconds toast_enter_duration { 240 };
    inline constexpr std::chrono::milliseconds toast_leave_duration { 200 };

    // diff 웹뷰가 지금 보이는 모습이다. 웹뷰를 쥔 driver가 frame마다 채우고, 머리 막대(전환 단추)와 대체 보기가 읽는다.
    struct diff_surface_state
    {
        diff_layout layout { diff_layout::inline_view };
        image_compare_mode image_mode { image_compare_mode::side_by_side };
        diff_content content { diff_content::empty };
        // 웹뷰로 그릴 수 있는가. 거짓이면 텍스트 대체 보기를 쓴다 (WebView2 Runtime이 없거나 뷰어를 못 찾은 경우).
        // 기본이 거짓인 것은 웹뷰를 쥔 쪽이 확인해 올리기 전에는 대체 보기가 안전하기 때문이다.
        bool webview_available { false };
        // 줄 수 상한으로 잘린 diff다. 머리 막대가 알려 준다.
        bool truncated { false };
        // 보는 파일에 textconv가 걸려 있다. 머리 막대가 "textconv" 배지를 보인다. 비면 걸려 있지 않다.
        std::string textconv_driver {};

        [[nodiscard]] bool operator==(const diff_surface_state&) const noexcept = default;
    };

    // 툴바 선택 상자가 펼친 목록이다. 한 번에 하나만 열린다. 좌표는 창 좌표의 물리 픽셀이다.
    struct selector_state
    {
        selector_kind kind { selector_kind::repository };
        float x { 0.0f };
        float y { 0.0f };
        float width { 0.0f };
        // 거르는 글이다. 열 때마다 비운다.
        luil::text::text_edit_state filter {};
    };

    struct ref_list_state
    {
        std::string sha {};
        float x { 0.0f };
        float y { 0.0f };
        float scroll { 0.0f };
    };

    struct ui_state
    {
        std::map<repository_tab_id, tab_ui_state> tabs {};
        float sidebar_width { default_sidebar_width };
        float detail_width { default_detail_width };
        bool operation_log_open { false };
        float log_scroll { 0.0f };
        float tab_scroll { 0.0f };
        bool reveal_selected_tab { true };
        luil::text::text_edit_state start_search {};
        float start_scroll { 0.0f };
        // medium·narrow에서 상세 서랍이 열려 있다.
        bool drawer_open { false };
        // narrow의 아이콘 막대에서 연 구역이다. 떠 있는 판으로 그 구역만 보인다.
        std::optional<sidebar_section> rail_section {};
        std::optional<clone_dialog_state> clone {};
        // 오래된 것이 앞이다.
        std::vector<notification> notifications {};
        std::uint64_t notification_serial { 0 };
        // 상태 줄의 종을 눌러 연 알림 센터다. 열려 있는 동안 토스트는 띄우지 않는다.
        bool notifications_open { false };
        float notifications_scroll { 0.0f };
        // 알림 센터 머리의 거르기다. 센터를 열 때마다 전체로 돌아간다.
        notification_filter notifications_filter { notification_filter::all };
        // IME 조합 중인 칸의 표시 글이다. 한 번에 한 칸만 조합한다.
        std::optional<luil::text_composition_event> composition {};
        diff_surface_state diff_surface {};
        // 사용자가 연 대화 상자(브랜치·태그·push·확인)다. 한 번에 하나다.
        std::optional<dialog_state> dialog {};
        std::uint64_t dialog_serial { 0 };
        // 열린 입력 대화 상자의 칸과 선택이다 (`dialog`·체크아웃 정책·인증 질문 어느 것이든 하나만 보인다).
        form_inputs form {};
        // 열린 우클릭 메뉴다.
        std::optional<menu_state> menu {};
        std::optional<ref_list_state> ref_list {};
        dialog_memory memory {};
        // 열린 저장소·브랜치 선택 상자 목록이다.
        std::optional<selector_state> selector {};
        // 열린 명령 팔레트다.
        std::optional<palette_state> palette {};

        // 가장 최근 알림이다. 없으면 nullptr이다.
        [[nodiscard]] const notification* latest_notification() const noexcept
        {
            return notifications.empty() ? nullptr : &notifications.back();
        }
    };

    [[nodiscard]] inline luil::text_input_target to_luil_target(const text_target target) noexcept
    {
        return static_cast<luil::text_input_target>(static_cast<std::uint32_t>(target));
    }

    [[nodiscard]] inline text_target from_luil_target(const luil::text_input_target target) noexcept
    {
        const auto value { static_cast<std::uint32_t>(target) };
        const bool basic { value >= static_cast<std::uint32_t>(text_target::start_search) && value <= static_cast<std::uint32_t>(text_target::settings_test_host) };
        const bool form { value >= static_cast<std::uint32_t>(text_target::form_field_0) && value <= static_cast<std::uint32_t>(text_target::form_field_2) };
        const bool rebase { value >= static_cast<std::uint32_t>(text_target::rebase_message_subject) && value <= static_cast<std::uint32_t>(text_target::rebase_message_body) };
        const bool selector { value == static_cast<std::uint32_t>(text_target::selector_filter) };
        const bool signing { value >= static_cast<std::uint32_t>(text_target::settings_signing_gpg_program) && value <= static_cast<std::uint32_t>(text_target::settings_signing_allowed_signers) };
        const bool hosting { value >= static_cast<std::uint32_t>(text_target::settings_hosting_host) && value <= static_cast<std::uint32_t>(text_target::settings_hosting_api_base) };
        const bool commands { value >= static_cast<std::uint32_t>(text_target::palette_filter) && value <= static_cast<std::uint32_t>(text_target::settings_rule_prefix) };
        const bool fonts { value >= static_cast<std::uint32_t>(text_target::settings_ui_font) && value <= static_cast<std::uint32_t>(text_target::settings_code_font) };
        return basic || form || rebase || selector || hosting || commands || signing || fonts ? static_cast<text_target>(value) : text_target::none;
    }
} // namespace cyella
