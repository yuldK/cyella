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
#include "domain/submodule.h"
#include "presentation/commands/command_registry.h"
#include "presentation/drop_hints.h"
#include "presentation/file_tree.h"
#include "presentation/form_view.h"
#include "presentation/graph_geometry.h"
#include "presentation/hosting_view.h"
#include "presentation/palette/palette_view.h"
#include "presentation/rebase/rebase_editor_view.h"
#include "presentation/settings/settings_view.h"
#include "presentation/shell_layout.h"
#include "presentation/signing_view.h"
#include "presentation/start_page_view.h"
#include "presentation/ui_state.h"

#include "luil/ui/image_element.h"
#include "luil/ui/text_input_state.h"
#include "luil/ui/virtual_list_element.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
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
        // upstream이 없어 push 대화 상자에서 원격을 골라야 한다.
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
        // 선택된 저장소의 작성자 신원이다. 저장소 화면에서만 보인다. 누르면 프로필을 고르는 팔레트가 열린다.
        bool identity_visible { false };
        std::u8string identity_text {};
        std::u8string identity_tooltip {};
        // 커밋하려면 손봐야 한다 (이름이나 이메일이 없다).
        bool identity_warning { false };
    };

    enum class banner_kind
    {
        failure,
        // (쓰지 않는다) 입력이 필요한 operation은 배너가 아니라 모달 입력 상자(`view_snapshot::form`)와 충돌 화면이 맡는다.
        needs_input,
        // 아직 없는 대화 상자의 안내다.
        notice,
    };

    // 배너의 조치 단추다 ("Pull 후 다시 push"). 눌리면 `banner_action_intent`가 키와 operation을 나른다.
    struct banner_action_view
    {
        std::u8string key {};
        std::u8string label {};
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
        std::vector<banner_action_view> actions {};
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

    // 사이드바 행이 가리키는 것이다. 브랜치·태그·stash(`ref`)가 기본이고, 나머지는 submodule·worktree·LFS·PR 구역이다.
    enum class sidebar_item_kind : std::uint8_t
    {
        ref,
        submodule,
        worktree,
        lfs,
        // 사이드바 PULL REQUESTS 구역의 행(PR·MR과 동작 행)이다. `path`가 동작 열쇠다.
        pull_request,
    };

    // 행 오른쪽 꼬리 글의 색 느낌이다.
    enum class badge_tone : std::uint8_t
    {
        none,
        accent,
        warning,
        danger,
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
        // stash 행의 `stash@{N}`에서 N이다.
        std::uint32_t stash_index { 0 };
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
        // ---- submodule·worktree·LFS
        sidebar_item_kind item { sidebar_item_kind::ref };
        // submodule: 저장소 루트 기준 경로, worktree: 폴더(`/` 구분 그대로), lfs: 파일 경로다.
        std::string path {};
        submodule_state submodule { submodule_state::initialized };
        bool worktree_main { false };
        bool worktree_current { false };
        bool worktree_locked { false };
        bool worktree_prunable { false };
        // 오른쪽에 붙는 꼬리 글이다 ("미초기화", "잠김", "받음").
        std::u8string badge {};
        badge_tone tone { badge_tone::none };
        // 흐리게 그린다 (초기화하지 않은 submodule, 사라진 worktree).
        bool dimmed { false };
        // 로컬 브랜치가 **다른** worktree에 체크아웃되어 있으면 그 폴더다.
        std::string worktree_path {};
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
        // 세션이 읽어 둔 커밋 서명이다. 행에 서명 표지를 단다. 위의 포인터 규칙을 따른다.
        const std::map<std::string, commit_signature>* signatures { nullptr };
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
        // Git LFS가 추적하는 경로다. 행에 "LFS" 배지가 붙는다.
        bool lfs { false };
        // submodule 포인터(gitlink) 변경이다. 텍스트 변경이 아니라 특별한 행으로 그린다.
        bool submodule { false };
    };

    struct commit_detail_view
    {
        std::string sha {};
        std::u8string short_sha {};
        std::u8string subject {};
        std::u8string body {};
        std::u8string author {};
        // 상세 패널은 이름과 메일을 따로 그린다 (`author`는 둘을 이은 한 줄이다).
        std::u8string author_name {};
        std::u8string author_email {};
        // 작성자 사진이다 (Gravatar를 켰고 받아 왔을 때만). 비어 있으면 이름 첫 글자 동그라미다.
        luil::ui_image author_avatar {};
        // `date`는 상대 시각("3일 전"), `date_absolute`는 이 컴퓨터 시간대의 "2026-10-20 14:03"이다.
        std::u8string date {};
        std::u8string date_absolute {};
        std::vector<std::u8string> parents {};
        // `parents`와 같은 순서의 전체 SHA다. 부모를 누르면 그 커밋을 고른다.
        std::vector<std::string> parent_shas {};
        std::vector<ref_label> labels {};
        // 로그인한 호스트의 CI 상태다. 로그인하지 않았거나 서비스를 모르면 값이 없다.
        std::optional<ci_mark_view> ci {};
        // 그래프에서 그 커밋 행의 레인 색 번호다. 라벨 칩이 그래프와 같은 색을 쓴다.
        std::uint8_t lane_color { 0 };
        std::vector<file_row_view> files {};
        // 이 커밋 위로 다시 쌓을 현재 브랜치(HEAD)의 커밋 수다 (`rev-list --count 커밋..HEAD`). HEAD 자신이거나 이력을 덜 읽었으면 값이 없다.
        std::optional<std::uint32_t> commits_above {};
        // HEAD의 조상이다 (현재 브랜치 위의 커밋). 아니면 현재 브랜치를 이 커밋 위로 옮겨 쌓는 편집이다.
        bool on_current_branch { false };
        // 변경 파일 머리의 줄 수 합계다 ("+12", "-3"). 없으면 비어 있다.
        std::u8string added_total {};
        std::u8string removed_total {};
        // 변경 파일을 폴더 트리로 본다 (앱 설정). 그때만 `tree`가 차고, 파일 행은 `files`의 번호로 가리킨다.
        bool files_tree { false };
        std::vector<file_tree_row> tree {};
        // 고른 커밋의 `show_commit` 결과가 아직 오지 않았다.
        bool loading { false };
        // 서명 배지다. 서명 검증은 파일 목록과 따로 읽으므로 `loading`과 무관하게 먼저 보일 수 있다.
        signature_view signature {};
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
        // amend를 켰고 HEAD 커밋이 이미 upstream에 있다. 고쳐 쓰면 강제 push가 필요하다.
        bool amend_warning { false };
        // 커밋 상자의 "서명" 토글이다. 켜져 있으면 `commit -S`다. 기본값은 설정 `commit.gpgsign`이다.
        bool sign { false };
        // 서명이 켜져 있는데 서명할 수 없는 설정이다 (ssh 서명인데 키가 없다). 단추 아래 한 줄로 알린다.
        std::u8string sign_hint {};
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

    // 텍스트 diff로 보일 수 없는 변경의 카드다. LFS 포인터와 submodule 포인터가 이 모양으로 그려진다.
    enum class diff_special_kind : std::uint8_t
    {
        lfs_pointer,
        submodule_pointer,
    };

    struct diff_special_row
    {
        std::u8string label {};
        std::u8string value {};
    };

    struct diff_special_view
    {
        diff_special_kind kind { diff_special_kind::lfs_pointer };
        // 카드 머리 글이다 ("LFS 포인터").
        std::u8string title {};
        std::vector<diff_special_row> rows {};
        std::u8string note {};
        // 카드 아래 단추다. 비면 단추가 없다.
        std::u8string action_label {};
        std::optional<git_operation> action {};
        // submodule 열기 단추의 경로다 (카드가 submodule 포인터일 때). 비면 단추가 없다.
        std::string open_path {};
    };

    // diff 자리의 머리 막대 값과, 웹뷰를 쓸 수 없을 때의 텍스트 대체 본문이다(`docs/ui-architecture.md`).
    struct diff_panel_view
    {
        diff_file_request request {};
        std::u8string title {};
        std::u8string subtitle {};
        std::vector<diff_line_view> lines {};
        bool loading { false };
        bool binary { false };
        float scroll { 0.0f };
        // 웹뷰가 본문을 그린다. 참이면 `lines`를 채우지 않는다 (큰 diff를 frame마다 복사하지 않으려는 것이다).
        bool use_webview { false };
        diff_surface_state surface {};
        // 텍스트 diff로 보일 수 없는 변경이면 본문 대신 이 카드를 그린다 (웹뷰 여부와 무관하다).
        std::optional<diff_special_view> special {};
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
        // 커밋 상세의 메시지·메타 칸의 스크롤이다.
        float commit_meta_scroll { 0.0f };
        std::optional<diff_panel_view> diff {};
        // 열린 PR·MR 상세(`detail == detail_mode::pull_request`)와 호스팅 값이다.
        std::optional<hosting_repository_view> hosting {};
        std::optional<sidebar_section> rail_section {};
        // rebase·merge·cherry-pick·revert가 멈춰 있으면 그래프 자리를 덮는 충돌 해결 화면이다.
        std::optional<conflict_view> conflict {};
        float conflict_scroll { 0.0f };
        // 끄는 동안 대상 위에서 보일 문구의 근거다.
        std::shared_ptr<const drop_hint_data> drop_hints {};
        // 커밋 상세·그래프 행이 amend 경고 등에 쓰는 값이다.
        bool head_pushed { false };
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
        // 모달 입력 상자다 (브랜치·태그·push·확인·체크아웃 정책·인증). 있으면 Clone 상자보다 위에 선다.
        std::optional<form_view> form {};
        std::optional<menu_view> menu {};
        // 열린 저장소·브랜치 선택 상자 목록이다.
        std::optional<selector_view> selector {};
        float sidebar_width { default_sidebar_width };
        float detail_width { default_detail_width };
        bool drawer_open { false };
        // 환경 설정 대화 상자다. 열려 있을 때만 값이 있다.
        std::optional<settings_view> settings {};
        // 인터랙티브 rebase 편집기다. 열려 있을 때만 값이 있다.
        std::optional<rebase_editor_view> rebase_editor {};
        // ---- 명령 팔레트·단축키
        // 명령 팔레트다. 열려 있을 때만 값이 있다.
        std::optional<palette_view> palette {};
        // 키 처리가 읽는 단축키 표다 (기본값 + 사용자가 바꾼 값). 입력 thread의 정책은 상태를 두지 않으므로 tree에 실린다.
        std::vector<shortcut_binding> shortcuts {};
    };
} // namespace cyella
