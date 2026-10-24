#pragma once

#include "application/git_operation.h"
#include "application/repository_tabs.h"
#include "domain/ref.h"
#include "presentation/ui_state.h"

#include "luil/ui/image_element.h"
#include "luil/ui/ui_interaction.h"

#include <cstdint>
#include <string>

namespace cyella {
    // 화면이 내는 메시지다. 저장소 operation은 `app_messages.h`의 `submit_operation_intent`로 바로 내고,
    // 여기에는 화면 상태(`ui_state`)·탭·설정을 바꾸는 것만 둔다. 둘을 나눈 이유는 백엔드 메시지
    // 헤더가 여러 백엔드 기능의 합류 지점이라 화면 메시지로 붐비지 않게 하려는 것이다.

    // UI thread가 실행하는 명령이다 (`app_ui_command::command`). OS 대화 상자처럼 logic thread가 할 수 없는 일이다.
    enum class ui_command_id : std::uint32_t
    {
        // 폴더를 골라 저장소로 연다. 결과는 `folder_chosen_intent{open}`이다.
        open_folder = 1,
        // 폴더를 골라 그 자리에 저장소를 만든다.
        init_folder,
        // Clone 대상 폴더를 고른다.
        clone_target_folder,
        // 환경 설정의 파일 고르기다. 결과는 `settings_file_chosen_intent`다.
        choose_git_executable,
        choose_tool_executable,
        // 없어진 최근 저장소의 새 위치를 고른다. 명령 인자가 옛 경로이고, 결과는 `folder_chosen_intent{locate}`다.
        locate_folder,
    };

    enum class folder_purpose
    {
        open,
        init,
        clone_target,
        locate,
    };

    // UI thread의 폴더 대화 상자 결과다. 취소했으면 오지 않는다.
    struct folder_chosen_intent
    {
        folder_purpose purpose { folder_purpose::open };
        std::u8string path {};
        // `locate`일 때 위치를 찾는 최근 저장소의 옛 경로다.
        std::u8string origin {};
    };

    // ------------------------------------------------------------------ 탭

    struct select_tab_intent
    {
        repository_tab_id id { 0 };
    };

    struct close_tab_intent
    {
        repository_tab_id id { 0 };
    };

    struct reorder_tab_intent
    {
        repository_tab_id moved { 0 };
        repository_tab_id target { 0 };
        // 거짓이면 대상 탭의 앞, 참이면 뒤에 끼운다 (놓은 자리가 대상의 어느 반쪽인가).
        bool after { false };
    };

    struct new_tab_intent
    {};

    // 키보드의 Ctrl+Tab·Ctrl+Shift+Tab이다.
    struct cycle_tab_intent
    {
        bool forward { true };
    };

    // Ctrl+W다. 선택된 탭을 닫는다.
    struct close_selected_tab_intent
    {};

    // F5다. 선택된 저장소를 다시 읽는다.
    struct refresh_intent
    {};

    struct scroll_tabs_intent
    {
        float delta { 0.0f };
        bool absolute { false };
    };

    // ------------------------------------------------------------------ 셸

    // 테마를 system → light → dark 차례로 바꾸고 설정에 저장한다.
    struct cycle_theme_intent
    {};

    // 안내를 알림으로 띄운다 (토스트로 뜨고 알림 센터에 남는다).
    struct show_notice_intent
    {
        std::u8string text {};
        bool warning { false };
    };

    // 알림을 지운다 (토스트와 알림 센터에서 함께). `id`가 0이면 가장 최근 알림이다.
    struct dismiss_notification_intent
    {
        std::uint64_t id { 0 };
    };

    // 알림의 [자세히]를 펴거나 접는다.
    struct toggle_notification_details_intent
    {
        std::uint64_t id { 0 };
    };

    // 상태 줄의 종이다. 열면 모든 알림을 읽은 것으로 친다.
    struct toggle_notification_center_intent
    {};

    // 알림 센터를 닫는다. `dismissed`는 popup 바깥을 눌러 닫혔다는 뜻이다 (그 누름이 종이었으면 다시 열지 않는다).
    struct close_notification_center_intent
    {
        bool dismissed { false };
    };

    struct clear_notifications_intent
    {};

    // 알림 센터 머리에서 보일 종류를 고른다.
    struct filter_notifications_intent
    {
        notification_filter filter { notification_filter::all };
    };

    struct toggle_operation_log_intent
    {};

    enum class toolbar_action
    {
        fetch,
        pull,
        push,
        branch,
        stash,
        pop,
    };

    struct toolbar_intent
    {
        toolbar_action action { toolbar_action::fetch };
    };

    // 사이드바·상세 판의 폭을 끈 만큼 바꾼다 (논리 픽셀, +가 넓어지는 쪽).
    struct resize_pane_intent
    {
        bool sidebar { true };
        float delta { 0.0f };
    };

    struct toggle_drawer_intent
    {
        bool open { false };
    };

    struct open_rail_section_intent
    {
        // 같은 구역을 다시 누르면 닫는다.
        sidebar_section section { sidebar_section::local };
    };

