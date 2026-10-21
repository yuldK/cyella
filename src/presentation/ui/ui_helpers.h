#pragma once

#include "presentation/shell_layout.h"
#include "presentation/ui/element_kinds.h"

#include "luil/generated/codicons.h"
#include "luil/theme/ui_theme.h"
#include "luil/ui/button_element.h"
#include "luil/ui/dialog_elements.h"
#include "luil/ui/draw_primitives.h"
#include "luil/ui/image_element.h"
#include "luil/ui/label_element.h"
#include "luil/ui/panel_element.h"
#include "luil/ui/ui_element.h"
#include "luil/ui/ui_events.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class SkFont;

namespace cyella::ui {
    namespace codicons = luil::codicons;

    using color_selector = std::function<luil::ui_color(const luil::ui_color_palette&)>;

    // ---------------------------------------------------------------- 액션

    template<typename intent_type>
    [[nodiscard]] luil::input_action message(intent_type intent)
    {
        return luil::make_app_action(std::move(intent));
    }

    // 실행되면 액션 하나를 내는 element 액션이다.
    [[nodiscard]] luil::ui_action action_of(luil::input_action action);
    // UI thread 명령이다 (폴더 대화 상자).
    [[nodiscard]] luil::input_action ui_command(std::uint32_t command, std::u8string argument = {});

    // ---------------------------------------------------------------- 색

    // cyella 앱 색이다. luil 팔레트에 없는 레인 색 8개와 성공색을 테마(어두운 배경인지)로 고른다.
    // 값은 diff 뷰어 CSS의 `--lane-*`, `--success-accent`와 같은 색이다.
    [[nodiscard]] luil::ui_color lane_color(const luil::ui_color_palette& palette, std::uint8_t index) noexcept;
    [[nodiscard]] luil::ui_color success_color(const luil::ui_color_palette& palette) noexcept;
    [[nodiscard]] luil::ui_color diff_add_background(const luil::ui_color_palette& palette) noexcept;
    [[nodiscard]] luil::ui_color diff_delete_background(const luil::ui_color_palette& palette) noexcept;
    // git 상태 글자(M·A·D·R·U·?)의 색이다.
    [[nodiscard]] luil::ui_color status_color(const luil::ui_color_palette& palette, char status) noexcept;

    // ---------------------------------------------------------------- 글자

    [[nodiscard]] std::u8string number_text(std::uint64_t value);
    [[nodiscard]] std::u8string to_u8(std::string_view text);
    [[nodiscard]] std::string to_text(std::u8string_view text);

    // ---------------------------------------------------------------- 조립기

