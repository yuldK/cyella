#pragma once

#include "application/app_settings_service.h"
#include "application/file_probe.h"
#include "application/git_workspace.h"
#include "domain/app_settings.h"
#include "domain/git_tool.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/settings/settings_state.h"
#include "presentation/settings/settings_view.h"
#include "presentation/ui_state.h"

#include "luil/ui/app_message.h"

#include <functional>
#include <optional>
#include <string>

namespace cyella {
    // 환경 설정 화면의 상태를 소유하고 그 메시지를 처리한다. `shell_controller`가 소유하고 logic thread에서만 쓴다.
    //
    // 저장하는 값(테마·git 경로·외부 diff 도구 표)은 `update`(보통 `app_settings_service::update`)로 바꾼다. 파일을 읽지 못해
    // 읽기 전용이면 서비스가 파일을 건드리지 않고 메모리만 바꾸므로, 화면이 배너로 그 사실을 알린다.
    // ssh 도구 실행은 workspace의 setup operation(`ssh_keygen`·`ssh_connection_test`)이다. passphrase 질문이 askpass로
    // 와서 operation의 `needs_input`이 되므로 인증 대화 상자가 그것을 받는다.
    class settings_controller
    {
    public:
        struct dependencies
        {
            std::function<const app_settings&()> settings {};
            std::function<void(const std::function<void(app_settings&)>&)> update {};
            std::function<bool()> read_only {};
            std::function<bool()> unsaved {};
            const file_probe* probe { nullptr };
            // 이번 실행이 찾은 git이다.
            git_tool_info git_tool {};
            git_workspace* workspace { nullptr };
            // agent·키 목록을 worker에게 읽어 오게 한다. 결과는 `ssh_info_loaded_intent`로 온다.
            std::function<void()> request_ssh_info {};
            std::function<void(std::string text)> copy_text {};
            // 키를 만들 폴더(`%USERPROFILE%\.ssh`)와 새 키 주석의 기본값(`사용자@PC`)이다.
            std::u8string ssh_directory {};
            std::u8string default_key_comment {};
            bool ssh_tools_found { false };
            std::u8string ssh_tools_origin {};
            // 사용자 프로필 쪽이 보이는 선택된 저장소의 값이다. 비어 있으면 저장소가 없는 것으로 본다.
            std::function<profiles_repository_inputs()> repository_profile {};
        };

        explicit settings_controller(dependencies dependencies);

        [[nodiscard]] bool is_open() const noexcept
        {
            return state_.has_value();
        }

        [[nodiscard]] const std::optional<settings_state>& state() const noexcept
        {
            return state_;
        }

        // 화면 메시지면 처리하고 true다.
        bool handle(const luil::app_message& message);
        // 끝난 operation의 후속이다 (키를 만들었으면 목록을 다시 읽는다).
        void after_events();
        // 흘리는 창이 배치된 뒤의 최대치다. 스크롤을 범위 안으로 다듬는 데 쓴다.
        void apply_scroll_maximum(std::optional<float> maximum);

        // 텍스트 입력 칸의 초안이다. 환경 설정 칸이 아니거나 닫혀 있으면 null이다.
        [[nodiscard]] luil::text::text_edit_state* draft(text_target target);

        // 열려 있을 때만 값이 있다.
        [[nodiscard]] std::optional<settings_view> make_view(const std::optional<luil::text_composition_event>& composition) const;

    private:
        void open(settings_page page);
        void update_settings(const std::function<void(app_settings&)>& change);
        void apply_git_path();
        void start_tool_edit(std::optional<std::size_t> index);
        void save_tool();
        void remove_tool(std::size_t index);
        void request_ssh_info();
        void create_ssh_key();
        void test_ssh_connection();
        void copy_public_key(const std::u8string& name);
        [[nodiscard]] ssh_task_input task_of(operation_id id) const;

        // ---- 단축키 쪽 (settings_controller_shortcuts.cpp)과 사용자 프로필 쪽 (settings_controller_profiles.cpp)
        [[nodiscard]] static bool is_shortcut_message(const luil::app_message& message);
        void handle_shortcut_message(const luil::app_message& message);
        [[nodiscard]] static bool is_profile_message(const luil::app_message& message);
        void handle_profile_message(const luil::app_message& message);

        dependencies dependencies_ {};
        std::optional<settings_state> state_ {};
        std::optional<float> scroll_maximum_ {};
        // 목록을 이미 다시 읽은 키 만들기 operation이다.
        operation_id refreshed_after_ { 0 };
    };
} // namespace cyella
