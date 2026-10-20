#pragma once

#include "luil/ui/text_input_state.h"
#include "luil/ui/ui_element.h"

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cyella::ui {
    // 여러 줄 텍스트 칸이다 (커밋 메시지 본문). luil에는 한 줄 칸(`text_input_element`)만 있어 앱이 짓는다.
    //
    // 편집 상태는 앱이 들고(`text_input_view`), 칸은 그 값을 그리고 포인터 자리를 offset으로 옮겨 주기만 한다.
    //  - `text_input()`이 `multiline`을 켜서 붙여넣기가 줄바꿈을 지킨다 (luil 7c4db82).
    //  - 포인터 판정은 `offset_at_point`가 줄(y)과 열(x)로 한다.
    //  - Enter·↑·↓·Home·End는 앱 정책(`cyella_policy`)과 controller가 줄 단위로 처리한다.
    //  - 글은 줄을 감싸지 않는다. 칸보다 긴 줄은 잘리고, caret이 칸 밖으로 나가면 세로로만 흘러 caret 줄을 보인다.
    class multiline_text_element final : public luil::ui_element
    {
    public:
        multiline_text_element(luil::ui_element_id id, luil::text_input_view value, std::u8string placeholder, std::u8string access_name);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> next_update(const luil::update_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] std::optional<std::size_t> offset_at(float x, const luil::text_measurer& measurer) const override;
        [[nodiscard]] std::optional<std::size_t> offset_at_point(float x, float y, const luil::text_measurer& measurer) const override;
        [[nodiscard]] std::optional<luil::text_input_snapshot> text_input() const override;
        [[nodiscard]] std::optional<luil::rect_f> text_span_bounds(const luil::text_span_query& query, const luil::text_measurer& measurer) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        // 글 안의 줄 구간이다 (`[처음, 끝)` byte, 줄바꿈 제외).
        using line_range = std::pair<std::size_t, std::size_t>;

        [[nodiscard]] static std::vector<line_range> split_lines(std::u8string_view text);
        [[nodiscard]] static std::size_t line_of(const std::vector<line_range>& lines, std::size_t offset) noexcept;
        [[nodiscard]] std::u8string_view displayed_text() const noexcept;
        [[nodiscard]] std::size_t displayed_caret() const noexcept;
        [[nodiscard]] float line_height() const noexcept;
        [[nodiscard]] float text_left() const noexcept;
        [[nodiscard]] float text_top() const noexcept;
        [[nodiscard]] std::size_t visible_lines() const noexcept;
        // caret 줄이 보이도록 위에서 건너뛸 줄 수다. 글과 caret만으로 정해져 그리기와 포인터 판정이 같다.
        [[nodiscard]] std::size_t first_line(const std::vector<line_range>& lines, std::size_t caret) const noexcept;

        luil::text_input_view value_ {};
        std::u8string placeholder_ {};
        std::u8string access_name_ {};
        float scale_ { 1.0f };
        float font_size_ { 11.0f };
    };
} // namespace cyella::ui