    // 누를 수 없는 글 한 줄이다. 잘린 글은 툴팁으로 되살린다.
    [[nodiscard]] std::unique_ptr<luil::label_element> make_label(std::u8string owner, std::u8string text, float size = 12.0f, luil::label_color_role role = luil::label_color_role::primary);
    // 아이콘만 있는 단추다. 툴팁이 곧 이름이다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> make_icon_button(
        std::u8string owner, char32_t glyph, std::u8string tooltip, luil::input_action action, bool enabled = true, bool active = false, float icon_size = 14.0f);
    [[nodiscard]] std::unique_ptr<luil::ui_element> make_text_button(
        std::u8string owner, std::u8string text, luil::text_button_visual visual, luil::input_action action, bool enabled = true, char32_t glyph = 0, bool default_button = false);
    // 글자 단추가 담길 폭이다 (논리 픽셀). 측정 단계가 없어 글자 수로 짐작한다.
    [[nodiscard]] float text_button_width(std::u8string_view text, bool glyph = false) noexcept;

    struct text_style
    {
        float size { 12.0f };
        color_selector color {};
        bool bold { false };
        bool code { false };
        // 0 왼쪽, 1 가운데, 2 오른쪽이다.
        int align { 0 };
    };

    // 색을 팔레트 선택자로 받는 글 한 줄이다. `label_element`의 세 역할로 모자란 색(경고·성공·강조)에 쓴다.
    class text_element final : public luil::ui_element
    {
    public:
        text_element(luil::ui_element_id id, std::u8string text, text_style style);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        std::u8string text_ {};
        text_style style_ {};
    };

    [[nodiscard]] std::unique_ptr<luil::ui_element> make_text(std::u8string owner, std::u8string text, text_style style);

    // 폭에 맞춰 줄을 바꾸는 글이다 (커밋 제목·본문). 줄 나누기는 그릴 때 실제 글꼴로 한다(`wrap_text_lines`).
    //  - 높이는 담는 쪽이 `wrapped_text_height`로 짐작해 준다. 측정 단계가 없어서다. 짐작은 넉넉하게 잡혀 대개 줄이 남는다.
    //  - 그래도 자리보다 줄이 많으면 마지막 줄을 그 문단 끝까지 이어 그리고 말줄임으로 자른다. 글이 조용히 사라지지 않는다.
    class wrapped_text_element final : public luil::ui_element
    {
    public:
        wrapped_text_element(luil::ui_element_id id, std::u8string text, text_style style, float line_height);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        std::u8string text_ {};
        text_style style_ {};
        float line_height_ { 18.0f };
    };

    // 작성자 한 사람이다: 사진 동그라미와 굵은 이름 (커밋 메시지 카드의 아랫줄).
    //  - 사진이 없으면(Gravatar를 껐거나 사진이 없는 사람) 이름 첫 글자 동그라미다.
    //  - 메일은 화면에 늘어놓지 않고 올리면 툴팁으로 보인다. 보조 기술에는 이름과 메일을 함께 읽힌다.
    class author_element final : public luil::ui_element
    {
    public:
        author_element(std::u8string owner, std::u8string name, std::u8string email, luil::ui_image avatar);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

        // 사진·간격·이름이 차지할 폭이다 (논리 픽셀, 이름은 글자로 짐작한다). 담는 쪽이 줄의 남은 자리를 나눌 때 쓴다.
        [[nodiscard]] static float width_for(std::u8string_view name) noexcept;

    private:
        std::u8string name_ {};
        std::u8string email_ {};
        std::u8string initial_ {};
        luil::ui_element* photo_ { nullptr };
    };

    // 칸 여럿을 위에서 아래로 한 테두리 안에 담는 카드다 (커밋 메시지: 제목 · 본문 · 작성 정보).
    //  - 칸 사이는 구분선이고, `tinted` 칸(머리·바닥)은 옅은 바탕이다. 둥근 모서리로 바탕과 자식을 함께 자른다.
    //  - 높이는 칸 높이의 합에 테두리 두 줄을 더한 값이다 (`height_for`). 담는 쪽이 그 높이로 놓는다.
    class card_element final : public luil::ui_element
    {
    public:
        struct section
        {
            std::unique_ptr<luil::ui_element> content {};
            // 논리 픽셀이다.
            float height { 0.0f };
            bool tinted { false };
        };

        card_element(std::u8string owner, std::u8string access_name, std::vector<section> sections);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

        [[nodiscard]] static float height_for(const std::vector<section>& sections) noexcept;

    private:
        struct placed
        {
            luil::ui_element* content { nullptr };
            float height { 0.0f };
            bool tinted { false };
        };

        std::u8string access_name_ {};
        std::vector<placed> sections_ {};
    };

    // `width`(논리 픽셀)에 담을 때 잡을 높이다. 빈 글은 0이다.
    [[nodiscard]] float wrapped_text_height(std::u8string_view text, float width, const text_style& style, float line_height);
    [[nodiscard]] std::unique_ptr<luil::ui_element> make_wrapped_text(std::u8string owner, std::u8string text, text_style style, float line_height);

    // 서랍·떠 있는 판·대화 상자가 쓰는 바탕이다 (표면색, 구분선 테두리, 그림자).
    [[nodiscard]] luil::panel_config floating_panel_config(float corner_radius, float shadow);

    // 자식마다 논리 픽셀 자리를 직접 정해 담는 판이다. 셸처럼 자리를 계산 함수 하나가 정하는 곳에 쓴다.
    class absolute_element final : public luil::ui_element
    {
    public:
        explicit absolute_element(luil::ui_element_id id);

        // 빈 사각형이면 담지 않는다.
        void place(std::unique_ptr<luil::ui_element> child, const shell_rect& rect);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;

    private:
        struct entry
        {
            luil::ui_element* child { nullptr };
            shell_rect rect {};
        };

        std::vector<entry> entries_ {};
    };

    // 색 바탕과 아래·위 구분선을 가진 막대다 (탭 줄·툴바·상태 줄). 내용 하나를 담는다.
    struct bar_style
    {
        color_selector background {};
        bool divider_bottom { false };
        bool divider_top { false };
    };

    class bar_element final : public luil::ui_element
    {
    public:
        bar_element(luil::ui_element_id id, bar_style style, std::unique_ptr<luil::ui_element> content);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;

    private:
        bar_style style_ {};
        luil::ui_element* content_ { nullptr };
    };

    // 툴바·좁은 사이드바 단추다. 툴바는 가로, 레일은 아이콘 아래에 글자를 둔다.
    class tool_button_element final : public luil::ui_element
    {
    public:
        tool_button_element(std::u8string owner, char32_t glyph, std::u8string label, std::u8string tooltip, bool show_label, bool enabled, luil::input_action action, bool horizontal = false);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        char32_t glyph_ { 0 };
        std::u8string label_ {};
        bool show_label_ { true };
        bool horizontal_ { false };
    };

    // 진행 중 작업의 도는 아이콘이다. 시간만의 함수라 `next_update`로 다시 그리기를 예고한다.
    class spinner_element final : public luil::ui_element
    {
    public:
        spinner_element(luil::ui_element_id id, char32_t glyph, color_selector color);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> next_update(const luil::update_context& context, const luil::interaction_snapshot& interaction) const override;

    private:
        char32_t glyph_ { 0 };
        color_selector color_ {};
    };

    // 아이콘 하나다. 색은 팔레트 선택자로 받는다.
    class icon_element final : public luil::ui_element
    {
    public:
        icon_element(luil::ui_element_id id, char32_t glyph, color_selector color, float size = 14.0f);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;

    private:
        char32_t glyph_ { 0 };
        color_selector color_ {};
        float size_ { 14.0f };
    };

    // UI 글꼴이다. 글꼴이 없으면(test) 기본 typeface다.
    [[nodiscard]] SkFont ui_font(const luil::draw_context& context, float logical_size, bool bold = false);
    [[nodiscard]] SkFont code_font(const luil::draw_context& context, float logical_size);
    [[nodiscard]] SkFont icon_font(const luil::draw_context& context, float logical_size);
} // namespace cyella::ui
