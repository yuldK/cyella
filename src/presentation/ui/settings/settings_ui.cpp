#include "presentation/ui/settings/settings_ui.h"

#include "application/app_messages.h"
#include "domain/text_catalog.h"
#include "presentation/hosting_intents.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/ui/app_symbol.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/segmented_control.h"
#include "presentation/ui/settings/appearance_ui.h"
#include "presentation/ui/settings/settings_page_parts.h"
#include "presentation/ui/settings/settings_pages_ui.h"
#include "presentation/ui/settings/signing_ui.h"
#include "presentation/ui/settings/tab_style_ui.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"

#include "luil/ui/check_element.h"
#include "luil/ui/choice_group_element.h"
#include "luil/ui/dialog_elements.h"
#include "luil/ui/list_element.h"
#include "luil/ui/modal_host_element.h"
#include "luil/ui/scroll_area_element.h"
#include "luil/ui/stack_element.h"

#include <algorithm>
#include <utility>

namespace cyella::ui {
    namespace {
        using namespace pages;

        constexpr float dialog_maximum_width { 900.0f };
        constexpr float dialog_maximum_height { 700.0f };
        constexpr float dialog_margin { 24.0f };
        constexpr float navigation_width { 170.0f };
        constexpr float content_padding { 16.0f };
        constexpr float content_right_padding { 10.0f };
        constexpr float scroll_inset { 8.0f };
        constexpr float banner_height { 34.0f };
        constexpr float footer_height { 26.0f };
        // 미리보기가 한 줄씩 보이는 인자의 상한이다.
        constexpr std::size_t maximum_preview_arguments { 8 };
        // 작업(키 만들기·연결 시험)의 상태를 한두 줄로 보인다. 줄 자리는 항상 잡아 아래가 밀리지 않게 한다.
        void add_task_lines(page_column& page, const std::u8string& owner, const task_view& task)
        {
            std::u8string text { task.text };
            tone value { task.result_tone };
            if (task.state == task_view::phase::running)
                value = tone::neutral;
            else if (task.state == task_view::phase::waiting_for_input)
                value = tone::warning;
            const bool waiting { task.state == task_view::phase::running || task.state == task_view::phase::waiting_for_input };
            if (waiting && task.cancellable)
            {
                // 입력을 기다리거나 도는 작업은 여기서 멈출 수 있다 (대화 상자를 닫아도 operation은 남는다).
                auto row { make_row(owner + u8"-status-row") };
                row->add_flexible(help_line(owner + u8"-status", std::move(text), tone_color(value)));
                row->add(button_of(owner + u8"-cancel", tr(u8"settings.common.cancel"), message(cancel_operation_intent { task.id }), true, luil::text_button_visual::danger),
                    { .length = 56.0f, .cross_length = 22.0f });
                page.add(std::move(row), line_height + 4.0f);
            }
            else
                page.add(help_line(owner + u8"-status", std::move(text), tone_color(value)), line_height + 4.0f);
            page.add(help_line(owner + u8"-detail", task.detail, dim), line_height);
        }

        // ------------------------------------------------------------------ 일반

        constexpr float font_preview_padding { 12.0f };
        constexpr float font_preview_height { font_preview_padding * 2.0f + 22.0f * 2.0f };
        constexpr float font_column_gap { 12.0f };
        constexpr float font_column_height { 20.0f + line_height + small_gap + input_height + 12.0f + 16.0f + font_preview_height };

