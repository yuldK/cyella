#pragma once

#include "application/git_operation.h"
#include "application/repository_tabs.h"
#include "domain/app_settings.h"
#include "domain/commit_graph_layout.h"
#include "domain/commit_info.h"
#include "domain/diff_file.h"
#include "domain/file_change.h"
#include "domain/operation_status.h"
#include "domain/ref.h"
#include "presentation/graph_geometry.h"
#include "presentation/shell_layout.h"
#include "presentation/start_page_view.h"
#include "presentation/ui_state.h"

#include "luil/ui/text_input_state.h"
#include "luil/ui/virtual_list_element.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace cyella {
    // 한 frame의 화면 값이다. `build_view_snapshot`이 workspace·탭·설정·`ui_state`에서 짓는 순수 함수의
    // 결과이고, `ui::build_ui_tree`가 이 값만 읽어 luil tree를 짓는다.
    //
    // 예외는 그래프 행 하나다. 10만 행을 frame마다 복사하지 않으려고 세션의 행 벡터를 가리키는 포인터를
    // 싣는다(`graph_view::rows`). 포인터는 snapshot을 지은 그 `make_frame` 안에서만 유효하다 — tree의 행
    // element는 짓는 동안 자기 값을 복사해 가므로 게시된 tree가 세션을 붙잡지 않는다.

    // 탭 하나의 표시다. 우선순위는 입력 필요 > 진행 중 > 대기 > 변경 있음이다.
    enum class tab_marker
    {
        none,
        changes,
        queued,
        running,
        needs_input,
    };

    struct tab_view
    {
        repository_tab_id id { 0 };
        std::u8string key {};
        std::u8string label {};
        tab_marker marker { tab_marker::none };
        // rebase·merge 중이면 "rebase 중" 같은 꼬리표다.
        std::u8string in_progress {};
        std::u8string tooltip {};
        bool start { false };
    };

    struct toolbar_view
    {
        std::u8string repository_name {};
        std::u8string branch_text {};
        bool detached { false };
        std::uint32_t ahead { 0 };
        std::uint32_t behind { 0 };
        bool show_labels { true };
        bool show_repository_selector { true };
        // medium·narrow에서만 상세 서랍 단추가 선다.
        bool show_detail_toggle { false };
        bool drawer_open { false };
        bool fetch_enabled { false };
        bool pull_enabled { false };
        bool push_enabled { false };
        // upstream이 없어 push 대화 상자가 필요하다. 지금은 안내만 띄운다.
        bool push_needs_dialog { false };
        bool branch_enabled { false };
        bool stash_enabled { false };
        bool pop_enabled { false };
        // 툴바 오른쪽의 진행 중 작업 한 줄이다.
        std::u8string busy_text {};
    };

    struct active_operation_view
    {
        operation_id id { 0 };
        std::u8string text {};
        std::optional<double> fraction {};
        bool cancellable { false };
        bool queued { false };
    };

    struct status_view
    {
        std::optional<active_operation_view> active {};
        std::u8string last_result {};
        bool last_failed { false };
        // 마지막 작업이 입력을 기다리며 끝났다 (대화 상자가 필요하다).
        bool last_needs_input { false };
        std::size_t queued_count { 0 };
        bool repository { false };
        std::u8string branch_text {};
        std::uint32_t ahead { 0 };
        std::uint32_t behind { 0 };
        std::uint32_t changes { 0 };
        bool log_open { false };
    };

    enum class banner_kind
    {
        failure,
        // 대화 상자가 아직 없는 needs_input이다. 종류를 이름으로 밝힌다.
        needs_input,
        // 아직 없는 대화 상자의 안내다.
        notice,
    };

    struct banner_view
    {
        banner_kind kind { banner_kind::failure };
        operation_id id { 0 };
        std::u8string title {};
        std::u8string summary {};
        // [자세히]가 보일 줄이다. 실행한 명령줄(`commands[]`)과 git 원문, 입력 필요의 파일 목록이다.
        std::vector<std::u8string> details {};
        bool expanded { false };
        bool warning { false };
    };

    struct log_line_view
    {
        std::u8string key {};
        std::u8string text {};
        // operation 머리 줄이다. 아니면 그 operation이 실행한 명령줄이다.
        bool heading { false };
        bool failed { false };
        bool automatic { false };
    };

    struct start_page_snapshot
    {
        start_page_view list {};
        luil::text_input_view search {};
        // 탭으로 이미 열린 저장소의 경로 키다 (`repository_path_key`). 항목에 "열림" 배지를 단다.
        std::set<std::u8string> open_keys {};
        // git을 찾지 못했으면 그 안내다. 비어 있으면 git을 쓸 수 있다.
        std::u8string git_message {};
        std::u8string git_version {};
        float scroll { 0.0f };
        // 진행 중인 clone·init의 한 줄이다 (대화 상자를 닫아도 계속된다).
        std::u8string setup_progress {};
    };

    struct sidebar_row_view
    {
        std::u8string key {};
        bool header { false };
        // 원격 이름처럼 구역 안의 묶음 머리다. 고를 수는 있지만 체크아웃하지 않는다.
        bool group { false };
        sidebar_section section { sidebar_section::local };
        std::u8string label {};
        // 원격 묶음 머리의 URL처럼 흐리게 붙는 글이다.
        std::u8string detail {};
        // 머리 줄의 항목 수다.
        std::uint32_t count { 0 };
        bool collapsed { false };
        ref_kind kind { ref_kind::local };
        std::string ref_name {};
        std::string target_sha {};
        bool current { false };
        std::uint32_t ahead { 0 };
        std::uint32_t behind { 0 };
        // 로컬 브랜치에 upstream이 없다. 구름 아이콘을 단다.
        bool no_upstream { false };
        bool upstream_gone { false };
        // 원격이 있는데 어느 원격에도 없는 태그다.
        bool local_only { false };
        // 원격 묶음 안의 브랜치는 한 단 들여 쓴다.
        int depth { 0 };
        std::u8string tooltip {};
    };

    struct graph_view
    {
        // 세션의 행과 커밋이다 (같은 순서). 위의 포인터 규칙을 따른다.
        const std::vector<graph_row>* rows { nullptr };
        const std::vector<commit_info>* commits { nullptr };
        // `virtual_list_config::items`에 그대로 옮길 모델이다. 세션 revision이 같으면 같은 벡터를 다시 쓴다.
        std::shared_ptr<const std::vector<luil::virtual_list_item>> items {};
        std::shared_ptr<const ref_label_map> labels {};
        std::u8string selected_key {};
        float scroll { 0.0f };
        std::uint16_t lane_count { 0 };
        bool has_wip { false };
        std::uint32_t wip_count { 0 };
        // HEAD 커밋의 행 번호다. WIP 점선이 여기서 끝난다. 모르면 0이다.
        std::size_t head_row_index { 0 };
        std::string head_sha {};
        bool loading { false };
        bool history_complete { false };
        // narrow에서는 작성자·날짜 열을 감추고 라벨 열을 줄인다.
        bool compact_columns { false };
        // 날짜 열의 "N분 전"을 셀 기준 시각이다 (유닉스 초).
        std::int64_t now { 0 };
    };

    struct file_row_view
    {
        std::u8string key {};
        std::string path {};
        std::string original_path {};
        std::u8string name {};
        std::u8string folder {};
        char status { 'M' };
        change_side side { change_side::unstaged };
        bool untracked { false };
        bool conflicted { false };
        std::u8string added {};
        std::u8string removed {};
        bool selected { false };
    };

    struct commit_detail_view
    {
        std::string sha {};
        std::u8string short_sha {};
        std::u8string subject {};
        std::u8string body {};
        std::u8string author {};
        std::u8string date {};
        std::vector<std::u8string> parents {};
        std::vector<ref_label> labels {};
        // 그래프에서 그 커밋 행의 레인 색 번호다. 라벨 칩이 그래프와 같은 색을 쓴다.
        std::uint8_t lane_color { 0 };
        std::vector<file_row_view> files {};
        // 고른 커밋의 `show_commit` 결과가 아직 오지 않았다.
        bool loading { false };
    };

    struct working_area_view
    {
        std::u8string branch {};
        std::vector<file_row_view> conflicts {};
        std::vector<file_row_view> unstaged {};
        std::vector<file_row_view> staged {};
        luil::text_input_view subject {};
        luil::text_input_view body {};
        std::size_t subject_length { 0 };
        bool amend { false };
        std::u8string commit_label {};
        bool can_commit { false };
        // 커밋 단추가 꺼진 이유다. 단추 아래에 글로 보인다.
        std::u8string blocked_reason {};
        bool committing { false };
    };

    struct diff_line_view
    {
        diff_line_kind kind { diff_line_kind::context };
        // hunk 머리 줄(`@@ ... @@`)이다.
        bool hunk_header { false };
        std::int32_t old_line { 0 };
        std::int32_t new_line { 0 };
        std::u8string text {};
    };

    // diff 자리의 임시 본문이다. WebView2 diff가 붙으면 이 자리를 대신한다.
    struct diff_panel_view
    {
        diff_file_request request {};
        std::u8string title {};
        std::u8string subtitle {};
        std::vector<diff_line_view> lines {};
        bool loading { false };
        bool binary { false };
        float scroll { 0.0f };
    };

    struct repository_view
    {
        repository_tab_id tab { 0 };
        std::string repository_id {};
        std::u8string display_name {};
        bool loaded { false };
        // 첫 refresh가 실패했으면 그 문구다.
        std::u8string load_error {};
        std::vector<sidebar_row_view> sidebar {};
        std::array<std::uint32_t, sidebar_section_count> section_counts {};
        luil::text_input_view sidebar_filter {};
        std::u8string selected_sidebar_key {};
        float sidebar_scroll { 0.0f };
        graph_view graph {};
        detail_mode detail { detail_mode::none };
        std::optional<commit_detail_view> commit {};
        std::optional<working_area_view> working {};
        float detail_scroll { 0.0f };
        std::optional<diff_panel_view> diff {};
        std::optional<sidebar_section> rail_section {};
    };

    struct clone_dialog_view
    {
        luil::text_input_view url {};
        luil::text_input_view path {};
        bool running { false };
        std::u8string phase {};
        std::optional<double> fraction {};
        operation_id operation { 0 };
        std::u8string error {};
        bool can_submit { false };
    };

    struct view_snapshot
    {
        // 창 크기는 물리 픽셀, `logical_*`는 배율로 나눈 논리 픽셀이다. 배치 경계는 논리 값으로 판정한다.
        float width { 0.0f };
        float height { 0.0f };
        float scale { 1.0f };
        float logical_width { 0.0f };
        float logical_height { 0.0f };
        // "N분 전"을 셀 기준 시각이다 (유닉스 초). 시계를 snapshot 밖에서 읽어 builder가 순수하게 남는다.
        std::int64_t now { 0 };
        layout_mode mode { layout_mode::wide };
        theme_preference theme { theme_preference::system };
        std::vector<tab_view> tabs {};
        repository_tab_id selected_tab { 0 };
        float tab_scroll { 0.0f };
        std::optional<start_page_snapshot> start {};
        std::optional<repository_view> repository {};
        toolbar_view toolbar {};
        status_view status {};
        std::optional<banner_view> banner {};
        bool log_open { false };
        std::vector<log_line_view> log {};
        float log_scroll { 0.0f };
        std::optional<clone_dialog_view> clone {};
        float sidebar_width { default_sidebar_width };
        float detail_width { default_detail_width };
        bool drawer_open { false };
    };
} // namespace cyella
