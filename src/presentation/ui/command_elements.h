#pragma once

#include "presentation/commands/command_registry.h"
#include "presentation/palette/palette_view.h"

#include "luil/ui/ui_element.h"
#include "luil/ui/ui_events.h"

#include <string_view>
#include <vector>

namespace cyella::ui {
    // 입력 thread의 `cyella_policy`는 상태를 두지 않으므로, 키 처리가 읽을 값은 frame마다 tree에 보이지 않는 표식으로 싣는다.
    // 표식은 그리지 않고 누름도 받지 않으며 보조 기술에도 보이지 않는다.
    inline constexpr std::u8string_view shortcut_table_owner { u8"shortcut-table" };
    inline constexpr std::u8string_view palette_enter_owner { u8"palette-enter" };

    // 지금 단축키 표다 (명령 레지스트리의 기본값에 사용자가 바꾼 값을 합친 것).
    class shortcut_table_element final : public luil::ui_element
    {
    public:
        explicit shortcut_table_element(std::vector<shortcut_binding> bindings);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;

        [[nodiscard]] const std::vector<shortcut_binding>& bindings() const noexcept
        {
            return bindings_;
        }

    private:
        std::vector<shortcut_binding> bindings_ {};
    };

    // 팔레트의 거르는 칸에서 Enter를 누르면 낼 액션이다 (커서가 선 항목을 고르는 액션).
    class palette_enter_element final : public luil::ui_element
    {
    public:
        explicit palette_enter_element(luil::input_action action);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;

        [[nodiscard]] const luil::input_action& action() const noexcept
        {
            return action_;
        }

    private:
        luil::input_action action_ {};
    };

    // 명령 하나를 실행하는 액션이다. UI thread가 실행해야 하는 명령(폴더 대화 상자)은 UI 명령이고 나머지는 `run_command_intent`다.
    // 단축키와 팔레트가 같은 길을 쓴다.
    [[nodiscard]] luil::input_action action_for_command(std::string_view id);
    // 팔레트 항목을 고르는 액션이다.
    [[nodiscard]] luil::input_action palette_item_action(const palette_item_view& item);
} // namespace cyella::ui