        std::unique_ptr<luil::ui_element> make_font_column(const settings_view& view, const bool code)
        {
            const std::u8string owner { code ? u8"settings-code-font" : u8"settings-ui-font" };
            auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, owner + u8"-column" }, luil::stack_config {}) };
            column->add(make_text(owner + u8"-label", code ? tr(u8"settings.general.code_font") : tr(u8"settings.general.ui_font"), text_style { .size = 12.0f, .bold = true }), 20.0f);
            column->add(help_line(owner + u8"-description", code ? tr(u8"settings.general.code_font_description") : tr(u8"settings.general.ui_font_description")), line_height);
            column->add_gap(small_gap);
            column->add(make_font_combo(view, code), input_height);
            column->add_gap(12.0f);
            column->add(help_line(owner + u8"-preview-label", tr(u8"settings.general.font_preview")), 16.0f);
            const std::u8string preview_owner { code ? u8"settings-font-preview-code" : u8"settings-font-preview-ui" };
            const luil::stack_config preview_config { .padding = luil::edge_insets::all(font_preview_padding) };
            auto preview { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, owner + u8"-preview-content" }, preview_config) };
            preview->add(make_text(preview_owner, code ? std::u8string { u8"git diff --stat" } : tr(u8"settings.general.ui_font_sample"), text_style { .size = 13.0f, .code = code }), 22.0f);
            preview->add(make_text(preview_owner + u8"-detail", code ? u8"const branch = \"main\";" : u8"Aa Bb Cc 0123456789", text_style { .size = 12.0f, .code = code }), 22.0f);

            luil::panel_config card {};
            card.background = [](const luil::ui_color_palette& palette) { return palette.window_background; };
            card.border = [](const luil::ui_color_palette& palette) { return palette.divider; };
            card.corner_radius = 6.0f;
            auto panel { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_panel, owner + u8"-preview-card" }, std::move(card)) };
            panel->set_content(std::move(preview));
            column->add(std::move(panel), font_preview_height);
            return column;
        }

        void build_general(page_column& page, const settings_view& view, const float content_width)
        {
            page.add(section_title(u8"settings-section-appearance", tr(u8"settings.general.theme")), section_height);
            page.add(make_theme_picker(view.theme, view.accent), theme_picker_height);
            page.gap(small_gap);
            add_help(page, u8"settings-theme-help", tr(u8"settings.general.theme_help"), content_width);

            page.add(section_title(u8"settings-section-accent", tr(u8"settings.general.accent")), section_height);
            page.add(make_accent_picker(view.accent), accent_picker_height(content_width));
            page.gap(small_gap);
            add_help(page, u8"settings-accent-help", tr(u8"settings.general.accent_help"), content_width);

            page.add(section_title(u8"settings-section-tab-shape", tr(u8"settings.tabs.shape")), section_height);
            page.add(make_tab_shape_picker(view), tab_style_picker_height(tab_shape_choices));
            page.gap(small_gap);
            add_help(page, u8"settings-tab-shape-help", tr(u8"settings.tabs.shape_help"), content_width);

            page.add(section_title(u8"settings-section-tab-marker", tr(u8"settings.tabs.marker")), section_height);
            page.add(make_tab_marker_picker(view), tab_style_picker_height(tab_marker_choices));

            // 언어 이름은 그 언어로 쓴다 (어느 화면 언어에서든 자기 언어를 찾을 수 있게). 바꾸면 다시 시작할 때 적용한다 (ADR-008).
            page.add(section_title(u8"settings-section-language", tr(u8"settings.general.language")), section_height);
            luil::choice_group_config language {};
            language.owner = u8"settings-language";
            language.style = luil::choice_style::toggle;
            language.name = tr(u8"settings.general.language");
            language.items = {
                { u8"system", tr(u8"settings.general.language_system") },
                { u8"ko", tr(u8"settings.general.language_korean") },
                { u8"en", tr(u8"settings.general.language_english") },
            };
            language.selected = std::u8string { language_preference_name(view.language) };
            language.select = [](const std::u8string& value) { return message(set_language_intent { parse_language_preference(value).value_or(language_preference::system) }); };
            auto language_row { make_row(u8"settings-language-row") };
            language_row->add_flexible(make_segmented_control(std::move(language)));
            page.add(std::move(language_row), input_height);
            add_help(page, u8"settings-language-help", tr(u8"settings.general.language_help"), content_width);

            page.add(section_title(u8"settings-section-fonts", tr(u8"settings.general.fonts")), section_height);
            const bool font_columns { content_width >= 480.0f + font_column_gap };
            const luil::stack_config fonts_config { .direction = font_columns ? luil::stack_direction::row : luil::stack_direction::column, .spacing = font_column_gap };
            auto fonts { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"settings-font-columns" }, fonts_config) };
            for (const bool code : { false, true })
                if (font_columns)
                    fonts->add_flexible(make_font_column(view, code));
                else
                    fonts->add(make_font_column(view, code), font_column_height);
            page.add(std::move(fonts), font_columns ? font_column_height : font_column_height * 2.0f + font_column_gap);
            page.gap(small_gap);
            add_help(page, u8"settings-font-help", tr(u8"settings.general.font_help"), content_width);
            add_help(page, u8"settings-font-fallback", tr(u8"settings.general.font_fallback"), content_width);
            page.gap(small_gap);
            auto font_actions { make_row(u8"settings-font-actions") };
            font_actions->add_flexible_gap();
            font_actions->add(
                button_of(u8"settings-font-apply", tr(u8"settings.general.apply_fonts"), message(apply_font_settings_intent {}), view.fonts_dirty, luil::text_button_visual::accent), 88.0f);
            page.add(std::move(font_actions), input_height);

            page.add(section_title(u8"settings-section-author", tr(u8"settings.general.author_section")), section_height);
            // 작성자 사진은 밖으로 메일 해시를 보내므로 사용자가 켜야만 쓴다.
            luil::check_config gravatar {
                .owner = u8"settings-gravatar",
                .style = luil::check_style::toggle_switch,
                .label = tr(u8"settings.general.gravatar"),
                .checked = view.gravatar,
                .toggle = action_of(message(set_gravatar_intent { view.gravatar == false })),
            };
            page.add(std::make_unique<luil::check_element>(std::move(gravatar)), luil::check_row_height);
            add_help(page, u8"settings-gravatar-help", tr(u8"settings.general.gravatar_help"), content_width);
            add_help(page, u8"settings-gravatar-fallback", tr(u8"settings.general.gravatar_fallback"), content_width);
            page.gap(large_gap);

            const git_settings_view& git { view.git };
            page.add(section_title(u8"settings-section-git", u8"Git"), section_height);
            page.add(field_label(u8"settings-git-path-label", tr(u8"settings.general.git_path")), label_height);
            auto path_row { make_row(u8"settings-git-path-row") };
            path_row->add_flexible(input_of(kind_settings_git_path_input, u8"settings-git-path", git.path, tr(u8"settings.general.git_path_placeholder"), tr(u8"settings.general.git_path")));
            path_row->add(button_of(u8"settings-git-browse", tr(u8"settings.common.browse"), ui_command(static_cast<std::uint32_t>(ui_command_id::choose_git_executable))),
                { .length = 88.0f, .cross_length = button_height });
            page.add(std::move(path_row), input_height);
            page.gap(4.0f);
            if (git.validation.empty() == false)
                page.add(help_line(u8"settings-git-validation", git.validation, tone_color(tone::bad)), line_height);
            else if (git.dirty)
                page.add(help_line(u8"settings-git-validation", tr(u8"settings.general.git_path_pending")), line_height);
            else
                page.add(help_line(u8"settings-git-validation", git.configured ? tr(u8"settings.general.git_path_custom") : tr(u8"settings.general.git_path_auto")), line_height);
            page.gap(small_gap);
            const std::u8string reset_label { tr(u8"settings.general.git_reset") };
            auto reset_row { make_row(u8"settings-git-reset-row") };
            reset_row->add(
                button_of(u8"settings-git-reset", reset_label, message(reset_git_path_intent {}), git.configured), { .length = text_button_width(reset_label), .cross_length = button_height });
            reset_row->add_flexible_gap();
            reset_row->add(button_of(u8"settings-git-apply", tr(u8"settings.common.apply"), message(apply_git_path_intent {}), git.can_apply, luil::text_button_visual::accent),
                { .length = 56.0f, .cross_length = button_height });
            page.add(std::move(reset_row), input_height);
            page.gap(large_gap);

            const luil::stack_config status_config { .padding = luil::edge_insets::all(10.0f) };
            auto status { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"settings-git-status-content" }, status_config) };
            status->add(help_line(u8"settings-git-using-label", tr(u8"settings.general.git_in_use")), line_height);
            if (git.problem.empty())
            {
                status->add(make_text(u8"settings-git-using", git.executable, text_style { .size = 12.0f, .code = true }), 22.0f);
                status->add(help_line(u8"settings-git-version", git.version + u8" · " + git.origin), line_height);
            }
            else
            {
                status->add(make_text(u8"settings-git-problem", git.problem, text_style { .size = 12.0f, .color = tone_color(tone::bad) }), 22.0f);
                status->add(help_line(u8"settings-git-version", tr(u8"settings.general.git_set_path")), line_height);
            }
            luil::panel_config status_panel {};
            status_panel.background = [](const luil::ui_color_palette& palette) { return palette.window_background; };
            status_panel.corner_radius = 6.0f;
            auto current { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_panel, u8"settings-git-status" }, std::move(status_panel)) };
            current->set_content(std::move(status));
            page.add(std::move(current), 20.0f + line_height * 2.0f + 22.0f);
        }

        // ------------------------------------------------------------------ diff 도구

        std::unique_ptr<luil::ui_element> build_tool_row(const tool_row_view& tool, const bool editing, const bool stacked)
        {
            const std::u8string key { (tool.file_app ? u8"app-" : u8"") + number_text(tool.index) };
            auto row { make_row(u8"settings-tool-row-" + key, 8.0f) };
            auto name { make_text(u8"settings-tool-extensions-" + key, tool.extensions, text_style { .size = 12.0f, .bold = true }) };
            if (stacked)
                row->add_flexible(std::move(name));
            else
                row->add(std::move(name), 130.0f);
            if (stacked == false)
            {
                row->add_flexible(make_text(u8"settings-tool-executable-" + key, tool.executable, text_style { .size = 11.0f, .code = true }));
                row->add_flexible(make_text(u8"settings-tool-arguments-" + key, tool.arguments, text_style { .size = 11.0f, .color = dim, .code = true }));
            }
            row->add(button_of(u8"settings-tool-edit-" + key, tr(u8"settings.common.edit"), message(edit_tool_intent { tool.index, tool.file_app }), editing == false),
                { .length = 52.0f, .cross_length = button_height });
            row->add(button_of(u8"settings-tool-remove-" + key, tr(u8"settings.common.delete"), message(remove_tool_intent { tool.index, tool.file_app }), editing == false,
                         luil::text_button_visual::danger),
                { .length = 52.0f, .cross_length = button_height });
            if (stacked == false)
                return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-tool-bar-" + key }, bar_style { .divider_bottom = true }, std::move(row));
            auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"settings-tool-stack-" + key }, luil::stack_config {}) };
            column->add(std::move(row), row_height);
            column->add(code_line(u8"settings-tool-executable-" + key, tool.executable), line_height);
            column->add(code_line(u8"settings-tool-arguments-" + key, tool.arguments), line_height);
            column->add_gap(small_gap);
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-tool-bar-" + key }, bar_style { .divider_bottom = true }, std::move(column));
        }

        void build_tool_form(page_column& page, const tool_edit_view& edit)
        {
            page_column form { u8"settings-tool-form-column" };
            form.add(
                section_title(u8"settings-tool-form-title",
                    edit.file_app ? (edit.is_new ? tr(u8"settings.diff.add_app") : tr(u8"settings.diff.edit_app")) : (edit.is_new ? tr(u8"settings.diff.add_tool") : tr(u8"settings.diff.edit_tool"))),
                section_height);
            form.add(field_label(u8"settings-tool-extensions-label", edit.file_app ? tr(u8"settings.diff.app_name") : tr(u8"settings.diff.extensions_label")), label_height);
            form.add(input_of(kind_settings_tool_extensions_input, u8"settings-tool-extensions-input", edit.extensions,
                         edit.file_app ? tr(u8"settings.diff.app_name_placeholder") : std::u8string { u8".png, .psd" }),
                input_height);
            form.gap(small_gap);
            form.add(field_label(u8"settings-tool-executable-label", tr(u8"settings.diff.executable")), label_height);
            auto executable_row { make_row(u8"settings-tool-executable-row") };
            executable_row->add_flexible(input_of(kind_settings_tool_executable_input, u8"settings-tool-executable-input", edit.executable, u8"C:\\Tools\\imgdiff.exe"));
            executable_row->add(button_of(u8"settings-tool-browse", tr(u8"settings.common.browse"), ui_command(static_cast<std::uint32_t>(ui_command_id::choose_tool_executable))),
                { .length = 88.0f, .cross_length = button_height });
            form.add(std::move(executable_row), input_height);
            form.gap(small_gap);
            form.add(field_label(u8"settings-tool-arguments-label", tr(u8"settings.diff.arguments_label")), label_height);
            form.add(input_of(kind_settings_tool_arguments_input, u8"settings-tool-arguments-input", edit.arguments, edit.file_app ? u8"--reuse-window {file}" : u8"{left} {right}"), input_height);
            auto chips { make_row(u8"settings-tool-chips") };
            const auto tokens
                = edit.file_app ? std::vector<std::u8string_view> { u8"{file}", u8"{repository}", u8"{path}" } : std::vector<std::u8string_view> { u8"{left}", u8"{right}", u8"{base}", u8"{path}" };
            for (const std::u8string_view token : tokens)
                chips->add(
                    button_of(u8"settings-tool-chip-" + std::u8string { token.substr(1, token.size() - 2) }, std::u8string { token }, message(insert_tool_token_intent { std::u8string { token } })),
                    { .length = text_button_width(token) + 6.0f, .cross_length = button_height });
            chips->add_flexible_gap();
            form.add(std::move(chips), input_height);
            form.gap(small_gap);
            form.add(field_label(u8"settings-tool-preview-label", tr(u8"settings.diff.preview_label")), label_height);
            if (edit.preview.empty())
                form.add(code_line(u8"settings-tool-preview", tr(u8"settings.diff.preview_unavailable")), line_height);
            else
            {
                // 풀린 인자를 한 줄씩 보인다. 공백이 든 경로가 한 인자로 남는지 눈으로 확인하려는 것이다.
                form.add(code_line(u8"settings-tool-preview",
                             tr(u8"settings.diff.preview_run",
                                 { { u8"executable", edit.executable.displayed_text().empty() ? tr(u8"settings.diff.preview_no_executable") : std::u8string { edit.executable.displayed_text() } } })),
                    line_height);
                const std::size_t shown { std::min<std::size_t>(edit.preview_arguments.size(), maximum_preview_arguments) };
                for (std::size_t index { 0 }; index < shown; ++index)
                    form.add(code_line(u8"settings-tool-preview-" + number_text(index),
                                 tr(u8"settings.diff.preview_argument", { { u8"number", number_text(index + 1) }, { u8"value", edit.preview_arguments[index] } })),
                        line_height);
                if (edit.preview_arguments.size() > shown)
                    form.add(code_line(u8"settings-tool-preview-more", tr(u8"settings.diff.preview_more", { { u8"count", number_text(edit.preview_arguments.size() - shown) } })), line_height);
            }
            if (edit.error.empty() == false)
                form.add(help_line(u8"settings-tool-error", edit.error, tone_color(tone::bad)), line_height);
            else
                form.add(help_line(u8"settings-tool-error", edit.warning, tone_color(tone::warning)), line_height);
            form.gap(small_gap);
            auto buttons { make_row(u8"settings-tool-buttons") };
            buttons->add_flexible_gap();
            buttons->add(button_of(u8"settings-tool-cancel", tr(u8"settings.common.cancel"), message(cancel_tool_edit_intent {})), { .length = 64.0f, .cross_length = button_height });
            buttons->add(button_of(u8"settings-tool-save", tr(u8"settings.common.save"), message(save_tool_intent {}), edit.can_save, luil::text_button_visual::accent),
                { .length = 64.0f, .cross_length = button_height });
            form.add(std::move(buttons), input_height);

            const float height { form.height() + 24.0f };
            auto padded { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"settings-tool-form-padding" }, luil::stack_config { .padding = luil::edge_insets::all(12.0f) }) };
            padded->add_flexible(form.take());
            page.add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-tool-form" },
                         bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.window_background; } }, std::move(padded)),
                height);
        }

        // diff를 보는 방식이다: 처음 열 때의 보기와 hunk 둘레의 문맥 줄 수.
        void build_diff_view_settings(page_column& page, const settings_view& view, const float content_width)
        {
            page.add(section_title(u8"settings-section-diff-view", tr(u8"settings.diff.view_section")), section_height);
            page.add(field_label(u8"settings-diff-layout-label", tr(u8"settings.diff.default_view")), label_height);
            luil::choice_group_config layout {};
            layout.owner = u8"settings-diff-layout";
            layout.style = luil::choice_style::toggle;
            layout.name = tr(u8"settings.diff.default_view_name");
            layout.items = { { u8"inline", u8"Inline" }, { u8"split", tr(u8"settings.diff.layout_split") } };
            layout.selected = std::u8string { diff_layout_preference_name(view.diff.layout) };
            layout.select = [](const std::u8string& value) { return message(set_diff_layout_default_intent { parse_diff_layout_preference(value).value_or(diff_layout_preference::inline_view) }); };
            auto layout_row { make_row(u8"settings-diff-layout-row") };
            layout_row->add_flexible(make_segmented_control(std::move(layout)));
            page.add(std::move(layout_row), input_height);
            add_help(page, u8"settings-diff-layout-help", tr(u8"settings.diff.layout_help"), content_width);
            page.gap(small_gap);

            page.add(field_label(u8"settings-diff-context-label", tr(u8"settings.diff.context_label")), label_height);
            luil::choice_group_config context {};
            context.owner = u8"settings-diff-context";
            context.style = luil::choice_style::toggle;
            context.name = tr(u8"settings.diff.context_name");
            std::vector<std::uint32_t> presets { 0, 1, 3, 5, 10, 25 };
            // 설정 파일을 손으로 고쳐 프리셋에 없는 값이 있으면 그 값도 고를 수 있게 끼운다.
            if (std::ranges::find(presets, view.diff.context_lines) == presets.end())
            {
                presets.push_back(view.diff.context_lines);
                std::ranges::sort(presets);
            }
            for (const std::uint32_t lines : presets)
                context.items.push_back({ number_text(lines), number_text(lines) });
            context.selected = number_text(view.diff.context_lines);
            context.select = [](const std::u8string& value) {
                std::uint32_t lines { default_diff_context_lines };
                for (const char8_t digit : value)
                    if (digit < u8'0' || digit > u8'9')
                        return message(set_diff_context_lines_intent { default_diff_context_lines });
                if (value.empty() == false)
                    lines = static_cast<std::uint32_t>(std::stoul(std::string { value.begin(), value.end() }));
                return message(set_diff_context_lines_intent { lines });
            };
            auto context_row { make_row(u8"settings-diff-context-row") };
            context_row->add_flexible(make_segmented_control(std::move(context)));
            page.add(std::move(context_row), input_height);
            add_help(page, u8"settings-diff-context-help", tr(u8"settings.diff.context_help"), content_width);
            page.gap(large_gap);
        }

        void build_diff_tools(page_column& page, const settings_view& view, const float content_width)
        {
            const bool stacked { content_width < 560.0f };
            const float tool_height { stacked ? row_height + line_height * 2.0f + small_gap : row_height };
            build_diff_view_settings(page, view, content_width);
            const bool editing { view.edit.has_value() };
            page.add(section_title(u8"settings-section-file-apps", tr(u8"settings.diff.file_apps_section")), section_height);
            add_help(page, u8"settings-file-apps-help", tr(u8"settings.diff.file_apps_help"), content_width);
            add_help(page, u8"settings-file-apps-revision-help", tr(u8"settings.diff.file_apps_revision_help"), content_width);
            page.gap(small_gap);
            if (view.file_apps.empty())
                page.add(help_line(u8"settings-file-apps-empty", tr(u8"settings.diff.file_apps_empty")), row_height);
            for (const tool_row_view& app : view.file_apps)
                page.add(build_tool_row(app, editing, stacked), tool_height);
            page.gap(small_gap);
            auto app_add { make_row(u8"settings-file-app-add-row") };
            app_add->add(button_of(u8"settings-file-app-add", tr(u8"settings.diff.add_app"), message(add_tool_intent { true }), editing == false, luil::text_button_visual::accent),
                { .length = 92.0f, .cross_length = button_height });
            page.add(std::move(app_add), input_height);
            if (view.edit.has_value() && view.edit->file_app)
            {
                page.gap(small_gap);
                build_tool_form(page, *view.edit);
            }
            page.gap(large_gap);
            page.add(section_title(u8"settings-section-tools", tr(u8"settings.diff.tools_section")), section_height);
            add_help(page, u8"settings-tools-help-1", tr(u8"settings.diff.tools_help_1"), content_width);
            add_help(page, u8"settings-tools-help-2", tr(u8"settings.diff.tools_help_2"), content_width);
            page.gap(small_gap);
            if (view.tools.empty())
                page.add(help_line(u8"settings-tools-empty", tr(u8"settings.diff.tools_empty")), row_height);
            else
            {
                auto header { make_row(u8"settings-tools-header", 8.0f) };
                header->add(field_label(u8"settings-tools-header-extensions", tr(u8"settings.diff.extensions")), 130.0f);
                header->add_flexible(field_label(u8"settings-tools-header-executable", tr(u8"settings.diff.executable")));
                header->add_flexible(field_label(u8"settings-tools-header-arguments", tr(u8"settings.diff.arguments")));
                header->add_gap(112.0f);
                if (stacked == false)
                    page.add(std::move(header), line_height + 4.0f);
                for (const tool_row_view& tool : view.tools)
                    page.add(build_tool_row(tool, editing, stacked), tool_height);
            }
            page.gap(small_gap);
            auto add_row { make_row(u8"settings-tools-add-row") };
            add_row->add(button_of(u8"settings-tool-add", tr(u8"settings.diff.add_tool"), message(add_tool_intent {}), editing == false, luil::text_button_visual::accent),
                { .length = 92.0f, .cross_length = button_height });
            page.add(std::move(add_row), input_height);
            if (view.edit.has_value() && view.edit->file_app == false)
            {
                page.gap(small_gap);
                build_tool_form(page, *view.edit);
            }
            page.gap(large_gap);
            page.add(section_title(u8"settings-section-fallback", tr(u8"settings.diff.fallback_section")), section_height);
            add_help(page, u8"settings-fallback-help-1", tr(u8"settings.diff.fallback_help_1"), content_width);
            add_help(page, u8"settings-fallback-help-2", tr(u8"settings.diff.fallback_help_2"), content_width);
        }

        // ------------------------------------------------------------------ SSH

        void build_ssh(page_column& page, const settings_view& view, const float content_width)
        {
            const bool stacked { content_width < 560.0f };
            const ssh_view& ssh { view.ssh };
            page.add(section_title(u8"settings-section-agent", u8"ssh-agent"), section_height);
            auto service_row { make_row(u8"settings-agent-row") };
            if (ssh.loaded == false)
                service_row->add_flexible(make_text(u8"settings-agent-status", tr(u8"settings.ssh.reading_status"), text_style { .size = 12.0f, .color = dim }));
            else
                service_row->add_flexible(make_text(u8"settings-agent-status", tr(u8"settings.ssh.service_status", { { u8"status", ssh.service_status } }),
                    text_style { .size = 12.0f, .color = tone_color(ssh.service_tone), .bold = true }));
            service_row->add(button_of(u8"settings-ssh-refresh", ssh.loading ? tr(u8"settings.common.loading") : tr(u8"settings.common.refresh"), message(refresh_ssh_intent {}), ssh.loading == false),
                { .length = 84.0f, .cross_length = button_height });
            page.add(std::move(service_row), input_height);
            page.add(help_line(u8"settings-agent-help", ssh.service_help), line_height);
            page.gap(small_gap);
            page.add(field_label(u8"settings-agent-identities-label", tr(u8"settings.ssh.agent_keys_label")), label_height);
            if (ssh.identities.empty())
                page.add(help_line(u8"settings-agent-message", ssh.agent_message), line_height);
            else
                for (std::size_t index { 0 }; index < ssh.identities.size(); ++index)
                    page.add(code_line(u8"settings-agent-identity-" + number_text(index), ssh.identities[index]), line_height);
            page.gap(large_gap);

            page.add(section_title(u8"settings-section-keys", tr(u8"settings.ssh.key_files")), section_height);
            page.add(help_line(u8"settings-keys-directory", ssh.ssh_directory.empty() ? tr(u8"settings.ssh.no_ssh_directory") : ssh.ssh_directory), line_height);
            if (ssh.keys.empty())
                page.add(help_line(u8"settings-keys-empty", ssh.loaded ? tr(u8"settings.ssh.no_keys") : tr(u8"settings.common.loading")), row_height);
            for (const ssh_key_row_view& key : ssh.keys)
            {
                auto row { make_row(u8"settings-key-row-" + key.name, 8.0f) };
                auto name { make_text(u8"settings-key-name-" + key.name, key.name, text_style { .size = 12.0f, .bold = true }) };
                if (stacked)
                    row->add_flexible(std::move(name));
                else
                    row->add(std::move(name), 180.0f);
                if (stacked == false)
                    row->add_flexible(make_text(u8"settings-key-detail-" + key.name, key.detail, text_style { .size = 11.0f, .color = dim }));
                row->add(button_of(u8"settings-key-copy-" + key.name, tr(u8"settings.ssh.copy_public_key"), message(copy_public_key_intent { key.name }), key.can_copy),
                    { .length = 100.0f, .cross_length = button_height });
                if (stacked)
                {
                    page.add(std::move(row), row_height);
                    page.add(help_line(u8"settings-key-detail-" + key.name, key.detail), line_height);
                    page.gap(small_gap);
                }
                else
                    page.add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-key-bar-" + key.name }, bar_style { .divider_bottom = true }, std::move(row)), row_height);
            }
            page.gap(large_gap);

            page.add(section_title(u8"settings-section-keygen", tr(u8"settings.ssh.create_key")), section_height);
            page.add(help_line(u8"settings-keygen-tools", ssh.tools_found ? tr(u8"settings.ssh.keygen_found", { { u8"origin", ssh.tools_origin } }) : tr(u8"settings.ssh.keygen_missing"),
                         ssh.tools_found ? dim : tone_color(tone::bad)),
                line_height);
            page.add(field_label(u8"settings-keygen-name-label", tr(u8"settings.ssh.key_file_name")), label_height);
            page.add(input_of(kind_settings_key_name_input, u8"settings-keygen-name", ssh.key_name, u8"id_ed25519"), input_height);
            page.add(field_label(u8"settings-keygen-comment-label", tr(u8"settings.ssh.key_comment")), label_height);
            page.add(input_of(kind_settings_key_comment_input, u8"settings-keygen-comment", ssh.key_comment, u8"you@example.com"), input_height);
            page.add(code_line(u8"settings-keygen-command", ssh.keygen_command), line_height);
            page.add(help_line(u8"settings-keygen-error", ssh.keygen_error, tone_color(tone::bad)), line_height);
            auto create_row { make_row(u8"settings-keygen-row") };
            create_row->add(button_of(u8"settings-keygen-create", tr(u8"settings.ssh.create_key"), message(create_ssh_key_intent {}), ssh.can_create, luil::text_button_visual::accent),
                { .length = 96.0f, .cross_length = button_height });
            page.add(std::move(create_row), input_height);
            add_task_lines(page, u8"settings-keygen", ssh.keygen);
            page.gap(large_gap);

            page.add(section_title(u8"settings-section-test", tr(u8"settings.ssh.test_section")), section_height);
            page.add(field_label(u8"settings-test-host-label", tr(u8"settings.ssh.test_host_label")), label_height);
            auto test_row { make_row(u8"settings-test-row") };
            test_row->add_flexible(input_of(kind_settings_test_host_input, u8"settings-test-host", ssh.test_host, u8"github.com"));
            test_row->add(button_of(u8"settings-test-run", tr(u8"settings.ssh.test"), message(test_ssh_connection_intent {}), ssh.can_test, luil::text_button_visual::accent),
                { .length = 64.0f, .cross_length = button_height });
            page.add(std::move(test_row), input_height);
            page.add(code_line(u8"settings-test-command", ssh.test_command), line_height);
            page.add(help_line(u8"settings-test-error", ssh.test_error, tone_color(tone::bad)), line_height);
            add_task_lines(page, u8"settings-test", ssh.test);
        }

        // ------------------------------------------------------------------ 호스팅

        void build_web_links(page_column& page, const hosting_settings_page_view& hosting, float content_width);

        void build_hosting(page_column& page, const settings_view& view, const float content_width)
        {
            const bool stacked { content_width < 560.0f };
            const hosting_settings_page_view& hosting { view.hosting };
            page.add(section_title(u8"settings-section-hosting", tr(u8"settings.hosting.section")), section_height);
            add_help(page, u8"settings-hosting-help", tr(u8"settings.hosting.help"), content_width);
            add_help(page, u8"settings-hosting-help-2", tr(u8"settings.hosting.help_2"), content_width);
            page.gap(small_gap);
            for (const hosting_host_row_view& host : hosting.hosts)
            {
                const std::u8string name { to_u8(host.host) };
                auto row { make_row(u8"settings-hosting-row-" + name, 8.0f) };
                auto title { make_text(u8"settings-hosting-host-" + name, name, text_style { .size = 12.0f, .bold = true }) };
                if (stacked)
                    row->add_flexible(std::move(title));
                else
                    row->add(std::move(title), 170.0f);
                const std::u8string kind { host.provider == hosting_provider::gitlab ? u8"GitLab" : u8"GitHub" };
                const std::u8string provider_text { kind + u8" · " + (host.api_base.empty() ? std::u8string { tr(u8"settings.hosting.default_api") } : to_u8(host.api_base)) };
                auto provider { make_text(u8"settings-hosting-kind-" + name, provider_text, text_style { .size = 11.0f, .color = dim }) };
                if (stacked == false)
                    row->add_flexible(std::move(provider));
                std::u8string state { tr(u8"settings.hosting.signed_out") };
                if (host.signed_in)
                    state = tr(u8"settings.hosting.signed_in", { { u8"user", host.user.empty() ? tr(u8"settings.hosting.checking_account") : host.user } });
                const text_style status_style { .size = 11.0f, .color = host.signed_in ? [](const luil::ui_color_palette& palette) { return success_color(palette); } : dim, .bold = host.signed_in };
                auto status { make_text(u8"settings-hosting-state-" + name, state, status_style) };
                if (stacked == false)
                    row->add(std::move(status), 150.0f);
                row->add(button_of(u8"settings-hosting-logout-" + name, tr(u8"settings.hosting.sign_out"), message(hosting_sign_out_intent { host.host }), host.signed_in),
                    { .length = 76.0f, .cross_length = button_height });
                if (host.builtin == false)
                    row->add(button_of(u8"settings-hosting-remove-" + name, tr(u8"settings.common.delete"), message(remove_hosting_host_intent { host.host }), true, luil::text_button_visual::danger),
                        { .length = 56.0f, .cross_length = button_height });
                if (stacked)
                {
                    auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"settings-hosting-stack-" + name }, luil::stack_config {}) };
                    column->add(std::move(row), row_height);
                    column->add(std::move(provider), line_height);
                    column->add(std::move(status), line_height);
                    column->add_gap(small_gap);
                    page.add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-hosting-bar-" + name }, bar_style { .divider_bottom = true }, std::move(column)),
                        row_height + line_height * 2.0f + small_gap);
                }
                else
                    page.add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-hosting-bar-" + name }, bar_style { .divider_bottom = true }, std::move(row)), row_height);
                if (host.rate_limit_text.empty() == false)
                    page.add(help_line(u8"settings-hosting-rate-" + name, host.rate_limit_text), line_height);
            }
            page.gap(large_gap);

            page.add(section_title(u8"settings-section-hosting-add", tr(u8"settings.hosting.add_section")), section_height);
            add_help(page, u8"settings-hosting-add-help", tr(u8"settings.hosting.add_help"), content_width);
            page.add(field_label(u8"settings-hosting-host-label", tr(u8"settings.hosting.host_label")), label_height);
            page.add(input_of(kind_settings_hosting_host_input, u8"settings-hosting-host-input", hosting.host, u8"git.example.com"), input_height);
            page.add(field_label(u8"settings-hosting-provider-label", tr(u8"settings.hosting.provider")), label_height);
            {
                page.add(std::make_unique<segmented_control_element>(u8"settings-hosting-provider", tr(u8"settings.hosting.provider"),
                             std::vector<segment_option> { { u8"github", u8"GitHub" }, { u8"gitlab", u8"GitLab" } }, hosting.new_provider == hosting_provider::github ? u8"github" : u8"gitlab",
                             [](const std::u8string& value) { return message(set_new_hosting_provider_intent { value == u8"gitlab" ? hosting_provider::gitlab : hosting_provider::github }); }),
                    input_height);
            }
            page.add(field_label(u8"settings-hosting-api-label", tr(u8"settings.hosting.api_label")), label_height);
            page.add(input_of(kind_settings_hosting_api_input, u8"settings-hosting-api-input", hosting.api_base, u8"https://git.example.com/api/v4"), input_height);
            page.add(help_line(u8"settings-hosting-error", hosting.error, tone_color(tone::bad)), line_height);
            auto add_row { make_row(u8"settings-hosting-add-row") };
            add_row->add(button_of(u8"settings-hosting-add", tr(u8"settings.hosting.add"), message(add_hosting_host_intent {}), hosting.can_add, luil::text_button_visual::accent),
                { .length = 100.0f, .cross_length = button_height });
            page.add(std::move(add_row), input_height);
            page.gap(large_gap);
            build_web_links(page, hosting, content_width);
        }

        std::u8string web_link_service_label(const web_link_service service)
        {
            const std::string name { web_link_service_display_name(service) };
            return name.empty() ? tr(u8"settings.web_link.other") : to_u8(name);
        }

        // 웹 링크 표다: 원격 URL의 호스트 → 브라우저로 여는 웹 주소와 "열기"·사이드바에 그릴 codicon이다.
        void build_web_links(page_column& page, const hosting_settings_page_view& hosting, const float content_width)
        {
            page.add(section_title(u8"settings-section-web-links", tr(u8"settings.web_link.section")), section_height);
            add_help(page, u8"settings-web-links-help", tr(u8"settings.web_link.help"), content_width);
            page.gap(small_gap);
            for (const web_link_row_view& link : hosting.web_links)
            {
                const std::u8string name { to_u8(link.host) };
                auto row { make_row(u8"settings-web-link-row-" + name, 8.0f) };
                row->add(
                    std::make_unique<icon_element>(luil::ui_element_id { kind_label, u8"settings-web-link-icon-" + name }, link.glyph != 0 ? link.glyph : codicons::icon_link_external, dim), 18.0f);
                row->add(make_text(u8"settings-web-link-host-" + name, name, text_style { .size = 12.0f, .bold = true }), content_width < 560.0f ? 120.0f : 170.0f);
                const std::u8string base { link.web_base.empty() ? u8"https://" + name : to_u8(link.web_base) };
                row->add_flexible(
                    make_text(u8"settings-web-link-kind-" + name, web_link_service_label(link.service) + u8" · " + base + u8" · " + to_u8(link.icon), text_style { .size = 11.0f, .color = dim }));
                if (link.builtin == false)
                    row->add(button_of(u8"settings-web-link-remove-" + name, tr(u8"settings.common.delete"), message(remove_web_link_intent { link.host }), true, luil::text_button_visual::danger),
                        { .length = 56.0f, .cross_length = button_height });
                page.add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-web-link-bar-" + name }, bar_style { .divider_bottom = true }, std::move(row)), row_height);
            }
            page.gap(large_gap);

            page.add(section_title(u8"settings-section-web-link-add", tr(u8"settings.web_link.add_section")), section_height);
            add_help(page, u8"settings-web-link-add-help", tr(u8"settings.web_link.add_help"), content_width);
            page.add(field_label(u8"settings-web-link-host-label", tr(u8"settings.web_link.host_label")), label_height);
            page.add(input_of(kind_settings_web_link_host_input, u8"settings-web-link-host-input", hosting.link_host, u8"git.example.com"), input_height);
            page.add(field_label(u8"settings-web-link-service-label", tr(u8"settings.web_link.service_label")), label_height);
            std::vector<segment_option> services {};
            for (const web_link_service service : { web_link_service::github, web_link_service::gitlab, web_link_service::gitea, web_link_service::bitbucket, web_link_service::other })
                services.push_back({ to_u8(web_link_service_name(service)), web_link_service_label(service) });
            page.add(std::make_unique<segmented_control_element>(u8"settings-web-link-service", tr(u8"settings.web_link.service_label"), std::move(services),
                         to_u8(web_link_service_name(hosting.new_link_service)),
                         [](const std::u8string& value) {
                             const std::optional<web_link_service> service { parse_web_link_service(std::string { value.begin(), value.end() }) };
                             return message(set_new_web_link_service_intent { service.value_or(web_link_service::other) });
                         }),
                input_height);
            page.add(field_label(u8"settings-web-link-base-label", tr(u8"settings.web_link.base_label")), label_height);
            page.add(input_of(kind_settings_web_link_base_input, u8"settings-web-link-base-input", hosting.link_base, u8"https://git.example.com"), input_height);
            page.add(field_label(u8"settings-web-link-icon-label", tr(u8"settings.web_link.icon_label")), label_height);
            auto icon_row { make_row(u8"settings-web-link-icon-row", 8.0f) };
            icon_row->add(std::make_unique<icon_element>(
                              luil::ui_element_id { kind_label, u8"settings-web-link-icon-preview" }, hosting.link_icon_preview != 0 ? hosting.link_icon_preview : codicons::icon_question, dim, 16.0f),
                20.0f);
            icon_row->add_flexible(input_of(kind_settings_web_link_icon_input, u8"settings-web-link-icon-input", hosting.link_icon, tr(u8"settings.web_link.icon_placeholder")));
            page.add(std::move(icon_row), input_height);
            page.add(help_line(u8"settings-web-link-error", hosting.link_error, tone_color(tone::bad)), line_height);
            auto add_row { make_row(u8"settings-web-link-add-row") };
            add_row->add(button_of(u8"settings-web-link-add", tr(u8"settings.web_link.add"), message(add_web_link_intent {}), hosting.can_add_link, luil::text_button_visual::accent),
                { .length = 100.0f, .cross_length = button_height });
            page.add(std::move(add_row), input_height);
        }

        // ------------------------------------------------------------------ 틀

        std::unique_ptr<luil::ui_element> build_navigation(const settings_view& view, const bool compact)
        {
            luil::ui_element_id column_id { kind_layout, u8"settings-navigation" };
            const luil::stack_config column_config { .spacing = 4.0f, .padding = luil::edge_insets::all(10.0f) };
            auto column { std::make_unique<luil::stack_element>(std::move(column_id), column_config) };
            luil::list_config navigation {};
            navigation.owner = u8"settings-pages";
            navigation.row_height = 34.0f;
            std::vector<std::pair<std::u8string, settings_page>> pages {};
            const auto entry = [&](std::u8string owner, std::u8string text, const settings_page page, const char32_t glyph) {
                if (view.page == page)
                    navigation.selected = owner;
                pages.emplace_back(owner, page);
                navigation.items.push_back({ std::move(owner), std::move(text), glyph });
            };
            entry(u8"settings-page-general", tr(u8"settings.pages.general"), settings_page::general, codicons::icon_settings_gear);
            entry(u8"settings-page-diff-tools", tr(u8"settings.pages.diff_tools"), settings_page::diff_tools, codicons::icon_diff);
            entry(u8"settings-page-ssh", u8"SSH", settings_page::ssh, codicons::icon_key);
            entry(u8"settings-page-signing", tr(u8"settings.pages.signing"), settings_page::signing, codicons::icon_verified);
            entry(u8"settings-page-hosting", tr(u8"settings.pages.hosting"), settings_page::hosting, codicons::icon_cloud);
            entry(u8"settings-page-profiles", tr(u8"settings.pages.profiles"), settings_page::profiles, codicons::icon_person);
            entry(u8"settings-page-shortcuts", tr(u8"settings.pages.shortcuts"), settings_page::shortcuts, codicons::icon_keyboard);
            navigation.select = [pages = std::move(pages)](const std::u8string& key) {
                const auto found { std::ranges::find(pages, key, &std::pair<std::u8string, settings_page>::first) };
                return message(select_settings_page_intent { found != pages.end() ? found->second : settings_page::general });
            };
            auto list { std::make_unique<luil::list_element>(std::move(navigation)) };
            list->set_search_label(tr(u8"settings.pages.list_name"));
            column->add(std::move(list), 34.0f * 7.0f);
            // 목록 맨 아래는 앱 심볼과 이름·버전이다. 따로 정보 화면을 두지 않고 이 자리가 그 몫을 한다.
            column->add_flexible_gap();
            auto about { make_row(u8"settings-about", 10.0f) };
            if (app_symbol().valid() && compact == false)
                about->add(make_app_symbol(u8"settings-symbol"), luil::stack_item { .length = 32.0f, .cross_length = 32.0f });
            auto names { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"settings-about-names" }, luil::stack_config {}) };
            // 타이틀 그림이 있으면 앱 이름 글자 대신 그린다. 그림은 비율대로 왼쪽에 붙인다.
            const bool titled { app_title_available() };
            const float name_height { titled ? 30.0f : 18.0f };
            if (titled)
            {
                auto title { make_row(u8"settings-app-title-row", 0.0f) };
                title->add(make_app_title(u8"settings-app-name", u8"cyella"), luil::stack_item { .length = name_height * app_title_aspect(), .cross_length = name_height });
                title->add_flexible_gap();
                names->add(std::move(title), name_height);
            }
            else
                names->add(make_text(u8"settings-app-name", u8"cyella", text_style { .size = 13.0f, .bold = true }), name_height);
            names->add(make_text(u8"settings-app-version", tr(u8"settings.about.version", { { u8"version", app_version() } }),
                           text_style { .size = 11.0f, .color = [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } }),
                16.0f);
            about->add(std::move(names), luil::stack_item { .length = 0.0f, .weight = 1.0f, .cross_length = name_height + 16.0f });
            if (compact == false)
                column->add(std::move(about), name_height + 18.0f);
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-navigation-bar" },
                bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.window_background; } }, std::move(column));
        }

        std::unique_ptr<luil::ui_element> build_banner_bar(const settings_view& view)
        {
            std::u8string text { tr(u8"settings.banner.save_failed") };
            if (view.read_only)
                text = tr(u8"settings.banner.read_only");
            auto row { make_row(u8"settings-banner-row", 8.0f) };
            row->add(std::make_unique<icon_element>(luil::ui_element_id { kind_label, u8"settings-banner-icon" }, codicons::icon_warning, tone_color(tone::warning)), 20.0f);
            row->add_flexible(make_text(u8"settings-banner-text", text, text_style { .size = 12.0f }));
            return std::make_unique<bar_element>(luil::ui_element_id { kind_banner, u8"settings-banner" },
                bar_style { .background = [](const luil::ui_color_palette& palette) { return luil::with_alpha(palette.warning_accent, 0.14f); } }, std::move(row));
        }
    } // namespace

    std::unique_ptr<luil::ui_element> build_settings_dialog(const settings_view& view, const float window_width, const float window_height, feedback_sources& sources)
    {
        const float width { std::min(dialog_maximum_width, std::max(window_width - dialog_margin * 2.0f, 320.0f)) };
        const float height { std::min(dialog_maximum_height, std::max(window_height - dialog_margin * 2.0f, 240.0f)) };

        luil::modal_host_config config {};
        config.owner = u8"settings-dialog";
        config.name = tr(u8"settings.common.title");
        config.dismiss = action_of(message(close_settings_intent {}));
        config.focus_entry = luil::ui_element_id { luil::ui_element_kind::list, u8"settings-pages" };
        config.surface = floating_panel_config(8.0f, 0.4f);
        config.content_width = width;
        config.content_height = height;
        auto host { std::make_unique<luil::modal_host_element>(std::move(config)) };

        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"settings-column" }, luil::stack_config {}) };
        luil::dialog_caption_config caption {
            .owner = u8"settings", .title = tr(u8"settings.common.title"), .close = action_of(message(close_settings_intent {})), .close_tooltip = tr(u8"settings.common.close")
        };
        const float caption_height { luil::dialog_caption_element::height_for(caption) };
        column->add(std::make_unique<luil::dialog_caption_element>(std::move(caption)), caption_height);

        auto body { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"settings-body" }, luil::stack_config { .direction = luil::stack_direction::row }) };
        const bool compact_navigation { width < 680.0f };
        const float navigation_extent { compact_navigation ? 52.0f : navigation_width };
        body->add(build_navigation(view, compact_navigation), navigation_extent);

        auto content { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"settings-content" }, luil::stack_config {}) };
        if (view.read_only || view.unsaved)
            content->add(build_banner_bar(view), banner_height);

        page_column page { u8"settings-page" };
        const float content_width { width - navigation_extent - scroll_inset - luil::scroll_area_bar_width - content_padding - content_right_padding - section_padding * 2.0f };
        switch (view.page)
        {
        case settings_page::general:
            build_general(page, view, content_width);
            break;
        case settings_page::diff_tools:
            build_diff_tools(page, view, content_width);
            break;
        case settings_page::ssh:
            build_ssh(page, view, content_width);
            break;
        case settings_page::signing:
            build_signing_page(page, view.signing, content_width);
            break;
        case settings_page::hosting:
            build_hosting(page, view, content_width);
            break;
        case settings_page::profiles:
            if (view.profiles.has_value())
            {
                built_settings_page built { build_profiles_page(*view.profiles, content_width) };
                page.add_unframed(std::move(built.content), built.height);
            }
            break;
        case settings_page::shortcuts:
            if (view.shortcuts.has_value())
            {
                built_settings_page built { build_shortcuts_page(*view.shortcuts, content_width) };
                page.add_unframed(std::move(built.content), built.height);
            }
            break;
        }
        luil::scroll_area_config scroll {};
        scroll.owner = u8"settings-scroll";
        scroll.content_height = page.height() + large_gap;
        scroll.scroll_offset = view.scroll;
        scroll.scroll = [](const float delta) { return message(settings_scroll_intent { .value = delta, .absolute = false }); };
        scroll.scroll_to = [](const float offset) { return message(settings_scroll_intent { .value = offset, .absolute = true }); };
        scroll.edges = luil::scroll_edges { .shadows = true };
        auto area { std::make_unique<luil::scroll_area_element>(std::move(scroll)) };
        luil::ui_element_id padded_id { kind_layout, u8"settings-page-padding" };
        const luil::stack_config padded_config { .padding = luil::edge_insets { content_padding, 0.0f, content_right_padding, large_gap } };
        auto padded { std::make_unique<luil::stack_element>(std::move(padded_id), padded_config) };
        padded->add(page.take(), page.height());
        area->set_content(std::move(padded));
        sources.settings = area.get();
        const luil::stack_config scroll_padding { .padding = luil::edge_insets { 0.0f, scroll_inset, scroll_inset, scroll_inset } };
        auto inset { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"settings-scroll-inset" }, scroll_padding) };
        inset->add_flexible(std::move(area));
        content->add_flexible(std::move(inset));

        content->add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-footer-bar" }, bar_style { .divider_top = true },
                         make_text(u8"settings-message", view.message, text_style { .size = 11.0f, .color = dim })),
            footer_height);
        body->add_flexible(std::move(content));
        column->add_flexible(std::move(body));
        host->set_content(std::move(column));
        return host;
    }
} // namespace cyella::ui
