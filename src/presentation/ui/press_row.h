#pragma once

#include "luil/ui/ui_element.h"

#include <utility>
#include <vector>

namespace cyella::ui {
    // 마우스를 누르는 순간 고르는 목록 행이다 (그래프 행·라벨 칩·파일 행·사이드바 행).
    //
    // luil의 클릭 트리거는 **뗀** 순간에 나간다. 고르기를 왼쪽 클릭에 달면 누른 채로는 아직 옛 행이 골라져 있어, 누른 행과 고른 행이
    // 어긋나 보인다. 그래서 행은 고르기를 이 자리에 두고 `cyella_policy::on_press`가 누른 순간(마우스·펜·터치) 이것을 낸다.
    //  - 왼쪽 클릭 액션은 두지 않는다. 두면 뗄 때 한 번 더 골라 Ctrl+클릭(하나 더하기·빼기)이 두 번 뒤집힌다. 두 번 누름·우클릭·끌기는
    //    그대로 element의 트리거다.
    //  - 터치는 닿는 순간 고른다. 그 접촉이 스크롤로 끝나도 고른 것은 남는다.
    //  - 보조 기술의 "실행"은 누름이 아니므로 행마다 접근 처리(`set_access_handler`)로 같은 고르기를 잇는다.
    class press_row_element : public luil::ui_element
    {
    public:
        using luil::ui_element::ui_element;

        // 누른 순간의 고르기 메시지다. 고를 것이 없는 행이면 비어 있다.
        [[nodiscard]] std::vector<luil::input_action> press(const luil::ui_action_context& context) const
        {
            return press_ ? press_(context) : std::vector<luil::input_action> {};
        }

        [[nodiscard]] bool pressable() const noexcept
        {
            return static_cast<bool>(press_);
        }

        // 누르면 초점이 갈 목록이다. 행 내용은 초점 자리가 아니라, 정하지 않으면 누를 때 초점이 사라져 ↑↓·Space·Enter가 목록에 닿지
        // 않는다 (luil 목록의 기본 행이 하는 일을 이 element가 대신한다).
        void set_focus_owner(luil::ui_element_id owner)
        {
            focus_owner_ = std::move(owner);
        }

        [[nodiscard]] const luil::ui_element_id& pointer_focus_target() const noexcept override
        {
            return focus_owner_ == luil::ui_element_id {} ? id() : focus_owner_;
        }

    protected:
        // 누름을 단다. 액션이 없어도 hit를 받도록 막는다 — 받지 않으면 누름이 목록 행으로 내려가 뗄 때 다시 고른다.
        void set_press(luil::ui_action action)
        {
            press_ = std::move(action);
            set_hit_opaque(true);
        }

    private:
        luil::ui_action press_ {};
        luil::ui_element_id focus_owner_ {};
    };
} // namespace cyella::ui
