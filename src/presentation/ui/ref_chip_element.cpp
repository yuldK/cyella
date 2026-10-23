#include "presentation/ui/ref_chip_element.h"

#include "domain/text_catalog.h"
#include "presentation/dialog_intents.h"
#include "presentation/drop_hints.h"
#include "presentation/menu_model.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/graph_row_element.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkFont.h"
#include "include/core/SkRRect.h"
#include "include/core/SkRect.h"

#include <algorithm>

namespace cyella::ui {
    namespace {
        // 라벨 칩의 안쪽 여백·아이콘·간격이다 (`graph_row_element.cpp`의 `draw_ref_label`과 같은 값).
        constexpr float chip_padding { 6.0f };
        constexpr float chip_icon { 12.0f };
        constexpr float chip_gap { 4.0f };
        constexpr float chip_font { 11.0f };
        constexpr float chip_max_text { 140.0f };

        float glyph_width(const char32_t value) noexcept
        {
            if (value >= 0x1100)
                return 1.0f;
            switch (value)
            {
            case U'i':
            case U'j':
            case U'l':
            case U'I':
            case U'.':
            case U',':
            case U':':
            case U';':
            case U'!':
            case U'|':
            case U'\'':
                return 0.3f;
            case U'f':
            case U't':
            case U'r':
            case U'/':
            case U'-':
            case U' ':
            case U'(':
            case U')':
                return 0.4f;
            case U'm':
            case U'w':
            case U'M':
            case U'W':
                return 0.88f;
            default:
                break;
            }
            return value >= U'A' && value <= U'Z' ? 0.68f : 0.58f;
        }

        // UTF-8 글 한 줄을 코드 포인트로 훑는다.
        template<typename visitor>
        void for_each_code_point(const std::u8string_view text, visitor&& visit)
        {
            for (std::size_t index { 0 }; index < text.size();)
            {
                const auto lead { static_cast<std::uint8_t>(text[index]) };
                std::size_t length { 1 };
                char32_t value { lead };
                if (lead >= 0xF0)
                {
                    length = 4;
                    value = lead & 0x07u;
                }
                else if (lead >= 0xE0)
                {
                    length = 3;
                    value = lead & 0x0Fu;
                }
                else if (lead >= 0xC0)
                {
                    length = 2;
                    value = lead & 0x1Fu;
                }
                for (std::size_t offset { 1 }; offset < length && index + offset < text.size(); ++offset)
                    value = (value << 6) | (static_cast<std::uint8_t>(text[index + offset]) & 0x3Fu);
                visit(value);
                index += length;
            }
        }

        std::optional<menu_target> menu_target_of(const ref_label& label, const std::string& commit_sha)
        {
            menu_target target {};
            target.name = label.ref_name;
            target.sha = commit_sha;
            switch (label.kind)
            {
            case ref_label_kind::local:
                target.kind = menu_target_kind::local_branch;
                return target;
            case ref_label_kind::remote:
                target.kind = menu_target_kind::remote_branch;
                return target;
            case ref_label_kind::tag:
                target.kind = menu_target_kind::tag;
                return target;
            case ref_label_kind::head:
            case ref_label_kind::stash:
                break;
            }
            return std::nullopt;
        }

        // 두 번 눌러 여는 대상의 종류. 지역·원격 가지가 아니면 태그로 본다.
        ref_kind activated_ref_kind(const ref_label_kind kind) noexcept
        {
            if (kind == ref_label_kind::local)
                return ref_kind::local;
            if (kind == ref_label_kind::remote)
                return ref_kind::remote;
            return ref_kind::tag;
        }

        std::optional<drop_party> drop_party_of(const ref_label& label, const std::string& commit_sha)
        {
            if (label.kind == ref_label_kind::local)
                return drop_party { .kind = ref_kind::local, .name = label.ref_name, .sha = commit_sha, .is_commit = false };
            if (label.kind == ref_label_kind::remote)
                return drop_party { .kind = ref_kind::remote, .name = label.ref_name, .sha = commit_sha, .is_commit = false };
            return std::nullopt;
        }
    } // namespace

    float estimate_text_width(const std::u8string_view text, const float size) noexcept
    {
        float width { 0.0f };
        for_each_code_point(text, [&width](const char32_t value) { width += glyph_width(value); });
        return width * size * 1.03f;
    }

