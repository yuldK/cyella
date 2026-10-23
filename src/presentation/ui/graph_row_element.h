#pragma once

#include "domain/commit_graph_layout.h"
#include "domain/commit_signature.h"
#include "presentation/graph_geometry.h"
#include "presentation/ui/press_row.h"

#include "luil/ui/ui_element.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cyella::ui {
    // 그래프 목록의 열 폭이다 (논리 픽셀). 그래프 칸만 레인 수로 정한다.
    struct graph_columns
    {
        float refs { 160.0f };
        float graph { 60.0f };
        float author { 110.0f };
        float date { 90.0f };
        // narrow에서 작성자·날짜 열을 감춘다.
        bool compact { false };

        // 그래프 칸이 이 폭을 넘으면 자른다. 레인이 수십 개인 저장소에서도 메시지 열이 남아야 한다.
        static constexpr float maximum_graph { 240.0f };
    };

    // 목록 폭(`available_width`, 논리 픽셀)에 맞춘 열 폭이다. 0 이하면 폭을 모르는 것으로 보고 기본 폭을 그대로 쓴다.
    //  - 브랜치/태그 열은 가장 넓은 라벨 줄(`labels_width`, 칩 폭의 합)만큼만 선다. 기본 폭이 상한이고 머리 글자가 들어가는 폭이 하한이다.
    //    라벨이 짧은 저장소에서 빈 열이 메시지 자리를 먹지 않는다. 음수면 라벨을 모르는 것으로 보고 기본 폭을 쓴다.
    //  - 커밋 메시지 열은 남는 자리를 갖는다. 그래서 폭이 모자라면 메시지가 몇 글자만 남는다. 메시지 열의 하한을 먼저 지키고,
    //    모자란 만큼을 그래프 → 브랜치/태그 → 작성자 → 날짜 순으로 각자의 하한까지 줄여 마련한다.
    //  - 그래프를 먼저 줄이는 것은 잘리는 쪽이 오른쪽 레인이라 지금 보는 줄기(왼쪽 레인)는 남기 때문이다.
    [[nodiscard]] graph_columns make_graph_columns(std::uint16_t lane_count, bool compact, float available_width = 0.0f, float labels_width = -1.0f) noexcept;

    // 라벨 칩을 한 줄로 늘어놓은 폭이다 (논리 픽셀, 칩 사이 간격 포함). 브랜치/태그 열의 폭을 정할 때 쓴다.
    [[nodiscard]] float estimate_ref_label_strip_width(const std::vector<ref_label>& labels) noexcept;

    // 행 하나를 그리는 데 필요한 값이다. 짓는 순간 세션에서 복사해 와서 게시된 tree가 세션을 붙잡지 않는다.
    struct graph_row_data
    {
        graph_row row {};
        std::u8string subject {};
        std::u8string author {};
        std::u8string date {};
        std::vector<ref_label> labels {};
        std::size_t index { 0 };
        bool wip { false };
        std::uint32_t wip_count { 0 };
        // WIP 점선이 끝나는 HEAD 행 번호와 WIP 레인이다. WIP가 없으면 쓰지 않는다.
        bool has_wip { false };
        std::size_t head_row_index { 0 };
        std::uint16_t wip_lane { 0 };
        bool head { false };
        // 그 커밋의 항목 키(`graph_item_key`)와 SHA다. 행과 라벨 칩이 누르면 이 커밋을 고르고, 메뉴·끌어 놓기가 이 커밋을 가리킨다.
        std::u8string row_key {};
        std::string sha {};
        // 세션이 읽어 둔 서명이다. 읽지 않았거나 서명이 없으면 표지를 달지 않는다.
        bool signature_known { false };
        signature_status signature { signature_status::none };
        // 목록이 고른 행으로 그리는 행이다 (여러 커밋 비교 포함). 고르지 않은 행의 우클릭은 메뉴를 열기 전에 그 행만 고른다.
        bool selected { false };
    };

    class ref_chip_element;

    // 커밋 그래프의 행 내용이다 (라벨 | 그래프 | 메시지 | 작성자 | 날짜). hover 표시는 목록의 행이 그린다.
    // 이 element는 눌림을 직접 받으므로 (누름 = 고르기, 두 번 = 체크아웃, 우클릭 = 메뉴, 놓기 = 브랜치 메뉴) 목록 행의
    // 고르기와 같은 메시지를 다시 낸다. 라벨은 자식 칩이라 칩마다 따로 눌리고 끌린다.
    class graph_row_element final : public press_row_element
    {
    public:
        graph_row_element(std::u8string owner, graph_row_data data, graph_columns columns);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        luil::ui_element* add_chip(std::unique_ptr<luil::ui_element> chip);
        void draw_graph(luil::draw_context& context, const luil::rect_f& cell) const;

        graph_row_data data_ {};
        graph_columns columns_ {};
        std::u8string access_name_ {};
        // 라벨 칩들이다 (자식이 소유한다). 칸이 모자라면 뒤쪽은 숨고 `overflow_`가 "+N"을 보인다.
        std::vector<ref_chip_element*> chips_ {};
        luil::ui_element* overflow_ { nullptr };
    };

    // 그래프 목록 위의 열 머리다.
    class graph_header_element final : public luil::ui_element
    {
    public:
        explicit graph_header_element(graph_columns columns);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;

    private:
        graph_columns columns_ {};
    };

    // 라벨 칩 하나의 폭이다 (물리 픽셀). 상세 패널의 라벨 줄도 같은 그림을 쓴다.
    [[nodiscard]] float measure_ref_label(const luil::draw_context& context, const ref_label& label);
    void draw_ref_label(luil::draw_context& context, const ref_label& label, const luil::rect_f& box, luil::ui_color lane);

    // 라벨 칩을 가로로 늘어놓는 element다 (상세 패널).
    class ref_label_strip_element final : public luil::ui_element
    {
    public:
        ref_label_strip_element(std::u8string owner, std::vector<ref_label> labels, std::uint8_t lane_color);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        std::vector<ref_label> labels_ {};
        std::uint8_t lane_color_ { 0 };
    };
} // namespace cyella::ui
