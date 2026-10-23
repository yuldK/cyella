#pragma once

#include "presentation/menu_model.h"
#include "presentation/ui/press_row.h"
#include "presentation/view_snapshot.h"

#include "luil/ui/ui_element.h"
#include "luil/ui/ui_events.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cyella::ui {
    // 목록 행 안에 그리는 내용 element들이다. 고르기·hover·커서 표시는 luil 목록의 행이 그리고, 이 element들은
    // 그 안의 글·아이콘·작은 단추만 맡는다. 누름을 받지 않는 내용은 `hit_opaque(false)`라 행의 고르기를 가로채지 않는다.

    inline constexpr float sidebar_row_height { 24.0f };
    inline constexpr float sidebar_header_row_height { 28.0f };
    inline constexpr float file_row_height { 24.0f };
    inline constexpr float diff_row_height { 18.0f };
    inline constexpr float log_row_height { 20.0f };
    inline constexpr float start_row_height { 56.0f };

    // 사이드바 행이다. 브랜치 행은 두 번 누르면 체크아웃이고 우클릭하면 메뉴이며 끌어서 다른 브랜치·커밋에 놓을 수 있어,
    // 그 행만 스스로 누름을 받는다 — 목록 행에는 두 번 누름·우클릭·끌기 자리가 없다. 그래서 누름도 같은 고르기
    // 메시지를 누른 순간 다시 낸다 (`press_row_element`). 머리 줄의 누름은 접고 펴기라 평소의 클릭이다.
    // `selected`는 목록이 고른 행으로 그리는 행이다. 아니면 우클릭이 메뉴를 열기 전에 그 행을 고른다.
    class sidebar_row_element final : public press_row_element
    {
    public:
        sidebar_row_element(sidebar_row_view row, std::optional<luil::input_action> select, std::optional<luil::input_action> activate, std::optional<menu_target> menu, bool selected = false);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        sidebar_row_view row_ {};
    };

    struct file_row_button
    {
        std::u8string owner {};
        char32_t glyph { 0 };
        std::u8string tooltip {};
        luil::input_action action {};
        bool enabled { true };
        // 두 번 눌러야 실행한다 (되돌릴 수 없는 동작의 임시 안전장치).
        bool double_click { false };
    };

    [[nodiscard]] inline file_row_button make_row_button(
        std::u8string owner, const char32_t glyph, std::u8string tooltip, luil::input_action action, const bool enabled = true, const bool double_click = false)
    {
        file_row_button button {};
        button.owner = std::move(owner);
        button.glyph = glyph;
        button.tooltip = std::move(tooltip);
        button.action = std::move(action);
        button.enabled = enabled;
        button.double_click = double_click;
        return button;
    }

    // 작업 영역·커밋 상세의 파일 행이다. 오른쪽 끝의 작은 단추(stage·unstage·discard)가 먼저 누름을 받는다.
    class file_row_element final : public press_row_element
    {
    public:
        // `select`는 행을 누른 순간의 메시지(고르기, 수식 키를 받는다)이고 `menu`는 우클릭 메뉴의 대상이다. 둘 중 하나라도 있으면 이
        // element가 눌림을 직접 받으므로 목록 행의 고르기를 다시 낸다.
        // `tree_depth`는 변경 파일 트리의 들여쓰기 단계다. 목록 보기는 값이 없다.
        // `selected`는 고른 파일(여러 파일 선택 포함)이다. 아니면 우클릭이 메뉴를 열기 전에 이 파일만 고른다.
        file_row_element(file_row_view row, std::vector<file_row_button> buttons, luil::ui_action select = {}, std::optional<menu_target> menu = std::nullopt,
            std::optional<std::size_t> tree_depth = std::nullopt, bool selected = false);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        file_row_view row_ {};
        std::size_t button_count_ { 0 };
        std::optional<std::size_t> tree_depth_ {};
    };

    // 변경 파일 트리의 폴더 행이다 (› 폴더 이름 · 파일 수). 누르면 접거나 편다. 키보드 훑기는 이 행을 건너뛴다.
    class folder_row_element final : public luil::ui_element
    {
    public:
        folder_row_element(file_tree_row row, luil::input_action toggle);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        file_tree_row row_ {};
    };

    // 작업 영역 목록의 묶음 머리다 (Unstaged 변경 4 [버리기 아이콘] [모두 stage]).
    //  - `button`은 오른쪽 끝의 글 단추이고, `icon`은 그 왼쪽의 아이콘 단추다 (모두 버리기).
    class file_group_element final : public luil::ui_element
    {
    public:
        file_group_element(std::u8string owner, std::u8string title, std::size_t count, std::optional<file_row_button> button, std::optional<file_row_button> icon = std::nullopt);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        std::u8string title_ {};
        std::size_t count_ { 0 };
        luil::ui_element* button_ { nullptr };
        luil::ui_element* icon_ { nullptr };
        float button_width_ { 96.0f };
    };

    // diff 임시 본문의 한 줄이다 (웹뷰를 쓸 수 없을 때의 대체 보기).
    class diff_row_element final : public luil::ui_element
    {
    public:
        diff_row_element(std::u8string owner, diff_line_view line);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        diff_line_view line_ {};
    };

    class log_row_element final : public luil::ui_element
    {
    public:
        explicit log_row_element(log_line_view line);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        log_line_view line_ {};
    };

    // 시작 페이지의 저장소 한 줄이다. 줄을 누르면 열고, 오른쪽 단추로 고정·지우기를 한다.
    class start_row_element final : public luil::ui_element
    {
    public:
        start_row_element(std::u8string owner, start_page_item item, bool open, std::u8string when, luil::input_action activate, std::vector<file_row_button> buttons);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        start_page_item item_ {};
        bool open_ { false };
        std::u8string when_ {};
    };

    // 시작 페이지 위의 행동 카드다 (저장소 열기 · Clone · Init).
    class start_card_element final : public luil::ui_element
    {
    public:
        start_card_element(std::u8string owner, char32_t glyph, std::u8string title, std::u8string description, luil::input_action action, bool enabled);

        void arrange(const luil::arrange_context& context) override;
        void draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const override;
        [[nodiscard]] luil::access_info accessibility() const override;

    private:
        char32_t glyph_ { 0 };
        std::u8string title_ {};
        std::u8string description_ {};
    };
} // namespace cyella::ui