    std::vector<std::u8string> wrap_estimated(const std::u8string_view text, const float width, const float size)
    {
        std::vector<std::u8string> lines {};
        std::size_t line_start { 0 };
        std::size_t last_space { std::u8string_view::npos };
        float line_width { 0.0f };
        std::size_t index { 0 };
        while (index < text.size())
        {
            const auto lead { static_cast<std::uint8_t>(text[index]) };
            const std::size_t length { lead >= 0xF0 ? 4u : lead >= 0xE0 ? 3u : lead >= 0xC0 ? 2u : 1u };
            const std::u8string_view glyph { text.substr(index, length) };
            const float advance { estimate_text_width(glyph, size) };
            if (text[index] == u8'\n')
            {
                lines.emplace_back(text.substr(line_start, index - line_start));
                index += 1;
                line_start = index;
                last_space = std::u8string_view::npos;
                line_width = 0.0f;
                continue;
            }
            if (line_width + advance > width && index > line_start)
            {
                const std::size_t cut { last_space != std::u8string_view::npos && last_space > line_start ? last_space : index };
                lines.emplace_back(text.substr(line_start, cut - line_start));
                line_start = cut;
                while (line_start < text.size() && text[line_start] == u8' ')
                    ++line_start;
                index = line_start;
                last_space = std::u8string_view::npos;
                line_width = 0.0f;
                continue;
            }
            if (text[index] == u8' ')
                last_space = index;
            line_width += advance;
            index += length;
        }
        if (line_start < text.size() || lines.empty())
            lines.emplace_back(text.substr(line_start));
        return lines;
    }

    float estimate_ref_label_width(const ref_label& label) noexcept
    {
        float width { chip_padding * 2.0f + chip_icon + chip_gap + std::min(estimate_text_width(label.text, chip_font), chip_max_text) };
        if (label.also_remote)
            width += chip_gap + chip_icon;
        if (label.worktree_path.empty() == false)
            width += chip_gap + chip_icon;
        return width;
    }

    std::u8string ref_label_key(const ref_label& label)
    {
        switch (label.kind)
        {
        case ref_label_kind::local:
            return branch_drag_key(ref_kind::local, label.ref_name);
        case ref_label_kind::remote:
            return branch_drag_key(ref_kind::remote, label.ref_name);
        case ref_label_kind::tag:
            return u8"t:" + to_u8(label.ref_name);
        case ref_label_kind::stash:
            return u8"s:" + to_u8(label.ref_name);
        case ref_label_kind::head:
            break;
        }
        return u8"head";
    }

    // ---------------------------------------------------------------- ref_chip_element

    ref_chip_element::ref_chip_element(ref_label label, std::u8string row_key, std::string commit_sha, const std::uint8_t lane_color, const bool row_selected)
        : press_row_element { luil::ui_element_id { kind_ref_chip, std::u8string { ref_label_owner_prefix } + ref_label_key(label) } }
        , label_ { std::move(label) }
        , lane_color_ { lane_color }
    {
        set_tab_stop(false);
        set_tooltip(label_.also_remote ? label_.text + u8" · " + label_.remote_name : label_.text);
        if (label_.worktree_path.empty() == false)
            set_tooltip(tooltip() + tr(u8"ref_chip.checked_out_in_worktree_suffix", { { u8"path", std::u8string { label_.worktree_path.begin(), label_.worktree_path.end() } } }));
        access_name_ = label_.text + (label_.local_only ? tr(u8"ref_chip.not_on_remote_suffix") : std::u8string {})
            + (label_.worktree_path.empty() ? std::u8string {} : tr(u8"ref_chip.in_other_worktree_suffix"));
        // 칩을 눌러도 행을 누른 것과 같다 (Ctrl·Shift는 여러 커밋 고르기). 보조 기술의 "실행"은 그냥 누름이다.
        set_press([row_key](const luil::ui_action_context& context) -> std::vector<luil::input_action> {
            return { message(select_commit_intent { .key = row_key, .control = context.control, .shift = context.shift }) };
        });
        set_access_handler([row_key](const luil::access_request& request) -> std::optional<std::vector<luil::input_action>> {
            if (request.command != luil::access_command::invoke)
                return std::nullopt;
            return std::vector<luil::input_action> { message(select_commit_intent { .key = row_key }) };
        });

        switch (label_.kind)
        {
        case ref_label_kind::local:
        case ref_label_kind::remote:
        case ref_label_kind::tag:
            set_action(luil::ui_trigger::double_click, action_of(message(activate_ref_intent { activated_ref_kind(label_.kind), label_.ref_name })));
            break;
        case ref_label_kind::head:
        case ref_label_kind::stash:
            break;
        }

        // 고르지 않은 행의 라벨을 우클릭하면 그 행을 먼저 고른다. 메뉴는 라벨(브랜치·태그)의 것이다.
        if (const std::optional<menu_target> target { menu_target_of(label_, commit_sha) }; target.has_value())
            set_action(luil::ui_trigger::right_click, [menu = *target, row_key, row_selected](const luil::ui_action_context& context) -> std::vector<luil::input_action> {
                std::vector<luil::input_action> actions {};
                if (row_selected == false)
                    actions.push_back(message(select_commit_intent { .key = row_key }));
                actions.push_back(message(open_context_menu_intent { menu, context.x, context.y }));
                return actions;
            });

        const std::optional<drop_party> party { drop_party_of(label_, commit_sha) };
        if (party.has_value())
        {
            // 브랜치 라벨은 끌 수 있고, 다른 브랜치 라벨이 놓일 수 있다.
            luil::drag_source source {};
            source.make_payload = [key = ref_label_key(label_), text = label_.text, id = id()](const luil::ui_action_context&) {
                luil::drag_payload payload {};
                payload.source = id;
                payload.dragged_owner = key;
                payload.label = text;
                payload.rejected_cursor = luil::ui_cursor::not_allowed;
                return payload;
            };
            set_drag_source(std::move(source));

            luil::drop_target target {};
            target.accepts = [destination = *party](const luil::drag_payload& payload) {
                const std::optional<drop_party> source { parse_branch_drag_key(payload.dragged_owner) };
                return source.has_value() && can_drop_on(*source, destination);
            };
            target.on_drop = [destination = *party](const luil::drag_payload& payload, const luil::ui_action_context& context) -> std::vector<luil::input_action> {
                const std::optional<drop_party> source { parse_branch_drag_key(payload.dragged_owner) };
                if (source.has_value() == false)
                    return {};
                return { message(drop_intent { *source, destination, context.x, context.y }) };
            };
            set_drop_target(std::move(target));
            set_cursor(luil::ui_cursor::grab);
        }
        else
            set_cursor(luil::ui_cursor::hand);
    }

