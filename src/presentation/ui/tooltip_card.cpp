#include "presentation/ui/tooltip_card.h"

#include "luil/ui/draw_primitives.h"
#include "luil/ui/ui_tree.h"

#include "include/core/SkPaint.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace cyella::ui {
    namespace {
        constexpr float font_size { 12.0f };
        constexpr float line_height { 17.0f };
        // 제목과 설명 줄 사이를 조금 띄워 제목이 한 덩이로 읽히게 한다.
        constexpr float title_gap { 3.0f };
        constexpr float card_padding { 8.0f };
        constexpr std::size_t short_text_bytes { 32 };

        [[nodiscard]] luil::text_layout layout_line(std::u8string_view text, const float width, const bool bold)
        {
            luil::text_layout_request request {
                .text = std::u8string { text },
                .font_size = font_size,
                .width = width,
                .wrap = true,
                .line_height = line_height,
            };
            if (bold)
                request.styles.push_back({ .begin = 0, .end = text.size(), .bold = true });
            return luil::make_text_layout(std::move(request));
        }

        // 폭을 넘는 경로 줄을 `/`·`\` 바로 뒤에서 끊는다. luil의 줄 바꿈은 공백이 없으면 글자 사이에서 끊어
        // `.../g` `oing/...`처럼 이름이 쪼개진다. 구분자 사이 한 마디가 폭보다 길면 그 마디는 luil이 접는다.
        [[nodiscard]] std::vector<std::u8string_view> split_path(const std::u8string_view text, const float width, const bool bold)
        {
            const auto separator = [](const char8_t value) { return value == u8'/' || value == u8'\\'; };
            if (std::ranges::none_of(text, separator))
                return { text };
            luil::text_layout_request request { .text = std::u8string { text }, .font_size = font_size };
            if (bold)
                request.styles.push_back({ .begin = 0, .end = text.size(), .bold = true });
            const luil::text_layout whole { luil::make_text_layout(std::move(request)) };
            if (whole.size().width <= width)
                return { text };

            std::vector<std::u8string_view> pieces {};
            std::size_t start { 0 };
            float start_x { 0.0f };
            while (whole.size().width - start_x > width)
            {
                std::size_t cut { 0 };
                float cut_x { 0.0f };
                for (const luil::text_layout_cluster& cluster : whole.clusters())
                {
                    if (cluster.begin < start)
                        continue;
                    if (cluster.right - start_x > width - 1.0f)
                        break;
                    if (separator(text[cluster.end - 1]))
                    {
                        cut = cluster.end;
                        cut_x = cluster.right;
                    }
                }
                if (cut <= start || cut >= text.size())
                    break;
                pieces.push_back(text.substr(start, cut - start));
                start = cut;
                start_x = cut_x;
            }
            pieces.push_back(text.substr(start));
            return pieces;
        }
    } // namespace

    // ---------------------------------------------------------------- tooltip_card_element

    tooltip_card_element::tooltip_card_element(const std::u8string_view text, const float width)
        : ui_element { luil::ui_element_id { luil::ui_element_kind::root, u8"tooltip-card" } }
        , text_ { text }
    {
        const bool several { text.find(u8'\n') != std::u8string_view::npos };
        float top { 0.0f };
        std::size_t paragraphs { 0 };
        std::size_t begin { 0 };
        while (begin <= text.size())
        {
            const std::size_t end { std::min(text.find(u8'\n', begin), text.size()) };
            const std::u8string_view part { text.substr(begin, end - begin) };
            begin = end + 1;
            // 빈 줄은 그리지 않는다 — 조립하는 쪽이 비어 있을 수 있는 칸을 그대로 이은 경우다.
            if (part.empty())
                continue;
            const bool title { paragraphs == 0 };
            const bool bold { title && several };
            if (paragraphs == 1)
                top += title_gap;
            ++paragraphs;
            for (const std::u8string_view piece : split_path(part, width, bold))
            {
                luil::text_layout layout { layout_line(piece, width, bold) };
                const luil::text_layout_size measured { layout.size() };
                size_.width = std::max(size_.width, std::ceil(measured.width));
                lines_.push_back({ std::move(layout), top, title });
                top += measured.height;
            }
        }
        size_.height = std::ceil(top);
    }

    void tooltip_card_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    void tooltip_card_element::draw(luil::draw_context& context, const luil::interaction_snapshot&) const
    {
        // 툴팁 안의 그리기 배율은 1이고 캔버스가 표면 배율을 이미 얹었다.
        const SkPaint title { luil::solid_paint(context.palette.primary_foreground) };
        const SkPaint detail { luil::solid_paint(context.palette.secondary_foreground) };
        for (const line& value : lines_)
            luil::draw_text_layout(context.canvas, value.text, { bounds().x, bounds().y + value.top }, value.title ? title : detail);
    }

    luil::access_info tooltip_card_element::accessibility() const
    {
        return { .role = luil::access_role::static_text, .name = text_ };
    }

    std::optional<luil::tooltip_config> make_tooltip_card(const std::u8string_view text)
    {
        // 한 줄에 다 드는 글은 luil의 한 줄 툴팁이 더 가볍다. 카드는 줄이 둘 이상일 때만 쓴다.
        //  - 모든 단추·라벨의 툴팁이 매 frame 여기를 지나므로 짧은 글은 재지 않는다.
        //    가장 넓은 라틴 글자(약 11px)로 채워도 `tooltip_card_width`에 드는 길이다.
        const bool single { text.find(u8'\n') == std::u8string_view::npos };
        if (text.empty() || (single && text.size() <= short_text_bytes))
            return std::nullopt;
        if (single && luil::make_text_layout({ .text = std::u8string { text }, .font_size = font_size }).size().width <= tooltip_card_width)
            return std::nullopt;

        auto content { std::make_unique<tooltip_card_element>(text, tooltip_card_width) };
        const luil::size_f size { content->size() };
        if (size.width <= 0.0f || size.height <= 0.0f)
            return std::nullopt;
        return luil::tooltip_config {
            .content = std::make_shared<const luil::ui_tree>(luil::make_arranged_tree(std::move(content), { 0.0f, 0.0f, size.width, size.height }, 1.0f)),
            .padding = card_padding,
        };
    }

    void set_text_tooltip(luil::ui_element& element, std::u8string text)
    {
        if (auto card { make_tooltip_card(text) }; card.has_value())
            element.set_tooltip_content(std::move(*card));
        element.set_tooltip(std::move(text));
    }
} // namespace cyella::ui
