#include "presentation/ui/settings/settings_ui.h"

#include "application/app_messages.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/settings/settings_pages_ui.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"

#include "luil/ui/choice_group_element.h"
#include "luil/ui/dialog_elements.h"
#include "luil/ui/modal_host_element.h"
#include "luil/ui/scroll_area_element.h"
#include "luil/ui/stack_element.h"

#include <algorithm>
#include <utility>

namespace cyella::ui {
    namespace {
        constexpr float dialog_maximum_width { 900.0f };
        constexpr float dialog_maximum_height { 700.0f };
        constexpr float dialog_margin { 24.0f };
        constexpr float navigation_width { 170.0f };
        constexpr float content_padding { 16.0f };
        constexpr float banner_height { 34.0f };
        constexpr float footer_height { 26.0f };
        constexpr float section_height { 30.0f };
        constexpr float line_height { 18.0f };
        constexpr float label_height { 17.0f };
        constexpr float input_height { 28.0f };
        constexpr float button_height { 26.0f };
        constexpr float row_height { 30.0f };
        // 미리보기가 한 줄씩 보이는 인자의 상한이다.
        constexpr std::size_t maximum_preview_arguments { 8 };
        constexpr float small_gap { 6.0f };
        constexpr float large_gap { 14.0f };

        const color_selector dim { [](const luil::ui_color_palette& palette) { return palette.secondary_foreground; } };

        color_selector tone_color(const tone value)
        {
            return [value](const luil::ui_color_palette& palette) {
                switch (value)
                {
                case tone::good:
                    return success_color(palette);
                case tone::warning:
                    return palette.warning_accent;
                case tone::bad:
                    return palette.error_accent;
                case tone::neutral:
                    break;
                }
                return palette.secondary_foreground;
            };
        }

        std::unique_ptr<luil::stack_element> make_column(std::u8string owner)
        {
            luil::ui_element_id id { kind_layout, std::move(owner) };
            const luil::stack_config config { .padding = luil::edge_insets { 0.0f, 0.0f, 0.0f, 0.0f } };
            return std::make_unique<luil::stack_element>(std::move(id), config);
        }

        // 한 쪽(page)의 내용을 쌓는다. 측정 단계가 없으므로 담을 때 높이를 함께 더해 두었다가 흘리는 창에 알려 준다.
        class page_column
        {
        public:
            explicit page_column(std::u8string owner)
                : column_ { make_column(std::move(owner)) }
            {}

            void add(std::unique_ptr<luil::ui_element> child, const float height)
            {
                column_->add(std::move(child), height);
                height_ += height;
            }

            void gap(const float height)
            {
                column_->add_gap(height);
                height_ += height;
            }

            [[nodiscard]] float height() const noexcept
            {
                return height_;
            }

            [[nodiscard]] std::unique_ptr<luil::ui_element> take()
            {
                return std::move(column_);
            }

        private:
            std::unique_ptr<luil::stack_element> column_;
            float height_ { 0.0f };
        };

