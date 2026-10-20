#pragma once

#include "luil/ui/ui_element_id.h"

namespace cyella::ui {
    inline constexpr luil::ui_element_kind kind_segmented { luil::application_element_kind(600) };
    inline constexpr luil::ui_element_kind kind_segment { luil::application_element_kind(601) };
    inline constexpr luil::ui_element_kind kind_settings_section { luil::application_element_kind(602) };
    inline constexpr luil::ui_element_kind kind_theme_option { luil::application_element_kind(603) };
    inline constexpr luil::ui_element_kind kind_font_combo { luil::application_element_kind(604) };
    inline constexpr luil::ui_element_kind kind_ui_font_input { luil::application_element_kind(605) };
    inline constexpr luil::ui_element_kind kind_code_font_input { luil::application_element_kind(606) };
    // cyella element의 kind다. luil이 정하지 않은 앱 kind는 여기 한 곳에만 둔다. UI 자동화는 kind 숫자가 아니라
    // owner 이름으로 요소를 찾으므로(UI Automation), owner를 화면 안에서 유일하게 짓는다.
    inline constexpr luil::ui_element_kind kind_layout { luil::application_element_kind(0) };
    inline constexpr luil::ui_element_kind kind_label { luil::application_element_kind(1) };
    inline constexpr luil::ui_element_kind kind_button { luil::application_element_kind(2) };
    inline constexpr luil::ui_element_kind kind_text_button { luil::application_element_kind(3) };
    inline constexpr luil::ui_element_kind kind_tool_button { luil::application_element_kind(4) };
    // 텍스트 입력 칸은 kind마다 대상이 하나다 (`cyella_policy::text_target_of`).
    inline constexpr luil::ui_element_kind kind_start_search_input { luil::application_element_kind(5) };
    inline constexpr luil::ui_element_kind kind_sidebar_filter_input { luil::application_element_kind(6) };
    inline constexpr luil::ui_element_kind kind_commit_subject_input { luil::application_element_kind(7) };
    inline constexpr luil::ui_element_kind kind_commit_body_input { luil::application_element_kind(8) };
    inline constexpr luil::ui_element_kind kind_clone_url_input { luil::application_element_kind(9) };
    inline constexpr luil::ui_element_kind kind_clone_path_input { luil::application_element_kind(10) };
    inline constexpr luil::ui_element_kind kind_graph_row { luil::application_element_kind(11) };
    inline constexpr luil::ui_element_kind kind_sidebar_row { luil::application_element_kind(12) };
    inline constexpr luil::ui_element_kind kind_file_row { luil::application_element_kind(13) };
    inline constexpr luil::ui_element_kind kind_diff_row { luil::application_element_kind(14) };
    inline constexpr luil::ui_element_kind kind_log_row { luil::application_element_kind(15) };
    inline constexpr luil::ui_element_kind kind_start_row { luil::application_element_kind(16) };
    inline constexpr luil::ui_element_kind kind_start_card { luil::application_element_kind(17) };
    inline constexpr luil::ui_element_kind kind_banner { luil::application_element_kind(18) };
    inline constexpr luil::ui_element_kind kind_selector { luil::application_element_kind(19) };
    inline constexpr luil::ui_element_kind kind_splitter { luil::application_element_kind(20) };
    inline constexpr luil::ui_element_kind kind_panel { luil::application_element_kind(21) };
    inline constexpr luil::ui_element_kind kind_spinner { luil::application_element_kind(22) };
    inline constexpr luil::ui_element_kind kind_progress { luil::application_element_kind(23) };
    inline constexpr luil::ui_element_kind kind_graph_header { luil::application_element_kind(24) };
    inline constexpr luil::ui_element_kind kind_image { luil::application_element_kind(25) };
    // 환경 설정 화면의 입력 칸이다. 다른 화면(대화 상자)이 번호를 더해도 부딪치지 않게 40번부터 쓴다.
    inline constexpr luil::ui_element_kind kind_settings_git_path_input { luil::application_element_kind(40) };
    inline constexpr luil::ui_element_kind kind_settings_tool_extensions_input { luil::application_element_kind(41) };
    inline constexpr luil::ui_element_kind kind_settings_tool_executable_input { luil::application_element_kind(42) };
    inline constexpr luil::ui_element_kind kind_settings_tool_arguments_input { luil::application_element_kind(43) };
    inline constexpr luil::ui_element_kind kind_settings_key_name_input { luil::application_element_kind(44) };
    inline constexpr luil::ui_element_kind kind_settings_key_comment_input { luil::application_element_kind(45) };
    inline constexpr luil::ui_element_kind kind_settings_test_host_input { luil::application_element_kind(46) };
    // 입력 대화 상자의 칸 셋, 그래프 라벨 칩, 끄는 동안의 문구, 충돌 파일 행이다. 다른 화면이 번호를 더해도 겹치지 않게 100부터 쓴다.
    inline constexpr luil::ui_element_kind kind_form_input_0 { luil::application_element_kind(100) };
    inline constexpr luil::ui_element_kind kind_form_input_1 { luil::application_element_kind(101) };
    inline constexpr luil::ui_element_kind kind_form_input_2 { luil::application_element_kind(102) };
    inline constexpr luil::ui_element_kind kind_ref_chip { luil::application_element_kind(103) };
    inline constexpr luil::ui_element_kind kind_drag_hint { luil::application_element_kind(104) };
    inline constexpr luil::ui_element_kind kind_conflict_row { luil::application_element_kind(105) };
    inline constexpr luil::ui_element_kind kind_form_note { luil::application_element_kind(106) };
    // 인터랙티브 rebase 편집기의 메시지 칸(제목 한 줄, 본문 여러 줄)이다. 다른 칸과 겹치지 않게 200부터 쓴다.
    inline constexpr luil::ui_element_kind kind_rebase_subject_input { luil::application_element_kind(200) };
    inline constexpr luil::ui_element_kind kind_rebase_body_input { luil::application_element_kind(201) };
    // 툴바 저장소·브랜치 선택 상자 popup의 거르기 칸이다. 다른 칸과 겹치지 않게 110부터 쓴다.
    inline constexpr luil::ui_element_kind kind_selector_filter_input { luil::application_element_kind(110) };
    // 환경 설정 "호스팅" 화면의 호스트·API 주소 칸이다. 500부터 쓴다.
    inline constexpr luil::ui_element_kind kind_settings_hosting_host_input { luil::application_element_kind(500) };
    inline constexpr luil::ui_element_kind kind_settings_hosting_api_input { luil::application_element_kind(501) };
    // 명령 팔레트의 거르기 칸·목록 행, 환경 설정의 단축키 입력 칸과 사용자 프로필·규칙 입력 칸이다. 300부터 쓴다.
    inline constexpr luil::ui_element_kind kind_palette_filter_input { luil::application_element_kind(300) };
    inline constexpr luil::ui_element_kind kind_palette_row { luil::application_element_kind(301) };
    inline constexpr luil::ui_element_kind kind_shortcut_input { luil::application_element_kind(302) };
    inline constexpr luil::ui_element_kind kind_profile_name_input { luil::application_element_kind(303) };
    inline constexpr luil::ui_element_kind kind_profile_user_name_input { luil::application_element_kind(304) };
    inline constexpr luil::ui_element_kind kind_profile_email_input { luil::application_element_kind(305) };
    inline constexpr luil::ui_element_kind kind_profile_signing_key_input { luil::application_element_kind(306) };
    inline constexpr luil::ui_element_kind kind_profile_rule_prefix_input { luil::application_element_kind(307) };
    // 키 처리가 tree에서 읽는 보이지 않는 표식이다 (단축키 표, 팔레트 Enter).
    inline constexpr luil::ui_element_kind kind_command_marker { luil::application_element_kind(308) };
    // 환경 설정 "서명" 쪽의 프로그램 경로·allowed signers 입력 칸이다. 팔레트·단축키·프로필 칸의 300대와 겹치지 않게 400부터 쓴다.
    inline constexpr luil::ui_element_kind kind_settings_signing_gpg_input { luil::application_element_kind(400) };
    inline constexpr luil::ui_element_kind kind_settings_signing_ssh_input { luil::application_element_kind(401) };
    inline constexpr luil::ui_element_kind kind_settings_signing_allowed_input { luil::application_element_kind(402) };
} // namespace cyella::ui
