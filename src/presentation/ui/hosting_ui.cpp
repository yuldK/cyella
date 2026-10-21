#include "presentation/ui/hosting_ui.h"
#include "presentation/hosting_intents.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui/ui_parts.h"
#include "presentation/ui_intents.h"

#include "luil/ui/stack_element.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace cyella::ui {
    namespace {
        constexpr float pane_header_height { 36.0f };
        constexpr float line_height { 18.0f };
        constexpr std::size_t check_line_limit { 8 };
        constexpr float action_height { 26.0f };
        constexpr float action_gap { 8.0f };
        constexpr float content_padding { 12.0f };
        constexpr float scroll_inset { 8.0f };

        const color_selector dim_color { [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } };

        color_selector ci_color(const ci_state state)
        {
            switch (state)
            {
            case ci_state::success:
                return [](const luil::ui_color_palette& palette) { return success_color(palette); };
            case ci_state::failure:
            case ci_state::error:
                return [](const luil::ui_color_palette& palette) { return palette.error_accent; };
            case ci_state::pending:
                return [](const luil::ui_color_palette& palette) { return status_color(palette, 'M'); };
            case ci_state::none:
                break;
            }
            return dim_color;
        }

        std::u8string ci_prefix(const ci_state state)
        {
            switch (state)
            {
            case ci_state::success:
                return u8"✓ ";
            case ci_state::failure:
                return u8"✗ ";
            case ci_state::pending:
                return u8"● ";
            case ci_state::error:
                return u8"! ";
            case ci_state::none:
                break;
            }
            return u8"– ";
        }

        std::unique_ptr<luil::ui_element> detail_header(std::u8string title, std::u8string tail, const bool drawer)
        {
            auto row = std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"pr-detail-header" },
                luil::stack_config { .direction = luil::stack_direction::row, .spacing = 6.0f, .padding = luil::edge_insets { 12.0f, 0.0f, 6.0f, 0.0f } });
            row->add(make_text(u8"pr-detail-title", title, text_style { .size = 13.0f, .bold = true }), text_button_width(title));
            row->add_flexible(make_text(u8"pr-detail-subtitle", std::move(tail), text_style { .size = 11.0f, .color = dim_color }));
            row->add(
                make_icon_button(u8"pr-detail-close", codicons::icon_close, drawer ? u8"상세 닫기" : u8"상세 닫기", message(hosting_detail_action_intent { hosting_detail_action::close })), 28.0f);
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"pr-detail-header-bar" }, bar_style { .divider_bottom = true }, std::move(row));
        }
    } // namespace

    std::unique_ptr<luil::ui_element> build_pull_request_detail(const pull_request_detail_view& detail, const bool drawer, const float width, const float scroll, feedback_sources& sources)
    {
        const std::u8string number { u8"#" + number_text(detail.number) };
        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"pr-detail" }, luil::stack_config {}) };
        column->add(detail_header(detail.abbreviation + u8" " + number, detail.state_text, drawer), pane_header_height);

        auto meta { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"pr-meta" }, luil::stack_config { .padding = luil::edge_insets { 12.0f, 8.0f, 12.0f, 6.0f } }) };
        float meta_height { 14.0f };
        const auto add = [&meta, &meta_height](std::unique_ptr<luil::ui_element> child, const float length) {
            meta_height += length;
            meta->add(std::move(child), length);
        };
        const float text_width { std::max(width - content_padding * 2.0f - scroll_inset - luil::scroll_area_bar_width, 120.0f) };
        const auto wrapped = [&](std::u8string owner, const std::u8string& text, const text_style& style, const float line) {
            add(make_wrapped_text(std::move(owner), text, style, line), wrapped_text_height(text, text_width, style, line));
        };
        wrapped(u8"pr-subject", detail.title.empty() ? std::u8string { u8"…" } : detail.title, text_style { .size = 14.0f, .bold = true }, 22.0f);
        if (detail.just_created)
            wrapped(u8"pr-created", u8"방금 만들었습니다: " + to_u8(detail.web_url), text_style { .size = 11.0f, .color = ci_color(ci_state::success) }, line_height);
        meta->add_gap(8.0f);
        meta_height += 8.0f;
        wrapped(u8"pr-branches", detail.source_branch + u8" → " + detail.target_branch + (detail.from_fork ? u8"  (fork)" : u8""), text_style { .size = 11.5f, .code = true }, line_height);
        if (detail.author.empty() == false)
            add(make_text(u8"pr-author", u8"작성자  " + detail.author, text_style { .size = 11.0f, .color = dim_color }), line_height);
        if (detail.updated_text.empty() == false)
            add(make_text(u8"pr-updated", u8"갱신  " + detail.updated_text, text_style { .size = 11.0f, .color = dim_color }), line_height);
        if (detail.head_sha.empty() == false)
            add(make_text(u8"pr-sha", u8"SHA  " + to_u8(detail.head_sha.substr(0, 12)), text_style { .size = 11.0f, .color = dim_color, .code = true }), line_height);
        add(make_text(u8"pr-ci", ci_prefix(detail.ci.state) + detail.ci.text, text_style { .size = 12.0f, .color = ci_color(detail.ci.state), .bold = true }), line_height + 4.0f);
        if (detail.ci.error.empty() == false)
            wrapped(u8"pr-ci-error", detail.ci.error, text_style { .size = 11.0f, .color = ci_color(ci_state::failure) }, line_height);
        std::size_t shown { 0 };
        for (const ci_check_view& check : detail.ci.checks)
        {
            if (shown >= check_line_limit)
            {
                add(make_text(u8"pr-check-more", u8"   외 " + number_text(detail.ci.checks.size() - shown) + u8"개", text_style { .size = 11.0f, .color = dim_color }), line_height);
                break;
            }
            add(make_text(u8"pr-check-" + number_text(shown), u8"   " + ci_prefix(check.state) + check.name, text_style { .size = 11.0f, .color = ci_color(check.state) }), line_height);
            ++shown;
        }
        meta->add_gap(12.0f);
        meta_height += 12.0f;
        add(make_text(u8"pr-body-label", u8"설명", text_style { .size = 11.0f, .color = dim_color, .bold = true }), 24.0f);
        wrapped(u8"pr-body", detail.body.empty() ? std::u8string { u8"작성한 설명이 없습니다." } : detail.body, text_style { .size = 12.0f, .color = dim_color }, line_height);

        luil::scroll_area_config config {};
        config.owner = u8"pr-scroll";
        config.content_height = meta_height;
        config.scroll_offset = scroll;
        config.scroll = [](const float delta) { return message(scroll_intent { .target = scroll_target::detail, .value = delta }); };
        config.scroll_to = [](const float offset) { return message(scroll_intent { .target = scroll_target::detail, .value = offset, .absolute = true }); };
        config.edges = luil::scroll_edges { .shadows = true };
        auto area { std::make_unique<luil::scroll_area_element>(std::move(config)) };
        area->set_content(std::move(meta));
        sources.detail_area = area.get();
        const luil::stack_config inset_config { .padding = luil::edge_insets { 0.0f, scroll_inset, scroll_inset, scroll_inset } };
        auto inset { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"pr-scroll-inset" }, inset_config) };
        inset->add_flexible(std::move(area));
        column->add_flexible(std::move(inset));

        // 본문이 길어도 체크아웃·브라우저·갱신은 고정된 바닥에 남는다.
        const bool stacked { width < 360.0f };
        const luil::stack_config actions_config { .spacing = action_gap, .padding = luil::edge_insets::all(content_padding) };
        auto actions { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"pr-actions-content" }, actions_config) };
        {
            const std::u8string label { detail.checked_out ? u8"'" + detail.local_branch + u8"'에 있음" : u8"로컬에 체크아웃 (" + detail.local_branch + u8")" };
            auto checkout = make_text_button(u8"pr-checkout", label, luil::text_button_visual::accent, message(hosting_detail_action_intent { hosting_detail_action::checkout }),
                detail.can_checkout && detail.checked_out == false, codicons::icon_git_pull_request_go_to_changes);
            checkout->set_tooltip(label);
            actions->add(std::move(checkout), action_height);
        }
        auto browser = make_text_button(u8"pr-open-browser", u8"브라우저에서 열기", luil::text_button_visual::normal, message(hosting_detail_action_intent { hosting_detail_action::open_browser }),
            detail.web_url.empty() == false, codicons::icon_link_external);
        auto refresh = make_text_button(u8"pr-refresh-ci", u8"CI 새로고침", luil::text_button_visual::normal, message(hosting_detail_action_intent { hosting_detail_action::refresh_ci }),
            detail.head_sha.empty() == false, codicons::icon_refresh);
        if (stacked)
        {
            actions->add(std::move(browser), action_height);
            actions->add(std::move(refresh), action_height);
        }
        else
        {
            const luil::stack_config row_config { .direction = luil::stack_direction::row, .spacing = action_gap };
            auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"pr-actions" }, row_config) };
            row->add_flexible(std::move(browser));
            row->add_flexible(std::move(refresh));
            actions->add(std::move(row), action_height);
        }
        const float action_count { stacked ? 3.0f : 2.0f };
        column->add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"pr-actions-bar" }, bar_style { .divider_top = true }, std::move(actions)),
            content_padding * 2.0f + action_height * action_count + action_gap * (action_count - 1.0f));
        return column;
    }

    std::unique_ptr<luil::ui_element> build_commit_ci_line(const ci_mark_view& ci)
    {
        // 커밋 상세의 "CI" 라벨 칸 옆에 서므로 값 앞의 "CI "는 뺀다 ("CI  ✓ 통과 · GitHub Actions").
        std::u8string_view state { ci.text };
        if (state.starts_with(u8"CI "))
            state.remove_prefix(3);
        std::u8string text { ci_prefix(ci.state) + std::u8string { state } };
        if (ci.source.empty() == false)
            text += u8" · " + ci.source;
        return make_text(u8"detail-ci", std::move(text), text_style { .size = 12.0f, .color = ci_color(ci.state) });
    }
} // namespace cyella::ui
