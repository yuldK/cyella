#pragma once

#include "luil/ui/ui_interaction.h"

#include <optional>
#include <vector>

namespace cyella::ui {
    // luil 상호작용 상태 기계가 모르는 앱 정책이다. 어느 element가 텍스트 칸인지, 편집을 어떤 메시지로 보낼지,
    // 휠을 어디로 흘릴지, 남은 키가 무엇이 되는지를 답한다. input thread에서 불리므로 상태를 두지 않는다.
    //
    // 휠은 표 없는 `route_wheel`이다. 흘리는 창(`virtual_list_element`·`scroll_area_element`)이 자기 스크롤
    // 메시지를 들고 있어 앱이 표를 지을 필요가 없다. 이 정책이 없으면(nullptr) 텍스트 입력과 휠이 함께 꺼진다.
    class cyella_policy final : public luil::interaction_policy
    {
    public:
        [[nodiscard]] std::optional<luil::text_input_target> text_target_of(luil::ui_element_kind kind) const override;
        // 우클릭 메뉴(`luil::menu_element`)의 키보드 탐색(↑/↓/Enter)과 Esc 닫기다.
        [[nodiscard]] std::optional<luil::menu_kinds> menu() const override;
        [[nodiscard]] std::vector<luil::input_action> close_menu() const override;
        [[nodiscard]] luil::input_action make_text_edit_action(const luil::text_edit_request& request) const override;
        [[nodiscard]] luil::input_action make_text_composition_action(const luil::text_composition_event& event) const override;
        [[nodiscard]] std::vector<luil::input_action> on_wheel(const luil::ui_tree& tree, const luil::mouse_wheel_event& event, float scroll_delta) override;
        [[nodiscard]] std::vector<luil::input_action> on_focus_moved(const luil::ui_tree& tree, const luil::ui_element_id& focused) override;
        [[nodiscard]] std::vector<luil::input_action> on_key(const luil::ui_tree* tree, const luil::key_pressed_event& event, const luil::interaction_snapshot& snapshot) override;

    private:
        // 인터랙티브 rebase 편집기의 키다. 편집기의 키가 아니면 값이 없다.
        [[nodiscard]] static std::optional<std::vector<luil::input_action>> rebase_editor_key(
            const luil::ui_tree* tree, const luil::key_pressed_event& event, const luil::interaction_snapshot& snapshot);
    };
} // namespace cyella::ui
