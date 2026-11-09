#pragma once

#include "presentation/graph_geometry.h"
#include "presentation/ui/press_row.h"

#include "luil/ui/ui_element.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cyella::ui {
    // 글 한 줄의 폭을 글꼴 없이 짐작한다 (논리 픽셀). 배치에는 측정 단계가 없어 칩의 자리를 배치(`arrange`)에서
    // 정해야 하는데 그때는 글꼴이 없다. 한글·넓은 글자는 한 칸, 영문은 평균 폭, 좁은 글자는 좁게 센다.
    [[nodiscard]] float estimate_text_width(std::u8string_view text, float size) noexcept;

    // 글을 `width`(논리 픽셀) 안에 들어가게 줄로 나눈다. 공백에서 먼저 끊고, 공백이 없으면 글자 단위로 끊는다. 대화 상자의
    // 안내 글이 길 때 줄 수를 알아야 높이를 정할 수 있어서 같은 짐작(`estimate_text_width`)으로 센다.
    [[nodiscard]] std::vector<std::u8string> wrap_estimated(std::u8string_view text, float width, float size);

    // 라벨 칩의 폭이다 (논리 픽셀, `graph_row_element`의 칩 줄이 쓴다).
    [[nodiscard]] float estimate_ref_label_width(const ref_label& label) noexcept;

    // 이 라벨의 끌기·메뉴 키다. 브랜치는 사이드바 행과 같은 키(`l:main`, `rb:origin/main`)이고, 그 밖은 라벨 안에서만
    // 유일하다.
    [[nodiscard]] std::u8string ref_label_key(const ref_label& label);

    // 그래프 행 안의 ref 라벨 하나다. 행의 고르기와 따로 눌리고(누름 = 이 커밋 고르기, 두 번 = 체크아웃, 우클릭 =
    // 메뉴), 브랜치 라벨은 끌어서 다른 라벨·커밋·사이드바 항목에 놓을 수 있다.
    class ref_chip_element final : public press_row_element
    {
    public:
        // `row_key`는 그 행의 항목 키다 (`select_commit_intent`). `commit_sha`는 라벨이 붙은 커밋이다.
        // `row_selected`는 그 행이 골라져 있다는 뜻이다. 아니면 우클릭이 메뉴를 열기 전에 그 행을 고른다.
        ref_chip_element(ref_label label, std::u8string row_key, std::string commit_sha, std::uint8_t lane_color, bool row_selected = false);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        ref_label label_ {};
        std::uint8_t lane_color_ { 0 };
        std::u8string access_name_ {};
    };

    // 칸이 모자라 줄어든 라벨들의 "+N" 단추다. 호버·클릭으로 이 커밋의 전체 ref 목록을 연다.
    class ref_overflow_element final : public luil::ui_element
    {
    public:
        ref_overflow_element(std::u8string owner, std::size_t hidden, std::string sha);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        std::u8string text_ {};
    };

    // 참조 목록 카드와 그 안의 요소(메뉴·항목·흘리는 창)는 owner가 모두 이것으로 시작한다.
    // 카드는 popup 창이 아니라 주 창 tree에 서므로, 호버 감시는 "포인터가 카드 위인가"를 표면 이름이 아니라 이것으로 가린다.
    inline constexpr std::u8string_view ref_list_owner_prefix { u8"ref-list" };
    [[nodiscard]] bool is_ref_list_part(const luil::ui_element_id& id) noexcept;
    // 카드 자신의 id다. tree에 이것이 있으면 카드가 떠 있다.
    [[nodiscard]] luil::ui_element_id ref_list_card_id();
} // namespace cyella::ui
