#include "presentation/ui/diff_webview.h"

#include "application/diff_view_protocol.h"
#include "presentation/diff_view_intents.h"
#include "presentation/ui/diff_special_card.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/list_rows.h"
#include "presentation/ui/segmented_control.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"

#include "luil/ui/badge_element.h"
#include "luil/ui/choice_group_element.h"
#include "luil/ui/stack_element.h"
#include "luil/ui/virtual_list_element.h"
#include "luil/ui/webview_element.h"

#include <utility>

namespace cyella::ui {
    namespace {
        constexpr float diff_header_height { 32.0f };
        constexpr float layout_toggle_width { 150.0f };
        constexpr float image_mode_toggle_width { 220.0f };
        constexpr float truncated_note_width { 150.0f };
        constexpr float fallback_note_width { 250.0f };
        constexpr float textconv_badge_width { 70.0f };

        const color_selector dim_color { [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } };

        std::unique_ptr<luil::ui_element> panel_surface(std::u8string owner, std::unique_ptr<luil::ui_element> content)
        {
            return std::make_unique<bar_element>(
                luil::ui_element_id { kind_panel, std::move(owner) }, bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.surface_background; } }, std::move(content));
        }

        std::unique_ptr<luil::ui_element> centered_note(std::u8string owner, std::u8string text)
        {
            auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, owner + u8"-column" }, luil::stack_config { .padding = luil::edge_insets::all(16.0f) }) };
            column->add_flexible_gap();
            column->add(make_text(std::move(owner), std::move(text), text_style { .size = 12.0f, .color = dim_color, .align = 1 }), 24.0f);
            column->add_flexible_gap();
            return column;
        }

        std::unique_ptr<luil::ui_element> layout_toggle(const diff_surface_state& surface)
        {
            luil::choice_group_config config {};
            config.owner = u8"diff-layout";
            config.style = luil::choice_style::toggle;
            config.name = u8"diff 보기";
            config.items = { { u8"inline", u8"Inline" }, { u8"split", u8"좌우 분할" } };
            config.selected = surface.layout == diff_layout::split ? u8"split" : u8"inline";
            config.select = [](const std::u8string& value) { return message(set_diff_layout_intent { value == u8"split" ? diff_layout::split : diff_layout::inline_view }); };
            return make_segmented_control(std::move(config));
        }

        std::unique_ptr<luil::ui_element> image_mode_toggle(const diff_surface_state& surface)
        {
            luil::choice_group_config config {};
            config.owner = u8"diff-image-mode";
            config.style = luil::choice_style::toggle;
            config.name = u8"이미지 비교 방식";
            config.items = { { u8"side_by_side", u8"좌우" }, { u8"onion_skin", u8"겹치기" }, { u8"swipe", u8"밀기" } };
            switch (surface.image_mode)
            {
            case image_compare_mode::side_by_side:
                config.selected = u8"side_by_side";
                break;
            case image_compare_mode::onion_skin:
                config.selected = u8"onion_skin";
                break;
            case image_compare_mode::swipe:
                config.selected = u8"swipe";
                break;
            }
            config.select = [](const std::u8string& value) {
                image_compare_mode mode { image_compare_mode::side_by_side };
                if (value == u8"onion_skin")
                    mode = image_compare_mode::onion_skin;
                else if (value == u8"swipe")
                    mode = image_compare_mode::swipe;
                return message(set_diff_image_mode_intent { mode });
            };
            return make_segmented_control(std::move(config));
        }

        std::unique_ptr<luil::ui_element> build_header(const diff_panel_view& diff)
        {
            luil::stack_config row_config { .direction = luil::stack_direction::row, .spacing = 6.0f, .padding = luil::edge_insets { 4.0f, 0.0f, 10.0f, 0.0f } };
            row_config.cross_alignment = luil::stack_alignment::center;
            auto header { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"diff-header" }, row_config) };
            header->add(make_icon_button(u8"diff-close", codicons::icon_arrow_left, u8"그래프로 돌아가기", message(close_diff_intent {})), 30.0f);
            header->add(make_text(u8"diff-title", diff.title, text_style { .size = 12.0f, .bold = true }), text_button_width(diff.title));
            header->add_flexible(make_text(u8"diff-subtitle", diff.subtitle, text_style { .size = 11.0f, .color = dim_color }));
            // 텍스트 변환(textconv)을 거친 diff라는 표시다. 변환 결과를 비교하므로 파일 내용과 줄이 다를 수 있다.
            if (diff.surface.textconv_driver.empty() == false)
            {
                luil::badge_config badge { .text = u8"textconv", .tone = luil::badge_tone::accent };
                header->add(std::make_unique<luil::badge_element>(luil::ui_element_id { kind_label, u8"diff-textconv-badge" }, badge),
                    { .length = textconv_badge_width, .cross_length = luil::badge_element::height_for(badge) });
            }

            if (diff.use_webview == false && diff.special.has_value() == false)
                header->add(make_text(u8"diff-fallback-note", u8"WebView2 없이 텍스트로 보는 중", text_style { .size = 11.0f, .color = dim_color, .align = 2 }), fallback_note_width);
            else
            {
                if (diff.surface.truncated)
                    header->add(make_text(u8"diff-truncated-note", u8"큰 diff의 앞부분만 보임", text_style { .size = 11.0f, .color = dim_color, .align = 2 }), truncated_note_width);
                if (diff.surface.content == diff_content::text)
                    header->add(layout_toggle(diff.surface), { .length = layout_toggle_width, .cross_length = luil::choice_toggle_height });
                else if (diff.surface.content == diff_content::image)
                    header->add(image_mode_toggle(diff.surface), { .length = image_mode_toggle_width, .cross_length = luil::choice_toggle_height });
            }
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"diff-header-bar" }, bar_style { .divider_bottom = true }, std::move(header));
        }

        // 웹뷰가 앉을 자리다. 웹뷰가 서면 이 자리가 구멍으로 비워져 페이지가 보이고, 서지 못하면 placeholder 글이 남는다.
        std::unique_ptr<luil::ui_element> build_webview_slot(const diff_panel_view& diff)
        {
            luil::webview_config config {};
            config.webview = std::u8string { diff_webview_id };
            config.name = u8"diff 뷰어";
            config.placeholder = diff.loading ? u8"diff를 읽는 중…" : u8"diff 뷰어를 여는 중…";
            return std::make_unique<luil::webview_element>(luil::ui_element_id { luil::ui_element_kind::webview, std::u8string { diff_webview_id } }, std::move(config));
        }

        // 웹뷰를 쓸 수 없을 때의 본문이다. 줄 목록을 그대로 보인다 (색·hunk 머리뿐이고 stage 단추는 없다).
        std::unique_ptr<luil::ui_element> build_text_body(const diff_panel_view& diff, feedback_sources& sources)
        {
            if (diff.loading)
                return centered_note(u8"diff-loading", u8"diff를 읽는 중…");
            if (diff.lines.empty())
                return centered_note(
                    u8"diff-empty", diff.binary ? std::u8string { u8"바이너리 파일입니다. 이미지·바이너리 비교에는 WebView2 Runtime이 필요합니다." } : std::u8string { u8"보일 변경이 없습니다." });
            auto lines { std::make_shared<std::vector<diff_line_view>>(diff.lines) };
            luil::virtual_list_config config {};
            config.owner = u8"diff-lines";
            config.items.reserve(lines->size());
            for (std::size_t index { 0 }; index < lines->size(); ++index)
                config.items.push_back({ .key = u8"d" + number_text(index) });
            config.row_height = diff_row_height;
            config.scroll_offset = diff.scroll;
            config.overscan = 6;
            config.build_row = [lines](const luil::virtual_list_item& item, const std::size_t index, bool) -> std::unique_ptr<luil::ui_element> {
                return std::make_unique<diff_row_element>(u8"line-" + item.key, (*lines)[index]);
            };
            config.scroll = [](const float delta) { return message(scroll_intent { .target = scroll_target::diff, .value = delta }); };
            config.scroll_to = [](const float offset) { return message(scroll_intent { .target = scroll_target::diff, .value = offset, .absolute = true }); };
            auto list { std::make_unique<luil::virtual_list_element>(std::move(config)) };
            list->set_access_name(u8"diff");
            sources.diff = list.get();
            return list;
        }

        // 대소문자와 구분자 모양(`\`, `/`)을 무시하고 비교하는 글자다.
        char8_t folded(const char8_t value) noexcept
        {
            if (value == u8'\\')
                return u8'/';
            return value >= u8'A' && value <= u8'Z' ? static_cast<char8_t>(value - u8'A' + u8'a') : value;
        }
    } // namespace

    std::unique_ptr<luil::ui_element> build_diff_area(const view_snapshot& view, const repository_view& repository, feedback_sources& sources)
    {
        static_cast<void>(view);
        const diff_panel_view& diff { *repository.diff };
        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"diff-area" }, luil::stack_config {}) };
        column->add(build_header(diff), diff_header_height);
        // LFS 포인터·submodule 포인터는 텍스트 diff가 아니라 카드다 (웹뷰 여부와 무관하다).
        if (diff.special.has_value())
            column->add_flexible(build_diff_special_card(*diff.special, repository.repository_id));
        else
            column->add_flexible(diff.use_webview ? build_webview_slot(diff) : build_text_body(diff, sources));
        return panel_surface(u8"diff-panel", std::move(column));
    }

    luil::ui_webview make_diff_webview(const diff_webview_options& options)
    {
        luil::ui_webview webview {};
        webview.id = std::u8string { diff_webview_id };
        webview.user_data_folder = options.user_data_folder;
        // 뷰어는 실행 파일 옆 폴더에서 file://로 연다. 다른 스킴은 열지 않는다.
        webview.policy.allowed_schemes = { u8"file" };
        // 페이지가 보내는 가장 큰 메시지는 copy의 텍스트(최대 4 MiB, JSON 이스케이프로 두 배까지)다.
        webview.policy.maximum_message_bytes = diff_view_maximum_message_bytes;
        webview.policy.developer_tools_enabled = false;
        webview.navigate_revision = 1;
        webview.navigate_url = options.navigate_url;
        webview.post_revision = options.post_revision;
        webview.post_message = options.post_message;
        webview.on_event = [](luil::webview_event event) { return luil::app_message { diff_webview_event_intent { std::move(event) } }; };
        return webview;
    }

    bool is_viewer_document(const std::u8string_view document_url, const std::u8string_view viewer_url) noexcept
    {
        if (document_url.size() != viewer_url.size())
            return false;
        for (std::size_t index { 0 }; index < document_url.size(); ++index)
            if (folded(document_url[index]) != folded(viewer_url[index]))
                return false;
        return true;
    }
} // namespace cyella::ui
