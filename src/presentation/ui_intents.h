#pragma once

#include "application/git_operation.h"
#include "application/repository_tabs.h"
#include "domain/ref.h"
#include "presentation/ui_state.h"

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
    };

    // ------------------------------------------------------------------ 셸

    // 테마를 system → light → dark 차례로 바꾸고 설정에 저장한다.
    struct cycle_theme_intent
    {};

    // 아직 없는 대화 상자 자리에 안내를 띄운다.
    struct show_notice_intent
    {
        std::u8string text {};
        bool warning { false };
    };

    struct dismiss_notice_intent
    {};

    struct toggle_operation_log_intent
    {};

    // 배너의 [자세히]를 펴거나 접는다.
    struct toggle_banner_details_intent
    {};

    struct dismiss_banner_intent
    {
        operation_id id { 0 };
    };

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
    };

    // 상세 패널의 파일을 골랐다. diff를 읽어 그래프 자리에 보인다.
    struct select_file_intent
    {
        diff_file_request request {};
    };

    struct close_diff_intent
    {};

    // 커밋 상세의 변경 파일을 경로 목록으로 볼지 폴더 트리로 볼지다. 앱 설정에 남는다.
    struct set_commit_files_view_intent
    {
        bool tree { false };
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
