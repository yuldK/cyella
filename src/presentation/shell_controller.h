#pragma once

#include "application/app_settings_service.h"
#include "application/diff_view_coordinator.h"
#include "application/file_probe.h"
#include "application/git_workspace.h"
#include "application/repository_tabs.h"
#include "domain/app_settings.h"
#include "presentation/frame_feedback.h"
#include "presentation/settings/settings_controller.h"
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
#include <string>
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
            // 환경 설정 화면이 쓰는 값이다. 이번 실행이 찾은 git, ssh 도구의 위치, worker에게 시킬 일이다.
            git_tool_info git_tool {};
            std::function<void()> request_ssh_info {};
            std::function<void(std::string text)> copy_text {};
            std::u8string ssh_directory {};
            std::u8string default_key_comment {};
            bool ssh_tools_found { false };
            std::u8string ssh_tools_origin {};
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
        void handle_activate_ref(const activate_ref_intent& intent);
        void handle_commit();
        void handle_folder(const folder_chosen_intent& intent);
        void handle_text_edit(const luil::text_edit_request& request);
        void maybe_load_more();
        void show_notice(std::u8string text, bool warning = false);
        [[nodiscard]] luil::text::text_edit_state* draft(text_target target);
        [[nodiscard]] std::shared_ptr<const std::vector<luil::virtual_list_item>> graph_items(const repository_session& session);
        [[nodiscard]] bool repository_exists(std::u8string_view path);

        dependencies dependencies_ {};
        app_settings memory_settings_ {};
        repository_tabs tabs_ {};
        std::map<repository_tab_id, std::string> tab_repositories_ {};
        ui_state ui_ {};
        frame_feedback feedback_ {};
        // 마지막 frame의 배치다. 커밋을 고를 때 서랍을 열지 정한다.
        layout_mode last_mode_ { layout_mode::wide };
        std::map<std::string, graph_cache_entry> graph_cache_ {};
        std::map<std::u8string, bool> exists_cache_ {};
        // 끝나기를 기다리는 저장소 준비 작업이다. 끝나면 결과로 탭을 연다.
        std::map<operation_id, std::u8string> pending_inspections_ {};
        std::map<operation_id, std::u8string> pending_setup_ {};
        settings_controller settings_;
        // 판 폭을 설정에 쓸 시각이다. 폭을 끄는 동안 뒤로 밀리고, 값이 없으면 쓸 것이 없다.
        std::optional<std::chrono::steady_clock::time_point> pane_widths_flush_at_ {};
    };
} // namespace cyella
