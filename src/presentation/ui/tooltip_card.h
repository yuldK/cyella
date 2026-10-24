#pragma once

#include "luil/ui/text_layout.h"
#include "luil/ui/ui_element.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella::ui {
    // 카드 툴팁이 넘지 않는 폭이다 (논리 픽셀). 넘는 줄은 이 안에서 접는다.
    inline constexpr float tooltip_card_width { 360.0f };

    // 여러 줄이거나 한 줄로 길어지는 툴팁 글을 그리는 카드다.
    //  - `\n`으로 나눈 첫 줄이 제목이고 나머지는 흐린 설명 줄이다. 한 줄뿐이면 제목도 굵게 하지 않는다.
    //  - 배율 1의 논리 좌표로 배치한다. luil이 툴팁을 그릴 때 표면 배율을 캔버스에 얹는다.
    class tooltip_card_element final : public luil::ui_element
    {
    public:
        explicit tooltip_card_element(std::u8string_view text, float width = tooltip_card_width);
        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;
        [[nodiscard]] luil::size_f size() const noexcept
        {
            return size_;
        }

    private:
        struct line
        {
            luil::text_layout text {};
            float top { 0.0f };
            bool title { false };
        };
        std::u8string text_ {};
        std::vector<line> lines_ {};
        luil::size_f size_ {};
    };

    // 글이 한 줄 툴팁에 다 들지 않으면 카드 툴팁을 짓는다. 들면 비어 있다.
    [[nodiscard]] std::optional<luil::tooltip_config> make_tooltip_card(std::u8string_view text);

    // 툴팁 글을 단다. 글은 언제나 `set_tooltip`에 둔다 — 보조 기술은 그것을 HelpText로 읽는다.
    // 카드가 필요하면(`make_tooltip_card`) 그림은 카드가 맡는다.
    void set_text_tooltip(luil::ui_element& element, std::u8string text);
} // namespace cyella::ui
