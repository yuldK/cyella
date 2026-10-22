#pragma once

#include "luil/ui/strip_element.h"
#include "luil/ui/tab_bar_element.h"
#include "luil/ui/transition.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cyella::ui {
    // 탭과 추가 단추가 함께 흘러가고, 넘칠 때만 가로 막대를 보이는 저장소 탭 줄이다.
    //
    // 끌어 옮기기는 luil 탭의 끌기 위에 얹는다.
    //  - 놓을 자리는 대상 탭의 어느 반쪽에 올렸는가로 앞·뒤를 가른다. 순서가 그대로인 자리는 받지도 표시하지도 않는다.
    //  - 끌리는 탭은 탭 모양 그대로 줄을 따라 미끄러지고, 끼울 틈의 표시는 틈 사이를 미끄러져 옮겨 간다.
    class repository_tabs_element final : public luil::ui_element
    {
    public:
        // 끌어 놓은 탭(`moved`)을 대상 탭의 앞(`after`가 거짓)이나 뒤로 옮기자는 메시지다.
        using place_factory = std::function<luil::input_action(const std::u8string& moved, const std::u8string& target, bool after)>;

        repository_tabs_element(luil::tab_bar_config config, place_factory place, std::unique_ptr<luil::ui_element> add_button, float viewport_width, bool reveal_selected);
        void set_tabs_name(std::u8string name);
        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> next_update(const luil::update_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] float scroll_offset() const noexcept;

    private:
        // 끼울 틈의 표시가 움직이는 상태다. 값은 탭 폭을 1로 센 탭 줄 위의 자리다.
        //  - 그리기만의 상태라 element가 든다. 끄는 도중 tree가 다시 지어지면(넘침 자동 스크롤·배경 갱신) 처음부터
        //    다시 서서 한 번 미끄러지지 않고 그 자리에 뜰 뿐이다.
        struct drop_motion
        {
            bool visible { false };
            // 틈의 세로선 자리다.
            luil::transition caret {};
            // 놓일 쪽 반쪽 강조의 가운데다.
            luil::transition wash {};
        };

        // 이 줄의 탭이면 그 인덱스다. 다른 곳에서 시작한 끌기나 탭이 아닌 대상은 없다.
        [[nodiscard]] std::optional<std::size_t> index_of(const luil::ui_element_id& id) const noexcept;
        [[nodiscard]] bool lands_after(std::size_t target, float x) const noexcept;
        // 끼울 자리를 정해 움직임을 고치고 반쪽 강조를 그린다. 틈의 세로선은 끌리는 탭 위에 따로 긋는다(`draw_caret`).
        void draw_drop(luil::draw_context& context, const luil::drag_visual& drag, std::size_t moved) const;
        void draw_caret(luil::draw_context& context) const;
        void draw_dragged(luil::draw_context& context, const luil::interaction_snapshot& interaction, std::size_t moved) const;

        luil::strip_element* lane_ { nullptr };
        luil::tab_bar_element* bar_ { nullptr };
        luil::ui_element* scrollbar_ { nullptr };
        std::vector<luil::ui_element*> tabs_ {};
        std::u8string selected_ {};
        place_factory place_ {};
        bool overflowing_ { false };
        // draw만 고치고 UI thread에서만 읽는다.
        mutable drop_motion motion_ {};
    };
} // namespace cyella::ui