    void ref_chip_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    luil::access_info ref_chip_element::accessibility() const
    {
        return { .role = luil::access_role::button, .name = access_name_ };
    }

    void ref_chip_element::draw(luil::draw_context& context, const luil::interaction_snapshot& interaction) const
    {
        const luil::ui_color lane { lane_color(context.palette, lane_color_) };
        draw_ref_label(context, label_, bounds(), lane);
        // 끌어 놓을 자리로 가리키는 동안은 luil이 테두리를 그린다. 여기서는 포인터를 올린 것만 살짝 밝힌다.
        if (interaction.hovered == id() && interaction.drag.has_value() == false)
        {
            const luil::rect_f box { bounds() };
            const float radius { context.metrics.control_corner_radius * context.scale };
            context.canvas.drawRRect(
                SkRRect::MakeRectXY(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), radius, radius), luil::solid_paint(luil::with_alpha(context.palette.primary_foreground, 0.08f)));
        }
    }

    // ---------------------------------------------------------------- ref_overflow_element

    ref_overflow_element::ref_overflow_element(std::u8string owner, const std::size_t hidden, std::string sha)
        : ui_element { luil::ui_element_id { kind_ref_overflow, std::move(owner) } }
        , text_ { u8"+" + number_text(hidden) }
    {
        set_cursor(luil::ui_cursor::hand);
        set_action(luil::ui_trigger::left_click, [this, sha = std::move(sha)](const luil::ui_action_context&) -> std::vector<luil::input_action> {
            const luil::rect_f box { bounds() };
            return { message(open_ref_list_intent { sha, box.x, box.y + box.height }) };
        });
    }

    void ref_overflow_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    luil::access_info ref_overflow_element::accessibility() const
    {
        return { .role = luil::access_role::button, .name = tr(u8"ref_chip.overflow_access_name", { { u8"count", text_ } }) };
    }

    void ref_overflow_element::draw(luil::draw_context& context, const luil::interaction_snapshot&) const
    {
        const luil::rect_f box { bounds() };
        const float scale { context.scale };
        const float radius { context.metrics.control_corner_radius * scale };
        context.canvas.drawRRect(SkRRect::MakeRectXY(SkRect::MakeXYWH(box.x, box.y, box.width, box.height), radius, radius), luil::solid_paint(context.palette.input_background));
        const SkFont font { ui_font(context, chip_font) };
        static_cast<void>(luil::draw_text_within(context.canvas, text_, box.x + 5.0f * scale, box.y + luil::centered_text_baseline(font, box.height), box.width - 5.0f * scale, font,
            luil::solid_paint(context.palette.secondary_foreground)));
    }
} // namespace cyella::ui
