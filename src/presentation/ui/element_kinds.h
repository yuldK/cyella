#pragma once

#include "luil/ui/ui_element_id.h"

namespace cyella::ui {
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
} // namespace cyella::ui
