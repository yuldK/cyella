// 툴팁 글을 한 줄 툴팁과 카드 중 어디에 그릴지, 카드가 폭 안에서 접히는지를 본다. 보조 기술이 읽는 글(`tooltip()`)은
// 카드를 달아도 원문 그대로여야 한다.
#include "presentation/ui/tooltip_card.h"

#include "luil/ui/ui_tree.h"

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace cyella;

namespace {
    class probe final : public luil::ui_element
    {
    public:
        probe()
            : ui_element { luil::ui_element_id { luil::ui_element_kind::root, u8"probe" } }
        {}

        void arrange(const luil::arrange_context& context) override
        {
            set_bounds(context.slot);
        }

        void draw(luil::draw_context&, const luil::interaction_snapshot&) const override
        {}
    };

    [[nodiscard]] luil::rect_f content_bounds(const luil::tooltip_config& config)
    {
        REQUIRE(config.content != nullptr);
        REQUIRE(config.content->root() != nullptr);
        return config.content->root()->bounds();
    }
} // namespace

TEST_CASE("short single-line tooltips stay on the plain luil tooltip", "[presentation][tooltip]")
{
    CHECK_FALSE(ui::make_tooltip_card(u8"가져오기").has_value());
    CHECK_FALSE(ui::make_tooltip_card(u8"origin/main").has_value());
    CHECK_FALSE(ui::make_tooltip_card(u8"").has_value());

    probe target {};
    ui::set_text_tooltip(target, u8"가져오기");
    CHECK(target.tooltip() == u8"가져오기");
    CHECK(target.tooltip_content() == nullptr);
}

TEST_CASE("multi-line tooltips become a card no wider than the card width", "[presentation][tooltip]")
{
    const std::u8string text { u8"codex/branch-overflow-refresh\nupstream 없음\n다른 worktree에서 체크아웃됨\nC:/Users/Lusain/.codex/worktrees/branch-overflow-refresh/cyella" };
    const auto card { ui::make_tooltip_card(text) };
    REQUIRE(card.has_value());
    const luil::rect_f bounds { content_bounds(*card) };
    CHECK(bounds.width > 0.0f);
    CHECK(bounds.width <= ui::tooltip_card_width);
    // 줄마다 한 줄씩 선다 (이어 붙여 한 줄로 그리지 않는다).
    CHECK(bounds.height >= 17.0f * 4.0f);

    probe target {};
    ui::set_text_tooltip(target, text);
    CHECK(target.tooltip() == text);
    CHECK(target.tooltip_content() != nullptr);
}

TEST_CASE("a long single line wraps inside a card instead of running off the window", "[presentation][tooltip]")
{
    const std::u8string path { u8"C:/Users/Lusain/.codex/worktrees/branch-overflow-refresh/cyella/src/presentation/ui/repository_tabs_element.cpp" };
    const auto card { ui::make_tooltip_card(path) };
    REQUIRE(card.has_value());
    const luil::rect_f bounds { content_bounds(*card) };
    CHECK(bounds.width <= ui::tooltip_card_width);
    CHECK(bounds.height >= 17.0f * 2.0f);
}

TEST_CASE("empty lines in a composed tooltip are dropped from the card", "[presentation][tooltip]")
{
    const auto spaced { ui::make_tooltip_card(u8"main\n\nupstream: origin/main\n") };
    const auto packed { ui::make_tooltip_card(u8"main\nupstream: origin/main") };
    REQUIRE(spaced.has_value());
    REQUIRE(packed.has_value());
    CHECK(content_bounds(*spaced).height == content_bounds(*packed).height);
}
