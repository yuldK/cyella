#pragma once

#include "luil/ui/strip_element.h"
#include "luil/ui/tab_bar_element.h"

#include <memory>
#include <string>
#include <vector>

namespace cyella::ui {
    // 탭과 추가 단추가 함께 흘러가고, 넘칠 때만 가로 막대를 보이는 저장소 탭 줄이다.
    class repository_tabs_element final : public luil::ui_element
    {
    public:
        repository_tabs_element(luil::tab_bar_config config, std::unique_ptr<luil::ui_element> add_button, float viewport_width, bool reveal_selected);
        void set_tabs_name(std::u8string name);
        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] float scroll_offset() const noexcept;

    private:
        luil::strip_element* lane_ { nullptr };
        luil::tab_bar_element* bar_ { nullptr };
        luil::ui_element* scrollbar_ { nullptr };
        std::vector<const luil::ui_element*> tabs_ {};
        std::u8string selected_ {};
        bool overflowing_ { false };
    };
} // namespace cyella::ui
