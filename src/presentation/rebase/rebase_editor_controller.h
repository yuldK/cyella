#pragma once

#include "application/git_workspace.h"
#include "presentation/rebase/rebase_editor_intents.h"
#include "presentation/rebase/rebase_editor_state.h"
#include "presentation/rebase/rebase_editor_view.h"
#include "presentation/ui_state.h"

#include "luil/ui/app_message.h"
#include "luil/ui/ui_interaction.h"

#include <optional>

namespace cyella {
    // 인터랙티브 rebase 편집기의 상태를 소유하고 그 메시지를 처리한다. `shell_controller`가 소유하고 logic thread에서만 쓴다.
    //
    // 흐름: 열면 `load_rebase_plan`을 내고, 범위가 오면(`after_events`) 목록을 채운다. 시작하면 `interactive_rebase`를 내고
    // 끝나기를 기다린다. 성공하거나 충돌로 멈추면(`needs_input{conflict}`) 닫고, 실행 전에 거절되면(검사·확인 실패) 열어 둔 채
    // 이유를 보인다. 충돌 뒤는 기존 충돌 화면이 맡는다.
    class rebase_editor_controller
    {
    public:
        struct dependencies
        {
            git_workspace* workspace { nullptr };
        };

        explicit rebase_editor_controller(dependencies dependencies);

        [[nodiscard]] bool is_open() const noexcept
        {
            return state_.has_value();
        }

        [[nodiscard]] const std::optional<rebase_editor_state>& state() const noexcept
        {
            return state_;
        }

        void open(const open_rebase_editor_intent& intent);
        void close();

        // 편집기 메시지면 처리하고 true다. 닫혀 있으면 늦게 온 것이라 삼킨다.
        bool handle(const luil::app_message& message);
        // relay를 비운 뒤다. 범위가 왔으면 목록을 채우고, 실행이 끝났으면 닫거나 이유를 보인다.
        void after_events();

        // 텍스트 칸의 초안이다. 편집기 칸이 아니거나 메시지 칸이 없으면 null이다.
        [[nodiscard]] luil::text::text_edit_state* draft(text_target target);
        // 편집기 칸의 편집이면 적용하고 true다. 본문은 여러 줄이라 줄바꿈을 그대로 두고 Home·End를 그 줄의 처음·끝으로 옮긴다.
        bool edit_text(const luil::text_edit_request& request);

        [[nodiscard]] std::optional<rebase_editor_view> make_view(const std::optional<luil::text_composition_event>& composition) const;

    private:
        [[nodiscard]] rebase_message_draft* selected_draft();
        [[nodiscard]] std::string sha_of(const std::u8string& row_key) const;
        void start();

        dependencies dependencies_ {};
        std::optional<rebase_editor_state> state_ {};
    };
} // namespace cyella
