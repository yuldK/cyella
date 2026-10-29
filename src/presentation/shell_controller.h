#pragma once

#include "application/app_settings_service.h"
#include "application/diff_view_coordinator.h"
#include "application/file_probe.h"
#include "application/git_workspace.h"
#include "application/repository_tabs.h"
#include "domain/app_settings.h"
#include "domain/git_identity.h"
#include "presentation/dialog_intents.h"
#include "presentation/extension_intents.h"
#include "presentation/frame_feedback.h"
#include "presentation/graph_index.h"
#include "presentation/hosting_controller.h"
#include "presentation/rebase/rebase_editor_controller.h"
#include "presentation/selector_intents.h"
#include "presentation/settings/settings_controller.h"
#include "presentation/snapshot_dialogs.h"
#include "presentation/ui_intents.h"
#include "presentation/ui_state.h"
#include "presentation/view_snapshot.h"

#include "luil/ui/app_message.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cyella {
    // 셸의 화면 상태를 소유하고 화면 메시지를 처리한다. logic thread에서만 쓴다.
    //
    // 소유하는 것: 탭 목록(`repository_tabs`), 탭 → 저장소 id 표, `ui_state`, 그래프 모델 cache.
    // 빌리는 것: workspace(git 상태의 정본), 설정 service. 둘 다 없을 수 있다 — git을 찾지 못했으면
    // workspace가 없고, test나 설정 위치를 모르면 설정은 메모리에만 있다.
    //
    // driver는 메시지를 여기로 넘기고(`handle`), relay를 비운 뒤 `after_events`를 부르며, frame마다
    // `make_view`로 snapshot을 받는다. tree는 `ui::build_ui_tree`가 snapshot에서 짓는다.
    class shell_controller
    {
    public:
        struct dependencies
        {
            git_workspace* workspace { nullptr };
            app_settings_service* settings { nullptr };
            const file_probe* probe { nullptr };
            std::u8string git_message {};
            std::u8string git_version {};
            std::function<std::chrono::system_clock::time_point()> clock {};
            // 토스트의 떠오름·물러남·만료가 쓰는 시계다. 비어 있으면 `steady_clock::now`다 (test가 시각을 정한다).
            std::function<std::chrono::steady_clock::time_point()> steady_clock {};
            // 환경 설정 화면이 쓰는 값이다. 이번 실행이 찾은 git, ssh 도구의 위치, worker에게 시킬 일이다.
            git_tool_info git_tool {};
            std::function<void()> request_ssh_info {};
            // 환경 설정 "서명" 쪽이 worker에게 시킬 일이다. 결과는 signing_info_loaded_intent·signing_write_done_intent로 온다.
            std::function<void(const std::u8string& repository_path)> request_signing_info {};
            std::function<void(bool global, const std::u8string& repository_path, std::vector<signing_change> changes)> write_signing_config {};
            std::function<void(std::string text)> copy_text {};
            std::u8string ssh_directory {};
            std::u8string default_key_comment {};
            bool ssh_tools_found { false };
            std::u8string ssh_tools_origin {};
            // "이번만 연결"이 임시 known_hosts로 ssh를 부를 때 쓰는 ssh 실행 파일이다. 사용자의 `core.sshCommand`를 그 한 번의
            // 실행에서 덮어쓰므로 사용자가 쓰는 ssh여야 한다. 비면 git이 PATH에서 찾는 `ssh`다.
            std::u8string ssh_executable {};
            // 임시 known_hosts 파일을 둘 폴더다. 비우면 시스템 임시 폴더다 (test가 지정한다).
            std::u8string temporary_directory {};
            // 인증 대화 상자가 내는 백엔드 메시지(`answer_input_intent`·`cancel_input_intent`)를 처리하는 길이다. driver가
            // 자기 백엔드 처리기를 꽂는다. 비어 있으면(test) controller가 같은 일을 workspace에 바로 한다.
            std::function<void(const luil::app_message&)> send_backend_intent {};
            // GitHub·GitLab 호출을 워커에 맡기고, 메모리 토큰을 잊고, 브라우저로 주소를 여는 길이다. driver가 꽂는다. 비어 있으면(test)
            // 일감을 내는 동작은 아무 일도 하지 않는다.
            std::function<void(hosting_job)> submit_hosting_job {};
            std::function<void(const hosting_sign_out_request&)> hosting_sign_out {};
            std::function<void(const std::string& url)> open_url {};
            // 저장소의 작성자 신원을 worker에서 읽고 `git config --local`로 쓴다. 결과는 `identity_loaded_intent`·`identity_applied_intent`로 돌아온다.
            // 비어 있으면(test) 신원을 읽지 않고 프로필 적용도 하지 않는다.
            std::function<void(const std::string& repository_id, const std::u8string& path)> request_identity {};
            std::function<void(const std::string& repository_id, const std::u8string& path, const std::u8string& user_name, const std::u8string& user_email)> apply_identity {};
            // 작성자 사진(Gravatar)을 worker에서 받는다. 인자는 다듬은 메일이고 결과는 `avatar_loaded_intent`로 돌아온다.
            // 환경 설정 `appearance.gravatar`가 켜졌을 때만 부른다. 비어 있으면(test) 늘 이름 첫 글자다.
            std::function<void(const std::string& email)> request_avatar {};
            // 저장소 폴더를 탐색기·VS Code 같은 앱으로 연다. driver가 diff의 외부 앱 열기와 같은 worker 길을 꽂는다.
            // 비어 있으면(test) 아무 일도 하지 않는다.
            std::function<void(const external_file_app& app, const std::u8string& folder)> open_folder_app {};
        };

        explicit shell_controller(dependencies dependencies);

        // 설정의 `open_tabs`·`active_tab`으로 탭을 되살리고 저장소를 연다. `initial_repository`가 있으면
        // 그 저장소도 연다(명령줄 인자).
        void start(const std::u8string& initial_repository);

        // 저장소를 탭으로 연다. 이미 열린 저장소면 그 탭을 고른다. 최근 목록과 열린 탭을 설정에 남긴다.
        void open_repository(const std::u8string& path);

        // 화면 메시지면 처리하고 true다.
        bool handle(const luil::app_message& message);
        // relay를 비운 뒤다. 끝난 operation의 후속(열기·커밋 상자 비우기·clone 뒤 탭)을 처리한다.
        void after_events();

        [[nodiscard]] view_snapshot make_view(float width, float height, float scale);
        void apply_feedback(const frame_feedback& feedback);

        // 선택된 탭이 연 diff와 그 저장소다. 웹뷰가 보여야 할 대상이며, 열린 diff가 없으면 값이 없다.
        [[nodiscard]] std::optional<diff_target> current_diff_target() const;
        // diff 웹뷰의 지금 모습이다. 웹뷰를 쥔 driver가 frame마다 올려 머리 막대와 대체 보기가 읽는다.
        void set_diff_surface(const diff_surface_state& surface);

        // 판 폭을 끌 때마다 설정 파일을 쓰지 않도록, 끌기가 멈춘 뒤 한 번만 쓴다 (손잡이에는 놓았다는 사건이 없다).
        // driver가 `next_tick`·`tick`에 이어 주고, 앱을 닫을 때는 `flush`로 바로 쓴다.
        [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> next_tick() const noexcept;
        void tick(std::chrono::steady_clock::time_point now);
        void flush();

        [[nodiscard]] const repository_tabs& tabs() const noexcept
        {
            return tabs_;
        }

        [[nodiscard]] const ui_state& state() const noexcept
        {
            return ui_;
        }

        [[nodiscard]] const settings_controller& settings_screen() const noexcept
        {
            return settings_;
        }

        // 인터랙티브 rebase 편집기다.
        [[nodiscard]] const rebase_editor_controller& rebase_editor() const noexcept
        {
            return rebase_;
        }
        // GitHub·GitLab 화면의 상태다.
        [[nodiscard]] const hosting_controller& hosting() const noexcept
        {
            return hosting_;
        }
        // 메뉴가 복사하라고 한 글이다 (SHA·이름·경로). 한 번 꺼내면 비워진다. 클립보드는 UI thread의 일이라 driver가
        // 이 글을 가져가 넘긴다.
        [[nodiscard]] std::optional<std::u8string> take_clipboard_text();

        [[nodiscard]] const app_settings& settings() const noexcept;
        // 선택된 탭의 저장소 id다. 시작 탭이면 비어 있다.
        [[nodiscard]] std::string selected_repository_id() const;

    private:
        struct graph_cache_entry
        {
            std::uint64_t generation { 0 };
            std::size_t size { 0 };
            std::shared_ptr<const std::vector<luil::virtual_list_item>> items {};
        };

        void update_settings(const std::function<void(app_settings&)>& change);
        [[nodiscard]] settings_controller::dependencies settings_dependencies();
        void save_tabs();
        void close_tab(repository_tab_id id);
        void select_tab_index(std::size_t index);
        [[nodiscard]] tab_ui_state* selected_tab_state();
        [[nodiscard]] repository_session* selected_session();
        operation_id submit(git_operation operation);

        void handle_toolbar(toolbar_action action);
        void handle_scroll(const scroll_intent& intent);
        void handle_select_commit(const select_commit_intent& intent);
        // ---- 여러 커밋 비교 (shell_controller_compare.cpp)
        // Ctrl·Shift+클릭으로 비교를 시작하거나 마지막 커밋을 바꾼다. 비교할 기준이 없으면 false이고 그냥 클릭으로 다룬다.
        bool select_compare(tab_ui_state& tab, repository_session& session, const commit_info& clicked, bool shift);
        // 비교의 종류(범위인가)를 정하고 변경 파일 요청을 낸다. 조상 관계를 모르면 git에게 묻고 정하지 않은 채 둔다.
        void decide_compare(tab_ui_state& tab, repository_session& session);
        // 비교를 풀고 마지막에 누른 커밋 하나로 돌아간다.
        void clear_compare(tab_ui_state& tab);
        // 그래프 행을 끄는 동안 포인터 아래 행까지를 고른다 (그 행의 Shift+클릭).
        void handle_graph_sweep(const graph_sweep_intent& intent);
        // relay를 비운 뒤다. 조상 답을 기다리던 비교를 정하고, 새로 고침으로 고른 커밋이 사라진 비교를 푼다.
        void sync_compares();
        // ---- 작업 영역의 여러 파일 고르기 (shell_controller_working.cpp)
        // 파일 행을 눌렀다. 키가 있으면 Ctrl·Shift로 여러 파일을 고르고, 없으면 선택을 풀고 그 diff 하나를 연다.
        void handle_select_file(const select_file_intent& intent);
        // 그 파일의 diff를 그래프 자리에 연다.
        void open_file_diff(tab_ui_state& tab, diff_file_request request);
        // 우클릭한 작업 영역 파일이 여러 파일 선택 안에 있으면 그 선택의 경로들이다 (고른 순서). 아니면 비어 있다.
        [[nodiscard]] std::vector<std::string> working_menu_paths(const tab_ui_state& tab, const menu_target& target) const;
        // 메뉴의 "버리기"가 낼 확인 요청이다. 여러 파일이면 작업 영역에서 추적하지 않는 파일(휴지통)과 추적 중인 파일(restore)로 가른다.
        [[nodiscard]] discard_files_intent discard_intent_for(const std::vector<std::string>& paths, const menu_target& target);
        void handle_activate_ref(const activate_ref_intent& intent);
        void handle_commit();
        void handle_folder(const folder_chosen_intent& intent);

        // ---- 대화 상자·메뉴·끌어 놓기·충돌 해결 (shell_controller_dialogs.cpp)
        bool handle_dialog_message(const luil::app_message& message);
        [[nodiscard]] std::shared_ptr<const graph_index> history_index(const repository_session& session);
        // 선택된 저장소와 저장소 없는(clone·init) operation이다.
        void collect_operations(std::vector<const operation_status*>& repository, std::vector<const operation_status*>& setup);
        [[nodiscard]] form_source current_form_source();
        // 떠 있는 상자가 바뀌었으면 입력 초안을 지우고 비운다. 비밀이 다음 상자로 새지 않게 한다.
        void sync_form_inputs();
        void clear_form_inputs();
        void set_form_field(std::size_t index, std::u8string text);
        operation_id submit_to(const std::string& repository_id, git_operation operation);
        // 첫 operation을 내고, 앞이 성공하면 이어서 낼 것들을 맡긴다.
        operation_id submit_sequence(std::vector<git_operation> operations);
        void process_chains();
        void cancel_automatic_prompts();

        // ---- 툴바 저장소·브랜치 선택 상자 (shell_controller_selectors.cpp)
        bool handle_selector_message(const luil::app_message& message);
        void open_selector(const open_selector_intent& intent);
        // `dismissed`가 참이면 바깥 누름 등으로 닫힌 것이다 (곧 이어지는 같은 상자의 뗌을 다시 열기로 읽지 않는다).
        void close_selector(bool dismissed);
        void select_from_selector(const std::u8string& key);
        // 지금 탭의 저장소 폴더를 `repository_open_apps`의 `index`번째 앱으로 연다. 저장소 탭이 아니면 아무 일도 하지 않는다.
        // 원격의 웹 페이지를 브라우저로 연다 ("열기"의 링크, 사이드바 원격 행의 주소 링크). 링크 규칙이 없으면 아무것도 하지 않는다.
        void open_remote_web_link(const std::string& remote_name);
        void open_repository_in_app(std::size_t index);
        // 폴더를 `repository_open_apps`의 `index`번째 앱으로 연다. 탭 메뉴는 고르지 않은 탭의 폴더도 연다.
        void open_folder_in_app(std::size_t index, const std::u8string& folder);

        void open_context_menu(menu_target target, float x, float y);
        void request_drop_facts();
        void handle_menu_select(const std::u8string& key);
        void run_menu_action(const menu_target& target, const std::u8string& key, const menu_context& context);
        // 탭 우클릭 메뉴의 항목을 실행한다 (닫기 묶음, 경로 복사, 다른 앱으로 열기).
        void run_tab_menu_action(const menu_target& target, const std::u8string& key);
        void handle_drop(const drop_intent& intent);
        void handle_activate_commit(const std::string& sha);
        void open_menu_for_selection();
        void copy_text(std::u8string text);

        void open_create_branch_dialog(const std::string& at);
        void open_create_tag_dialog(const std::string& sha);
        void open_push_dialog(const std::string& branch, bool set_upstream);
        void open_rename_dialog(const std::string& branch);
        void open_set_upstream_dialog(const std::string& branch);
        // 원격 추가 상자다. branch가 있으면 추가한 뒤 그 브랜치를 게시(`push --set-upstream`)하는 체크 칸이 붙는다.
        void open_add_remote_dialog(const std::string& branch);
        // 원격 추가 상자에서 호스트를 골랐을 때의 [만들고 게시]다.
        void submit_publish(const dialog_state& dialog);
        void open_confirm(dialog_state dialog);
        void request_detached_checkout(const std::string& revision, std::u8string label);
        void request_rebase(const std::string& branch, const std::string& onto);
        // 인터랙티브 rebase 편집기를 연다 (presentation/rebase/shell_controller_rebase.cpp). branch가 비면 HEAD다.
        void open_rebase_editor(const std::string& onto, const std::string& branch, std::u8string onto_label, std::u8string branch_label);
        // 편집기를 여는 화면 메시지(커밋 상세 판의 단추)면 처리하고 true다.
        bool handle_rebase_entry(const luil::app_message& message);

        void handle_form_button(const std::u8string& key);
        void handle_form_check(std::size_t index);
        void handle_form_choose(std::size_t index);
        void handle_form_enter();
        void handle_dialog_button(const std::u8string& key);
        void handle_dirty_button(const operation_status& status, const std::u8string& key);
        void handle_auth_button(const operation_status& status, const std::u8string& key);
        void trust_host_once(const operation_status& status);
        void send_backend_intent(const luil::app_message& message);
        void handle_notification_action(const notification_action_intent& intent);
        void handle_conflict_action(conflict_action action);

        // ---- submodule·worktree·Git LFS (shell_controller_extension.cpp)
        bool handle_extension_message(const luil::app_message& message);
        void activate_extension_row(const activate_sidebar_item_intent& intent);
        void open_submodule(const std::string& path);
        void open_worktree(const std::string& path);
        // mode는 `worktree_dialog_mode`의 값이다. subject는 시작 위치(커밋 SHA, 비면 HEAD), branch는 칸에 미리 채울 이름이다.
        void open_add_worktree_dialog(const std::string& subject, std::size_t mode, const std::string& branch);
        void open_lfs_track_dialog(const std::string& pattern);
        void run_extension_menu_action(const menu_target& target, const std::u8string& key, const menu_context& context);

        // ---- 커밋 서명 (shell_controller_signing.cpp)
        bool handle_signing_message(const luil::app_message& message);
        // 아직 확인하지 않은 커밋의 서명을 읽기 lane에 요청한다 (이미 요청했거나 읽은 커밋은 workspace가 뺀다).
        void request_signatures(const std::vector<std::string>& shas);
        // 그래프에 보이는 행 둘레의 서명을 요청한다. 스크롤과 새 로그 묶음 뒤에 부른다.
        void request_visible_signatures();
        // 지금 선택한 탭의 저장소 폴더다. 저장소 탭이 아니면 비어 있다.
        [[nodiscard]] std::u8string selected_repository_path() const;
        // 서명 설정이 바뀌었다: 읽어 둔 서명을 버리고 refresh로 설정의 실효 값(커밋 상자의 서명 기본값)을 다시 읽는다. 전역이면 열린 모든 저장소다.
        void on_signing_changed(bool global);
        // 커밋 상자의 서명 토글이 지금 켜져 있는가 (사용자가 바꾼 값, 없으면 `commit.gpgsign`).
        [[nodiscard]] bool commit_sign_enabled(const tab_ui_state& tab, const repository_snapshot& snapshot) const;
        // 커밋 요청에 실을 `sign` 값이다: 켜면 true, 설정이 켜져 있는데 껐으면 false, 설정을 따르면 값 없음.
        [[nodiscard]] std::optional<bool> commit_sign_request(const tab_ui_state& tab, const repository_snapshot& snapshot) const;
        // ---- GitHub·GitLab (shell_controller_hosting.cpp)
        [[nodiscard]] hosting_controller::dependencies hosting_dependencies();
        [[nodiscard]] std::optional<hosting_target> hosting_target_of(const std::string& repository_id) const;
        void open_create_pull_request_dialog();
        void submit_create_pull_request(const dialog_state& dialog);
        [[nodiscard]] operation_id submit_git_sequence(const std::string& repository_id, std::vector<git_operation> operations);
        // ---- 명령 팔레트·단축키 (presentation/palette/shell_controller_palette.cpp)
        bool handle_command_message(const luil::app_message& message);
        // 레지스트리의 명령 하나를 실행한다 (단축키·팔레트·툴바가 같은 길이다). 지금 할 수 없는 명령은 아무 일도 하지 않는다.
        void run_command(std::string_view id);
        void open_palette(palette_mode mode);
        void close_palette();
        void pick_palette_item(const std::u8string& key);
        void move_palette_cursor(int delta);
        [[nodiscard]] command_availability current_availability();
        // 고른 사이드바 항목이나 커밋의 우클릭 메뉴 대상이다 (Shift+F10과 팔레트가 같은 대상을 쓴다).
        [[nodiscard]] std::optional<menu_target> selection_menu_target();
        [[nodiscard]] palette_context make_palette_context(palette_mode mode);
        [[nodiscard]] std::optional<palette_view> make_palette_view();
        // 거르는 글이다 (IME 조합 중인 글을 포함한다).
        [[nodiscard]] std::u8string palette_query() const;

        // ---- 저장소별 사용자 프로필 (presentation/settings/shell_controller_profiles.cpp)
        bool handle_profile_message(const luil::app_message& message);
        // 선택된 저장소의 신원을 아직 읽지 않았으면 worker에게 읽게 한다. 규칙에 맞는 프로필이 있으면 한 번 제안한다.
        void sync_identity();
        void apply_profile(const std::u8string& name);
        // 커밋 전 경고다. 신원이 없거나 규칙의 프로필과 다르면 안내를 띄우고 true(커밋하지 않는다)다. 저장소마다 한 번만 경고한다.
        [[nodiscard]] bool warn_before_commit();
        // 안내 알림의 프로필 조치다. 처리했으면 true다.
        bool handle_profile_notification_action(const std::u8string& key);
        void commit_now();
        // 프로필 모드 팔레트와 상태 줄이 쓰는 값이다. 선택된 저장소의 신원을 읽었는가와 그 신원이다.
        [[nodiscard]] const git_identity* selected_identity() const;
        [[nodiscard]] std::u8string suggested_profile_name() const;
        [[nodiscard]] std::u8string applied_profile_name() const;

        [[nodiscard]] std::u8string commit_label(const repository_session& session, const std::string& sha) const;
        void handle_text_edit(const luil::text_edit_request& request);
        void maybe_load_more();
        void show_notice(std::u8string text, bool warning = false);
        // 알림을 띄운다 (토스트로 뜨고 알림 센터에 남는다). id·시각·토스트 시간은 여기서 붙인다.
        void notify(notification value);
        // 새로 실패한 사용자 작업을 알리고, 같은 저장소의 뒤 작업이 성공했으면 앞 실패의 토스트를 내린다.
        void notify_failures();
        // 떠 있는 토스트를 물러나게 한다. `remove`면 물러남이 끝난 뒤 알림 센터에서도 지운다.
        void leave_toast(notification& value, std::chrono::steady_clock::time_point now, bool remove = false);
        // 그 저장소를 연 탭을 고른다. 열린 탭이 없으면 false다.
        bool select_repository_tab(const std::string& repository_id);
        [[nodiscard]] luil::text::text_edit_state* draft(text_target target);
        [[nodiscard]] std::shared_ptr<const std::vector<luil::virtual_list_item>> graph_items(const repository_session& session);
        [[nodiscard]] bool repository_exists(std::u8string_view path);

        dependencies dependencies_ {};
        app_settings memory_settings_ {};
        repository_tabs tabs_ {};
        std::map<repository_tab_id, std::string> tab_repositories_ {};
        ui_state ui_ {};
        frame_feedback feedback_ {};
        float last_logical_width_ { 0.0f };
        // 마지막 frame의 배치다. 커밋을 고를 때 서랍을 열지 정한다.
        layout_mode last_mode_ { layout_mode::wide };
        std::map<std::string, graph_cache_entry> graph_cache_ {};
        std::map<std::u8string, bool> exists_cache_ {};
        // 끝나기를 기다리는 저장소 준비 작업이다. 끝나면 결과로 탭을 연다.
        std::map<operation_id, std::u8string> pending_inspections_ {};
        std::map<operation_id, std::u8string> pending_setup_ {};
        // 없어진 최근 저장소의 새 위치를 확인하는 중인 경로 검사다. 옛 경로와 고른 폴더를 든다.
        struct locate_request
        {
            std::u8string origin {};
            std::u8string chosen {};
        };
        std::map<operation_id, locate_request> pending_locates_ {};
        // 바깥 누름으로 마지막에 닫힌 선택 상자와 그 시각이다.
        std::optional<std::pair<selector_kind, std::chrono::steady_clock::time_point>> selector_dismissed_ {};
        settings_controller settings_;
        rebase_editor_controller rebase_;
        hosting_controller hosting_;
        // 판 폭을 설정에 쓸 시각이다. 폭을 끄는 동안 뒤로 밀리고, 값이 없으면 쓸 것이 없다.
        std::optional<std::chrono::steady_clock::time_point> pane_widths_flush_at_ {};
        // 이미 알린 실패 operation이다. 같은 실패를 두 번 띄우지 않는다.
        std::set<operation_id> notified_failures_ {};
        // 알림 센터가 바깥 누름으로 닫힌 시각이다.
        std::optional<std::chrono::steady_clock::time_point> notifications_dismissed_at_ {};

        struct history_cache_entry
        {
            std::uint64_t generation { 0 };
            std::size_t size { 0 };
            std::shared_ptr<const graph_index> index {};
        };

        // 앞 operation이 성공해야 이어서 낼 operation들이다 (원격 태그를 먼저 지우고 로컬 태그를 지운다, pull 뒤 push).
        struct chain_entry
        {
            std::string repository_id {};
            std::vector<git_operation> rest {};
        };

        // "이번만 연결"로 다시 낸 operation이다. 성공하면 같은 임시 신뢰로 원격 태그 표시를 한 번 갱신한다.
        struct trusted_retry
        {
            std::string repository_id {};
            temporary_ssh_trust trust {};
        };

        std::map<std::string, history_cache_entry> history_cache_ {};
        std::map<operation_id, chain_entry> chains_ {};
        std::map<operation_id, trusted_retry> trusted_retries_ {};
        std::optional<std::u8string> clipboard_text_ {};
        std::uint64_t temporary_counter_ { 0 };
        float last_scale_ { 1.0f };

        // ---- 저장소별 작성자 신원. 저장소 id가 열쇠다.
        struct identity_entry
        {
            git_identity identity {};
            // worker에게 읽기를 청했다 / 읽었다 / 실패했다.
            bool requested { false };
            bool loaded { false };
            std::u8string error {};
            // 마지막으로 읽은 시각이다. 오래되면 다시 읽는다.
            std::chrono::steady_clock::time_point read_at {};
        };
        std::map<std::string, identity_entry> identities_ {};

        // ---- 커밋 작성자 사진(Gravatar). 다듬은 메일이 열쇠이고 앱을 켜 두는 동안만 둔다 (디스크 보관은 worker 쪽 몫이다).
        struct avatar_entry
        {
            luil::ui_image image {};
            // 답이 오기 전이다. 같은 메일을 거듭 묻지 않는다.
            bool pending { true };
            // 받지 못했으면 이 시각 뒤에 다시 묻는다.
            std::optional<std::chrono::steady_clock::time_point> retry_at {};
        };
        std::map<std::string, avatar_entry> avatars_ {};
        // 상세 패널의 작성자 사진을 채운다. 설정이 켜졌고 아직 묻지 않은 메일이면 worker에 청한다.
        void attach_avatar(commit_detail_view& commit);
        // 프로필을 이미 제안한 저장소, 커밋 전 경고를 이미 보인 저장소다 (앱을 켜 두는 동안만).
        std::set<std::string> profile_proposed_ {};
        std::set<std::string> commit_warned_ {};
        // 프로필을 적용하는 중인 저장소와 그 프로필 이름이다. 끝나면 적용 기록을 남긴다.
        std::map<std::string, std::u8string> applying_profiles_ {};
        // "프로필 적용 후 커밋"을 골랐다. 적용이 성공하면 커밋한다.
        std::set<std::string> commit_after_apply_ {};
    };
} // namespace cyella