        std::unique_ptr<luil::stack_element> make_row(std::u8string owner, const float spacing = small_gap)
        {
            luil::stack_config config { .direction = luil::stack_direction::row, .spacing = spacing };
            config.cross_alignment = luil::stack_alignment::center;
            return std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, std::move(owner) }, config);
        }

        std::unique_ptr<luil::ui_element> section_title(std::u8string owner, std::u8string text)
        {
            return make_text(std::move(owner), std::move(text), text_style { .size = 13.0f, .bold = true });
        }

        std::unique_ptr<luil::ui_element> help_line(std::u8string owner, std::u8string text, color_selector color = dim)
        {
            return make_text(std::move(owner), std::move(text), text_style { .size = 11.0f, .color = std::move(color) });
        }

        std::unique_ptr<luil::ui_element> field_label(std::u8string owner, std::u8string text)
        {
            return make_text(std::move(owner), std::move(text), text_style { .size = 11.0f, .color = dim, .bold = true });
        }

        std::unique_ptr<luil::ui_element> code_line(std::u8string owner, std::u8string text)
        {
            return make_text(std::move(owner), std::move(text), text_style { .size = 11.0f, .color = dim, .code = true });
        }

        std::unique_ptr<luil::ui_element> input_of(const luil::ui_element_kind kind, std::u8string owner, const luil::text_input_view& view, std::u8string placeholder)
        {
            return std::make_unique<luil::text_input_element>(luil::ui_element_id { kind, std::move(owner) }, view, luil::text_input_config { .placeholder = std::move(placeholder) });
        }

        std::unique_ptr<luil::ui_element> button_of(
            std::u8string owner, std::u8string text, luil::input_action action, const bool enabled = true, const luil::text_button_visual visual = luil::text_button_visual::normal)
        {
            return make_text_button(std::move(owner), std::move(text), visual, std::move(action), enabled);
        }

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
                row->add(button_of(owner + u8"-cancel", u8"취소", message(cancel_operation_intent { task.id }), true, luil::text_button_visual::danger), { .length = 56.0f, .cross_length = 22.0f });
                page.add(std::move(row), line_height + 4.0f);
            }
            else
                page.add(help_line(owner + u8"-status", std::move(text), tone_color(value)), line_height + 4.0f);
            page.add(help_line(owner + u8"-detail", task.detail, dim), line_height);
        }

        // ------------------------------------------------------------------ 일반

        void build_general(page_column& page, const settings_view& view)
        {
            page.add(section_title(u8"settings-section-appearance", u8"화면"), section_height);
            page.add(field_label(u8"settings-theme-label", u8"테마"), label_height);
            luil::choice_group_config theme {};
            theme.owner = u8"settings-theme";
            theme.style = luil::choice_style::toggle;
            theme.name = u8"테마";
            theme.items = { { u8"system", u8"시스템" }, { u8"light", u8"밝게" }, { u8"dark", u8"어둡게" } };
            theme.selected = std::u8string { theme_preference_name(view.theme) };
            theme.select = [](const std::u8string& value) { return message(set_theme_intent { parse_theme_preference(value).value_or(theme_preference::system) }); };
            const float toggle_height { luil::choice_toggle_height };
            auto toggle_row { make_row(u8"settings-theme-row") };
            toggle_row->add(std::make_unique<luil::choice_group_element>(std::move(theme)), { .length = 260.0f, .cross_length = toggle_height });
            page.add(std::move(toggle_row), input_height);
            page.add(help_line(u8"settings-theme-help", u8"'시스템'을 고르면 Windows의 앱 모드를 따릅니다."), line_height);
            page.gap(large_gap);

            const git_settings_view& git { view.git };
            page.add(section_title(u8"settings-section-git", u8"Git"), section_height);
            if (git.problem.empty())
            {
                page.add(field_label(u8"settings-git-using-label", u8"사용 중인 git"), label_height);
                page.add(make_text(u8"settings-git-using", git.executable, text_style { .size = 12.0f, .code = true }), line_height);
                page.add(help_line(u8"settings-git-version", git.version + u8" · " + git.origin), line_height);
            }
            else
            {
                page.add(make_text(u8"settings-git-problem", git.problem, text_style { .size = 12.0f, .color = tone_color(tone::bad) }), line_height);
                page.add(help_line(u8"settings-git-version", u8"아래에서 git 경로를 직접 지정할 수 있습니다."), line_height);
            }
            page.gap(small_gap);
            page.add(field_label(u8"settings-git-path-label", u8"git 실행 파일 경로 (비워 두면 자동 탐색)"), label_height);
            auto path_row { make_row(u8"settings-git-path-row") };
            path_row->add_flexible(input_of(kind_settings_git_path_input, u8"settings-git-path", git.path, u8"C:\\Program Files\\Git\\cmd\\git.exe"));
            path_row->add(
                button_of(u8"settings-git-browse", u8"찾아보기…", ui_command(static_cast<std::uint32_t>(ui_command_id::choose_git_executable))), { .length = 88.0f, .cross_length = button_height });
            path_row->add(
                button_of(u8"settings-git-apply", u8"적용", message(apply_git_path_intent {}), git.can_apply, luil::text_button_visual::accent), { .length = 56.0f, .cross_length = button_height });
            page.add(std::move(path_row), input_height);
            if (git.validation.empty() == false)
                page.add(help_line(u8"settings-git-validation", git.validation, tone_color(tone::bad)), line_height);
            else if (git.dirty)
                page.add(help_line(u8"settings-git-validation", u8"[적용]을 누르면 저장하고 다음 실행부터 씁니다."), line_height);
            else
                page.add(help_line(u8"settings-git-validation", git.configured ? u8"직접 지정한 경로입니다." : u8"지정한 경로가 없어 자동으로 찾습니다."), line_height);
            auto reset_row { make_row(u8"settings-git-reset-row") };
            reset_row->add(button_of(u8"settings-git-reset", u8"자동 탐색으로 되돌리기", message(reset_git_path_intent {}), git.configured),
                { .length = text_button_width(u8"자동 탐색으로 되돌리기"), .cross_length = button_height });
            page.add(std::move(reset_row), input_height);
        }

        // ------------------------------------------------------------------ diff 도구

        std::unique_ptr<luil::ui_element> build_tool_row(const tool_row_view& tool, const bool editing)
        {
            const std::u8string key { number_text(tool.index) };
            auto row { make_row(u8"settings-tool-row-" + key, 8.0f) };
            row->add(make_text(u8"settings-tool-extensions-" + key, tool.extensions, text_style { .size = 12.0f, .bold = true }), 130.0f);
            row->add_flexible(make_text(u8"settings-tool-executable-" + key, tool.executable, text_style { .size = 11.0f, .code = true }));
            row->add_flexible(make_text(u8"settings-tool-arguments-" + key, tool.arguments, text_style { .size = 11.0f, .color = dim, .code = true }));
            row->add(button_of(u8"settings-tool-edit-" + key, u8"편집", message(edit_tool_intent { tool.index }), editing == false), { .length = 52.0f, .cross_length = button_height });
            row->add(button_of(u8"settings-tool-remove-" + key, u8"삭제", message(remove_tool_intent { tool.index }), editing == false, luil::text_button_visual::danger),
                { .length = 52.0f, .cross_length = button_height });
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-tool-bar-" + key }, bar_style { .divider_bottom = true }, std::move(row));
        }

        void build_tool_form(page_column& page, const tool_edit_view& edit)
        {
            page_column form { u8"settings-tool-form-column" };
            form.add(section_title(u8"settings-tool-form-title", edit.is_new ? u8"도구 추가" : u8"도구 편집"), section_height);
            form.add(field_label(u8"settings-tool-extensions-label", u8"확장자 (쉼표나 공백으로 구분)"), label_height);
            form.add(input_of(kind_settings_tool_extensions_input, u8"settings-tool-extensions-input", edit.extensions, u8".png, .psd"), input_height);
            form.gap(small_gap);
            form.add(field_label(u8"settings-tool-executable-label", u8"실행 파일"), label_height);
            auto executable_row { make_row(u8"settings-tool-executable-row") };
            executable_row->add_flexible(input_of(kind_settings_tool_executable_input, u8"settings-tool-executable-input", edit.executable, u8"C:\\Tools\\imgdiff.exe"));
            executable_row->add(
                button_of(u8"settings-tool-browse", u8"찾아보기…", ui_command(static_cast<std::uint32_t>(ui_command_id::choose_tool_executable))), { .length = 88.0f, .cross_length = button_height });
            form.add(std::move(executable_row), input_height);
            form.gap(small_gap);
            form.add(field_label(u8"settings-tool-arguments-label", u8"인자 템플릿 (토큰을 누르면 끝에 붙습니다)"), label_height);
            form.add(input_of(kind_settings_tool_arguments_input, u8"settings-tool-arguments-input", edit.arguments, u8"{left} {right}"), input_height);
            auto chips { make_row(u8"settings-tool-chips") };
            for (const std::u8string_view token : { u8"{left}", u8"{right}", u8"{base}", u8"{path}" })
                chips->add(
                    button_of(u8"settings-tool-chip-" + std::u8string { token.substr(1, token.size() - 2) }, std::u8string { token }, message(insert_tool_token_intent { std::u8string { token } })),
                    { .length = text_button_width(token) + 6.0f, .cross_length = button_height });
            chips->add_flexible_gap();
            form.add(std::move(chips), input_height);
            form.gap(small_gap);
            form.add(field_label(u8"settings-tool-preview-label", u8"미리보기: 샘플 경로로 풀어 실행하는 명령"), label_height);
            if (edit.preview.empty())
                form.add(code_line(u8"settings-tool-preview", u8"(템플릿을 풀 수 없습니다)"), line_height);
            else
            {
                // 풀린 인자를 한 줄씩 보인다. 공백이 든 경로가 한 인자로 남는지 눈으로 확인하려는 것이다.
                form.add(code_line(u8"settings-tool-preview",
                             u8"실행  " + (edit.executable.displayed_text().empty() ? std::u8string { u8"<실행 파일>" } : std::u8string { edit.executable.displayed_text() })),
                    line_height);
                const std::size_t shown { std::min<std::size_t>(edit.preview_arguments.size(), maximum_preview_arguments) };
                for (std::size_t index { 0 }; index < shown; ++index)
                    form.add(code_line(u8"settings-tool-preview-" + number_text(index), u8"인자 " + number_text(index + 1) + u8"  " + edit.preview_arguments[index]), line_height);
                if (edit.preview_arguments.size() > shown)
                    form.add(code_line(u8"settings-tool-preview-more", u8"외 " + number_text(edit.preview_arguments.size() - shown) + u8"개"), line_height);
            }
            if (edit.error.empty() == false)
                form.add(help_line(u8"settings-tool-error", edit.error, tone_color(tone::bad)), line_height);
            else
                form.add(help_line(u8"settings-tool-error", edit.warning, tone_color(tone::warning)), line_height);
            form.gap(small_gap);
            auto buttons { make_row(u8"settings-tool-buttons") };
            buttons->add_flexible_gap();
            buttons->add(button_of(u8"settings-tool-cancel", u8"취소", message(cancel_tool_edit_intent {})), { .length = 64.0f, .cross_length = button_height });
            buttons->add(
                button_of(u8"settings-tool-save", u8"저장", message(save_tool_intent {}), edit.can_save, luil::text_button_visual::accent), { .length = 64.0f, .cross_length = button_height });
            form.add(std::move(buttons), input_height);

            const float height { form.height() + 24.0f };
            auto padded { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"settings-tool-form-padding" }, luil::stack_config { .padding = luil::edge_insets::all(12.0f) }) };
            padded->add_flexible(form.take());
            page.add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-tool-form" },
                         bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.window_background; } }, std::move(padded)),
                height);
        }

        // diff를 보는 방식이다: 처음 열 때의 보기와 hunk 둘레의 문맥 줄 수.
        void build_diff_view_settings(page_column& page, const settings_view& view)
        {
            page.add(section_title(u8"settings-section-diff-view", u8"diff 보기"), section_height);
            page.add(field_label(u8"settings-diff-layout-label", u8"기본 보기"), label_height);
            luil::choice_group_config layout {};
            layout.owner = u8"settings-diff-layout";
            layout.style = luil::choice_style::toggle;
            layout.name = u8"기본 diff 보기";
            layout.items = { { u8"inline", u8"Inline" }, { u8"split", u8"좌우 분할" } };
            layout.selected = std::u8string { diff_layout_preference_name(view.diff.layout) };
            layout.select = [](const std::u8string& value) { return message(set_diff_layout_default_intent { parse_diff_layout_preference(value).value_or(diff_layout_preference::inline_view) }); };
            auto layout_row { make_row(u8"settings-diff-layout-row") };
            layout_row->add(std::make_unique<luil::choice_group_element>(std::move(layout)), { .length = 260.0f, .cross_length = luil::choice_toggle_height });
            page.add(std::move(layout_row), input_height);
            page.add(help_line(u8"settings-diff-layout-help", u8"고르면 열려 있는 diff도 바로 이 보기로 바뀝니다. diff 머리의 단추로는 그 diff만 바꿉니다."), line_height);
            page.gap(small_gap);

            page.add(field_label(u8"settings-diff-context-label", u8"문맥 줄 수 (git diff -U<n>)"), label_height);
            luil::choice_group_config context {};
            context.owner = u8"settings-diff-context";
            context.style = luil::choice_style::toggle;
            context.name = u8"문맥 줄 수";
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
            context_row->add(std::make_unique<luil::choice_group_element>(std::move(context)), { .length = 320.0f, .cross_length = luil::choice_toggle_height });
            page.add(std::move(context_row), input_height);
            page.add(help_line(u8"settings-diff-context-help", u8"다음에 여는 diff부터 적용합니다. 0이면 바뀐 줄만 보이고 줄 단위 stage에는 --unidiff-zero를 씁니다."), line_height);
            page.gap(large_gap);
        }

        void build_diff_tools(page_column& page, const settings_view& view)
        {
            build_diff_view_settings(page, view);
            const bool editing { view.edit.has_value() };
            page.add(section_title(u8"settings-section-tools", u8"확장자별 외부 diff 도구"), section_height);
            page.add(help_line(u8"settings-tools-help-1", u8"바이너리 파일을 열 때 확장자로 도구를 찾아 실행합니다."), line_height);
            page.add(help_line(u8"settings-tools-help-2", u8"두 판은 git에서 임시 파일로 꺼내 인자 템플릿의 {left}·{right}에 넣습니다."), line_height);
            page.gap(small_gap);
            if (view.tools.empty())
                page.add(help_line(u8"settings-tools-empty", u8"등록한 도구가 없습니다. [도구 추가]로 확장자와 실행 파일을 정하세요."), row_height);
            else
            {
                auto header { make_row(u8"settings-tools-header", 8.0f) };
                header->add(field_label(u8"settings-tools-header-extensions", u8"확장자"), 130.0f);
                header->add_flexible(field_label(u8"settings-tools-header-executable", u8"실행 파일"));
                header->add_flexible(field_label(u8"settings-tools-header-arguments", u8"인자 템플릿"));
                header->add_gap(112.0f);
                page.add(std::move(header), line_height + 4.0f);
                for (const tool_row_view& tool : view.tools)
                    page.add(build_tool_row(tool, editing), row_height);
            }
            page.gap(small_gap);
            auto add_row { make_row(u8"settings-tools-add-row") };
            add_row->add(
                button_of(u8"settings-tool-add", u8"도구 추가", message(add_tool_intent {}), editing == false, luil::text_button_visual::accent), { .length = 92.0f, .cross_length = button_height });
            page.add(std::move(add_row), input_height);
            if (view.edit.has_value())
            {
                page.gap(small_gap);
                build_tool_form(page, *view.edit);
            }
            page.gap(large_gap);
            page.add(section_title(u8"settings-section-fallback", u8"표에 없는 확장자"), section_height);
            page.add(help_line(u8"settings-fallback-help-1", u8"표에 없으면 git의 diff.tool을 써서 git difftool로 엽니다."), line_height);
            page.add(help_line(u8"settings-fallback-help-2", u8"diff.tool도 없으면 이 화면에서 도구를 지정하라고 안내합니다. {base}는 병합용이라 지금은 비어 그 인자가 빠집니다."), line_height);
        }

        // ------------------------------------------------------------------ SSH

        void build_ssh(page_column& page, const settings_view& view)
        {
            const ssh_view& ssh { view.ssh };
            page.add(section_title(u8"settings-section-agent", u8"ssh-agent"), section_height);
            auto service_row { make_row(u8"settings-agent-row") };
            if (ssh.loaded == false)
                service_row->add_flexible(make_text(u8"settings-agent-status", u8"상태를 읽는 중…", text_style { .size = 12.0f, .color = dim }));
            else
                service_row->add_flexible(make_text(u8"settings-agent-status", u8"서비스: " + ssh.service_status, text_style { .size = 12.0f, .color = tone_color(ssh.service_tone), .bold = true }));
            service_row->add(button_of(u8"settings-ssh-refresh", ssh.loading ? u8"읽는 중…" : u8"새로고침", message(refresh_ssh_intent {}), ssh.loading == false),
                { .length = 84.0f, .cross_length = button_height });
            page.add(std::move(service_row), input_height);
            page.add(help_line(u8"settings-agent-help", ssh.service_help), line_height);
            page.gap(small_gap);
            page.add(field_label(u8"settings-agent-identities-label", u8"agent에 등록된 키 (ssh-add -l)"), label_height);
            if (ssh.identities.empty())
                page.add(help_line(u8"settings-agent-message", ssh.agent_message), line_height);
            else
                for (std::size_t index { 0 }; index < ssh.identities.size(); ++index)
                    page.add(code_line(u8"settings-agent-identity-" + number_text(index), ssh.identities[index]), line_height);
            page.gap(large_gap);

            page.add(section_title(u8"settings-section-keys", u8"키 파일"), section_height);
            page.add(help_line(u8"settings-keys-directory", ssh.ssh_directory.empty() ? u8"사용자 폴더의 .ssh를 찾지 못했습니다." : ssh.ssh_directory), line_height);
            if (ssh.keys.empty())
                page.add(help_line(u8"settings-keys-empty", ssh.loaded ? u8"키 쌍(개인 키와 .pub)이 없습니다. 아래에서 새로 만들 수 있습니다." : u8"읽는 중…"), row_height);
            for (const ssh_key_row_view& key : ssh.keys)
            {
                auto row { make_row(u8"settings-key-row-" + key.name, 8.0f) };
                row->add(make_text(u8"settings-key-name-" + key.name, key.name, text_style { .size = 12.0f, .bold = true }), 180.0f);
                row->add_flexible(make_text(u8"settings-key-detail-" + key.name, key.detail, text_style { .size = 11.0f, .color = dim }));
                row->add(
                    button_of(u8"settings-key-copy-" + key.name, u8"공개 키 복사", message(copy_public_key_intent { key.name }), key.can_copy), { .length = 100.0f, .cross_length = button_height });
                page.add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-key-bar-" + key.name }, bar_style { .divider_bottom = true }, std::move(row)), row_height);
            }
            page.gap(large_gap);

            page.add(section_title(u8"settings-section-keygen", u8"키 만들기"), section_height);
            page.add(help_line(u8"settings-keygen-tools",
                         ssh.tools_found ? u8"ssh-keygen: " + ssh.tools_origin + u8". passphrase는 명령줄이 아니라 인증 대화 상자로 묻습니다."
                                         : u8"ssh-keygen을 찾지 못했습니다. OpenSSH 클라이언트나 Git for Windows를 설치하세요.",
                         ssh.tools_found ? dim : tone_color(tone::bad)),
                line_height);
            page.add(field_label(u8"settings-keygen-name-label", u8"파일 이름 (ed25519)"), label_height);
            page.add(input_of(kind_settings_key_name_input, u8"settings-keygen-name", ssh.key_name, u8"id_ed25519"), input_height);
            page.add(field_label(u8"settings-keygen-comment-label", u8"주석"), label_height);
            page.add(input_of(kind_settings_key_comment_input, u8"settings-keygen-comment", ssh.key_comment, u8"you@example.com"), input_height);
            page.add(code_line(u8"settings-keygen-command", ssh.keygen_command), line_height);
            page.add(help_line(u8"settings-keygen-error", ssh.keygen_error, tone_color(tone::bad)), line_height);
            auto create_row { make_row(u8"settings-keygen-row") };
            create_row->add(button_of(u8"settings-keygen-create", u8"키 만들기", message(create_ssh_key_intent {}), ssh.can_create, luil::text_button_visual::accent),
                { .length = 96.0f, .cross_length = button_height });
            page.add(std::move(create_row), input_height);
            add_task_lines(page, u8"settings-keygen", ssh.keygen);
            page.gap(large_gap);

            page.add(section_title(u8"settings-section-test", u8"연결 시험"), section_height);
            page.add(field_label(u8"settings-test-host-label", u8"호스트 ([계정@]호스트[:포트], 계정 기본값 git)"), label_height);
            auto test_row { make_row(u8"settings-test-row") };
            test_row->add_flexible(input_of(kind_settings_test_host_input, u8"settings-test-host", ssh.test_host, u8"github.com"));
            test_row->add(
                button_of(u8"settings-test-run", u8"시험", message(test_ssh_connection_intent {}), ssh.can_test, luil::text_button_visual::accent), { .length = 64.0f, .cross_length = button_height });
            page.add(std::move(test_row), input_height);
            page.add(code_line(u8"settings-test-command", ssh.test_command), line_height);
            page.add(help_line(u8"settings-test-error", ssh.test_error, tone_color(tone::bad)), line_height);
            add_task_lines(page, u8"settings-test", ssh.test);
        }

        // ------------------------------------------------------------------ 틀

        std::unique_ptr<luil::ui_element> build_navigation(const settings_view& view)
        {
            luil::ui_element_id column_id { kind_layout, u8"settings-navigation" };
            const luil::stack_config column_config { .spacing = 4.0f, .padding = luil::edge_insets::all(10.0f) };
            auto column { std::make_unique<luil::stack_element>(std::move(column_id), column_config) };
            const auto entry = [&](std::u8string owner, std::u8string text, const settings_page page, const char32_t glyph) {
                column->add(make_text_button(std::move(owner), std::move(text), view.page == page ? luil::text_button_visual::accent : luil::text_button_visual::normal,
                                message(select_settings_page_intent { page }), true, glyph),
                    30.0f);
            };
            entry(u8"settings-page-general", u8"일반", settings_page::general, codicons::icon_settings_gear);
            entry(u8"settings-page-diff-tools", u8"diff", settings_page::diff_tools, codicons::icon_diff);
            entry(u8"settings-page-ssh", u8"SSH", settings_page::ssh, codicons::icon_key);
            entry(u8"settings-page-profiles", u8"사용자 프로필", settings_page::profiles, codicons::icon_person);
            entry(u8"settings-page-shortcuts", u8"단축키", settings_page::shortcuts, codicons::icon_keyboard);
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-navigation-bar" },
                bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.window_background; } }, std::move(column));
        }

        std::unique_ptr<luil::ui_element> build_banner_bar(const settings_view& view)
        {
            std::u8string text { u8"마지막 저장이 실패했습니다. 다음 변경이나 닫을 때 다시 저장을 시도합니다." };
            if (view.read_only)
                text = u8"설정 파일을 읽지 못해 이번 실행 동안은 저장하지 않습니다. 바꾼 값은 앱을 닫으면 사라집니다.";
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
        config.name = u8"환경 설정";
        config.dismiss = action_of(message(close_settings_intent {}));
        config.focus_entry = luil::ui_element_id { kind_text_button, u8"settings-page-general" };
        config.surface = floating_panel_config(8.0f, 0.4f);
        config.content_width = width;
        config.content_height = height;
        auto host { std::make_unique<luil::modal_host_element>(std::move(config)) };

        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"settings-column" }, luil::stack_config {}) };
        luil::dialog_caption_config caption { .owner = u8"settings", .title = u8"환경 설정", .close = action_of(message(close_settings_intent {})), .close_tooltip = u8"닫기" };
        const float caption_height { luil::dialog_caption_element::height_for(caption) };
        column->add(std::make_unique<luil::dialog_caption_element>(std::move(caption)), caption_height);

        auto body { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"settings-body" }, luil::stack_config { .direction = luil::stack_direction::row }) };
        body->add(build_navigation(view), navigation_width);

        auto content { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"settings-content" }, luil::stack_config {}) };
        if (view.read_only || view.unsaved)
            content->add(build_banner_bar(view), banner_height);

        page_column page { u8"settings-page" };
        switch (view.page)
        {
        case settings_page::general:
            build_general(page, view);
            break;
        case settings_page::diff_tools:
            build_diff_tools(page, view);
            break;
        case settings_page::ssh:
            build_ssh(page, view);
            break;
        case settings_page::profiles:
            if (view.profiles.has_value())
            {
                built_settings_page built { build_profiles_page(*view.profiles) };
                page.add(std::move(built.content), built.height);
            }
            break;
        case settings_page::shortcuts:
            if (view.shortcuts.has_value())
            {
                built_settings_page built { build_shortcuts_page(*view.shortcuts) };
                page.add(std::move(built.content), built.height);
            }
            break;
        }
        luil::scroll_area_config scroll {};
        scroll.owner = u8"settings-scroll";
        scroll.content_height = page.height() + content_padding;
        scroll.scroll_offset = view.scroll;
        scroll.scroll = [](const float delta) { return message(settings_scroll_intent { .value = delta, .absolute = false }); };
        scroll.scroll_to = [](const float offset) { return message(settings_scroll_intent { .value = offset, .absolute = true }); };
        scroll.edges = luil::scroll_edges { .shadows = true };
        auto area { std::make_unique<luil::scroll_area_element>(std::move(scroll)) };
        luil::ui_element_id padded_id { kind_layout, u8"settings-page-padding" };
        const luil::stack_config padded_config { .padding = luil::edge_insets { content_padding, 10.0f, 12.0f, 0.0f } };
        auto padded { std::make_unique<luil::stack_element>(std::move(padded_id), padded_config) };
        padded->add(page.take(), page.height());
        area->set_content(std::move(padded));
        sources.settings = area.get();
        content->add_flexible(std::move(area));

        content->add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-footer-bar" }, bar_style { .divider_top = true },
                         make_text(u8"settings-message", view.message, text_style { .size = 11.0f, .color = dim })),
            footer_height);
        body->add_flexible(std::move(content));
        column->add_flexible(std::move(body));
        host->set_content(std::move(column));
        return host;
    }
} // namespace cyella::ui
