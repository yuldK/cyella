#pragma once

#include "luil/ui/choice_group_element.h"

namespace cyella::ui {
    struct segment_option
    {
        std::u8string value {};
        std::u8string label {};
        char32_t glyph { 0 };
        // 자동화 이름(owner)을 따로 정할 때 지정한다. 비우면 `<owner>-<value>`다.
        std::u8string owner {};
    };

    inline constexpr float segmented_control_height { 28.0f };

    // 한 바탕과 하나의 선택 표시를 공유하는 배타 선택 UI다.
    class segmented_control_element final : public luil::ui_element
    {
    public:
        segmented_control_element(std::u8string owner, std::u8string name, std::vector<segment_option> options, std::u8string selected, luil::choice_message_factory select, bool icons_only = false);
        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        std::u8string name_ {};
    };

    [[nodiscard]] std::unique_ptr<luil::ui_element> make_segmented_control(luil::choice_group_config config);
    [[nodiscard]] std::unique_ptr<luil::ui_element> make_file_view_control(std::u8string owner, bool tree, luil::choice_message_factory select);
} // namespace cyella::ui
