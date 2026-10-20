#include "presentation/ui/hosting_ui.h"
#include "presentation/hosting_intents.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"

#include "luil/ui/stack_element.h"

#include <algorithm>
#include <memory>
#include <utility>

namespace cyella::ui {
    namespace {
        constexpr float pane_header_height { 36.0f };
        constexpr float line_height { 18.0f };
        constexpr std::size_t body_line_limit { 8 };
        constexpr std::size_t check_line_limit { 8 };

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

    std::unique_ptr<luil::ui_element> build_pull_request_detail(const pull_request_detail_view& detail, const bool drawer)
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
        add(make_text(u8"pr-subject", detail.title.empty() ? std::u8string { u8"…" } : detail.title, text_style { .size = 14.0f, .bold = true }), 24.0f);
        if (detail.just_created)
            add(make_text(u8"pr-created", u8"방금 만들었습니다: " + to_u8(detail.web_url), text_style { .size = 11.0f, .color = ci_color(ci_state::success) }), line_height);
        add(make_text(u8"pr-branches", detail.source_branch + u8" → " + detail.target_branch + (detail.from_fork ? u8"  (fork)" : u8""), text_style { .size = 12.0f, .code = true }),
            line_height + 2.0f);
        add(make_text(u8"pr-state", u8"상태  " + detail.state_text, text_style { .size = 11.0f, .color = dim_color }), line_height);
        if (detail.author.empty() == false)
            add(make_text(u8"pr-author", u8"작성자  " + detail.author, text_style { .size = 11.0f, .color = dim_color }), line_height);
        if (detail.updated_text.empty() == false)
            add(make_text(u8"pr-updated", u8"갱신  " + detail.updated_text, text_style { .size = 11.0f, .color = dim_color }), line_height);
        if (detail.head_sha.empty() == false)
            add(make_text(u8"pr-sha", u8"SHA  " + to_u8(detail.head_sha.substr(0, 12)), text_style { .size = 11.0f, .color = dim_color, .code = true }), line_height);
        add(make_text(u8"pr-ci", ci_prefix(detail.ci.state) + detail.ci.text, text_style { .size = 12.0f, .color = ci_color(detail.ci.state), .bold = true }), line_height + 4.0f);
        if (detail.ci.error.empty() == false)
            add(make_text(u8"pr-ci-error", detail.ci.error, text_style { .size = 11.0f, .color = ci_color(ci_state::failure) }), line_height);
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
        std::size_t line_index { 0 };
        std::size_t start { 0 };
        while (start < detail.body.size() && line_index < body_line_limit)
        {
            std::size_t end { detail.body.find(u8'\n', start) };
            if (end == std::u8string::npos)
                end = detail.body.size();
            std::u8string line { detail.body.substr(start, end - start) };
            if (line.empty() == false && line.back() == u8'\r')
                line.pop_back();
            add(make_text(u8"pr-body-" + number_text(line_index), std::move(line), text_style { .size = 12.0f }), line_height);
            ++line_index;
            start = end + 1;
        }

        // 동작 단추는 UI Automation으로도 눌린다 (Invoke).
        {
            auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"pr-actions" }, luil::stack_config { .direction = luil::stack_direction::row, .spacing = 6.0f }) };
            row->add(make_text_button(u8"pr-open-browser", u8"브라우저에서 열기", luil::text_button_visual::normal, message(hosting_detail_action_intent { hosting_detail_action::open_browser }),
                         detail.web_url.empty() == false, codicons::icon_link_external),
                { .length = 150.0f, .cross_length = 24.0f });
            row->add_flexible_gap();
            add(std::move(row), 30.0f);
        }
        {
            auto row { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"pr-actions-2" }, luil::stack_config { .direction = luil::stack_direction::row, .spacing = 6.0f }) };
            const std::u8string label { detail.checked_out ? u8"'" + detail.local_branch + u8"'에 있음" : u8"로컬에 체크아웃 (" + detail.local_branch + u8")" };
            row->add(make_text_button(u8"pr-checkout", label, luil::text_button_visual::accent, message(hosting_detail_action_intent { hosting_detail_action::checkout }),
                         detail.can_checkout && detail.checked_out == false, codicons::icon_git_pull_request_go_to_changes),
                { .length = std::max(210.0f, text_button_width(label, true)), .cross_length = 24.0f });
            row->add(make_text_button(u8"pr-refresh-ci", u8"CI 새로고침", luil::text_button_visual::normal, message(hosting_detail_action_intent { hosting_detail_action::refresh_ci }),
                         detail.head_sha.empty() == false, codicons::icon_refresh),
                { .length = 120.0f, .cross_length = 24.0f });
            row->add_flexible_gap();
            add(std::move(row), 30.0f);
        }
        column->add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"pr-meta-bar" }, bar_style { .divider_bottom = true }, std::move(meta)), meta_height);
        column->add_flexible_gap();
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