    struct close_overlays_intent
    {};

    // ------------------------------------------------------------------ 시작 페이지

    struct open_recent_intent
    {
        std::u8string path {};
    };

    struct pin_recent_intent
    {
        std::u8string path {};
        bool pinned { false };
    };

    struct remove_recent_intent
    {
        std::u8string path {};
    };

    struct open_clone_dialog_intent
    {};

    struct close_clone_dialog_intent
    {};

    struct submit_clone_intent
    {};

    // ------------------------------------------------------------------ 저장소 화면

    enum class scroll_target
    {
        graph,
        sidebar,
        detail,
        diff,
        start_page,
        operation_log,
        // 커밋 상세의 메시지·메타 칸이다. 본문이 길면 파일 목록(`detail`)과 따로 흘린다.
        commit_meta,
        // 높이가 모자란 변경 파일·커밋 작성 영역 전체다. 파일 목록의 detail 스크롤과 구분한다.
        working_area,
        // 상태 줄의 종이 연 알림 센터의 목록이다.
        notifications,
    };

    // 흘리는 창의 스크롤이다. `absolute`면 `value`가 자리, 아니면 변화량이다 (논리 픽셀).
    struct scroll_intent
    {
        scroll_target target { scroll_target::graph };
        float value { 0.0f };
        bool absolute { false };
    };

    struct toggle_sidebar_section_intent
    {
        sidebar_section section { sidebar_section::local };
    };

    struct select_sidebar_item_intent
    {
        std::u8string key {};
        // 그 항목이 가리키는 커밋이다. 있으면 그래프에서도 그 행을 고른다.
        std::string target_sha {};
    };

    // 사이드바 항목이나 그래프 라벨을 두 번 눌렀다 (체크아웃).
    struct activate_ref_intent
    {
        ref_kind kind { ref_kind::local };
        std::string name {};
    };

    struct select_commit_intent
    {
        // 그래프 항목 키(SHA 앞 15자)다. 모델에서 SHA로 되찾는다.
        std::u8string key {};
        // Ctrl+클릭은 앞서 고른 커밋과 두 커밋 비교, Shift+클릭은 그 사이 범위다. 둘 다 거짓이면 커밋 하나를 고른다.
        bool control { false };
        bool shift { false };
    };

    // 그래프 행을 누른 채 끌고 있다. 누른 행이 기준이고 포인터 아래 행까지가 고른 범위다 (그 행을 Shift+클릭한 것과 같다).
    //  - `y`는 포인터의 창 좌표(물리 픽셀)다. 행은 controller가 그래프 목록의 자리·배율·스크롤로 되찾는다
    //    (`frame_feedback::graph_top`). 끄는 동안 목록이 스스로 흘러도 같은 좌표로 다시 온다.
    struct graph_sweep_intent
    {
        float y { 0.0f };
    };

    // 상세 패널의 파일을 골랐다. diff를 읽어 그래프 자리에 보인다.
    struct select_file_intent
    {
        diff_file_request request {};
        // 작업 영역 파일 행의 키다 (`u:경로`처럼 묶음 접두가 붙는다). 있으면 Ctrl+클릭은 하나 더하기·빼기, Shift+클릭은 기준 행부터의
        // 범위 고르기다. 비어 있으면 여러 파일 선택을 풀고 이 diff 하나를 연다.
        std::u8string key {};
        bool control { false };
        bool shift { false };
    };

    struct close_diff_intent
    {};

    struct set_repository_layout_intent
    {
        bool graph { true };
    };

    struct select_workspace_intent
    {
        bool working { false };
    };

    // 커밋 상세의 변경 파일을 경로 목록으로 볼지 폴더 트리로 볼지다. 앱 설정에 남는다.
    struct set_commit_files_view_intent
    {
        bool tree { false };
    };

    struct set_working_files_view_intent
    {
        bool tree { false };
    };

    struct commit_body_line_intent
    {
        bool down { false };
        bool extend { false };
    };

    // 변경 파일 트리의 폴더를 접거나 편다. `path`는 끝에 `/`가 없는 폴더 경로다.
    struct toggle_file_folder_intent
    {
        std::string path {};
    };

    // 커밋 상세 머리의 SHA 복사 단추다 (커밋 우클릭 메뉴의 "SHA 복사"와 같다).
    struct copy_commit_sha_intent
    {
        std::string sha {};
    };

    // worker가 작성자 사진을 받아 왔다 (Gravatar, `appearance.gravatar`가 켜졌을 때만 묻는다). `email`은 다듬은 메일이다.
    // 사진이 없으면 `image`가 비어 있고, `failed`면 받지 못한 것이라 잠시 뒤 다시 묻는다.
    struct avatar_loaded_intent
    {
        std::string email {};
        luil::ui_image image {};
        bool failed { false };
    };

    struct toggle_amend_intent
    {};

    struct commit_intent
    {};

    // ------------------------------------------------------------------ 텍스트 입력

    struct edit_text_intent
    {
        luil::text_edit_request request {};
    };

    struct compose_text_intent
    {
        luil::text_composition_event event {};
    };
} // namespace cyella
