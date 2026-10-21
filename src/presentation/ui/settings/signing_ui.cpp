#include "presentation/ui/settings/signing_ui.h"

#include "application/app_messages.h"
#include "domain/text_catalog.h"
#include "presentation/settings/signing_intents.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/segmented_control.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"

#include "luil/ui/choice_group_element.h"

#include <algorithm>
#include <utility>

namespace cyella::ui {
    namespace {
        using namespace pages;

        // 설정 표의 열 폭이다 (논리 픽셀). 이름·적용되는 값·출처는 고정이고 이 저장소·전역 열이 남은 폭을 나눈다.
        constexpr float table_name_width { 188.0f };
        constexpr float table_effective_width { 128.0f };
        constexpr float table_origin_width { 66.0f };
        constexpr float table_row_height { 22.0f };

        std::unique_ptr<luil::ui_element> cell(std::u8string owner, std::u8string text, const bool bold = false, const bool code = false)
        {
            return make_text(std::move(owner), std::move(text), text_style { .size = 11.0f, .color = bold ? color_selector {} : dim, .bold = bold, .code = code });
        }

        std::unique_ptr<luil::ui_element> choice_row(std::u8string owner, std::u8string name, std::vector<luil::choice_item> items, std::u8string selected, luil::choice_message_factory select)
        {
            luil::choice_group_config config {};
            config.owner = owner;
            config.style = luil::choice_style::toggle;
            config.name = std::move(name);
            config.items = std::move(items);
            config.selected = std::move(selected);
            config.select = std::move(select);
            auto row { make_row(owner + u8"-row") };
            row->add_flexible(make_segmented_control(std::move(config)));
            return row;
        }

        void build_scope(page_column& page, const signing_page_view& view, const float content_width)
        {
            page.add(section_title(u8"settings-signing-section-scope", tr(u8"settings.signing.section")), section_height);
            add_help(page, u8"settings-signing-intro-1", tr(u8"settings.signing.intro"), content_width);
            page.gap(small_gap);

            auto header { make_row(u8"settings-signing-scope-header") };
            header->add_flexible(field_label(u8"settings-signing-scope-label", tr(u8"settings.signing.scope_label")));
            header->add(
                button_of(u8"settings-signing-refresh", view.loading ? tr(u8"settings.common.loading") : tr(u8"settings.common.refresh"), message(refresh_signing_intent {}), view.loading == false),
                { .length = 84.0f, .cross_length = button_height });
            page.add(std::move(header), input_height);

            std::vector<luil::choice_item> scopes {};
            if (view.repository)
                scopes.push_back({ u8"local", tr(u8"settings.signing.scope_local") });
            scopes.push_back({ u8"global", tr(u8"settings.signing.scope_global") });
            page.add(choice_row(u8"settings-signing-scope", tr(u8"settings.signing.scope_name"), std::move(scopes), view.global_scope ? u8"global" : u8"local",
                         [](const std::u8string& value) { return message(select_signing_scope_intent { value == u8"global" }); }),
                input_height);
            if (view.repository)
                page.add(code_line(u8"settings-signing-repository", tr(u8"settings.signing.repository_path", { { u8"path", view.repository_path } })), line_height);
            else
                page.add(help_line(u8"settings-signing-repository", tr(u8"settings.signing.no_repository")), line_height);
            page.add(help_line(u8"settings-signing-scope-help", view.global_scope ? tr(u8"settings.signing.scope_global_help") : tr(u8"settings.signing.scope_local_help")), line_height);

            if (view.confirming)
            {
                auto box { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"settings-signing-confirm-column" }, luil::stack_config { .spacing = 4.0f }) };
                box->add(make_text(u8"settings-signing-confirm-text", view.confirm_text, text_style { .size = 12.0f, .color = tone_color(tone::warning), .bold = true }), line_height + 4.0f);
                const std::u8string apply_label { tr(u8"settings.signing.apply_global") };
                auto buttons { make_row(u8"settings-signing-confirm-buttons") };
                buttons->add(button_of(u8"settings-signing-confirm-apply", apply_label, message(confirm_global_signing_intent {}), view.writing == false, luil::text_button_visual::danger),
                    { .length = text_button_width(apply_label) + 8.0f, .cross_length = button_height });
                buttons->add(
                    button_of(u8"settings-signing-confirm-cancel", tr(u8"settings.common.cancel"), message(cancel_global_signing_intent {})), { .length = 64.0f, .cross_length = button_height });
                buttons->add_flexible_gap();
                box->add(std::move(buttons), input_height);
                page.gap(small_gap);
                page.add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-signing-confirm" },
                             bar_style { .background = [](const luil::ui_color_palette& palette) { return luil::with_alpha(palette.warning_accent, 0.14f); } }, std::move(box)),
                    line_height + 4.0f + input_height + 8.0f);
            }
            page.add(help_line(u8"settings-signing-message", view.message, tone_color(view.message_error ? tone::bad : tone::neutral)), line_height);
        }

        void build_table(page_column& page, const signing_page_view& view, const float content_width)
        {
            page.add(section_title(u8"settings-signing-section-table", tr(u8"settings.signing.table_section")), section_height);
            if (view.loaded == false)
            {
                page.add(help_line(u8"settings-signing-loading", tr(u8"settings.signing.loading_config")), row_height);
                return;
            }
            auto header { make_row(u8"settings-signing-table-header", 8.0f) };
            header->add(field_label(u8"settings-signing-table-name", tr(u8"settings.signing.table_setting")), table_name_width);
            header->add(field_label(u8"settings-signing-table-effective", tr(u8"settings.signing.table_effective")), table_effective_width);
            header->add(field_label(u8"settings-signing-table-origin", tr(u8"settings.signing.origin")), table_origin_width);
            header->add_flexible(field_label(u8"settings-signing-table-local", tr(u8"settings.signing.scope_local")));
            header->add_flexible(field_label(u8"settings-signing-table-global", tr(u8"settings.signing.scope_global")));
            const bool stacked { content_width < 560.0f };
            if (stacked == false)
                page.add(std::move(header), line_height + 2.0f);
            for (const signing_setting_row_view& row : view.rows)
            {
                const std::u8string key { to_u8(signing_key_name(row.key)) };
                if (stacked)
                {
                    page.add(cell(u8"settings-signing-name-" + key, row.name, true, true), table_row_height);
                    const auto value_row = [&](const std::u8string& suffix, const std::u8string& label, const std::u8string& value, const bool bold) {
                        const std::u8string owner { u8"settings-signing-" + suffix + u8"-" + key };
                        auto values { make_row(owner + u8"-row", 8.0f) };
                        values->add(cell(owner + u8"-label", label), 68.0f);
                        values->add_flexible(cell(owner, value, bold, true));
                        page.add(std::move(values), table_row_height);
                    };
                    value_row(u8"effective", tr(u8"settings.signing.effective_short"), row.effective, true);
                    value_row(u8"origin", tr(u8"settings.signing.origin"), row.origin, false);
                    value_row(u8"local", tr(u8"settings.signing.scope_local"), row.local, false);
                    value_row(u8"global", tr(u8"settings.signing.scope_global"), row.global, false);
                    page.gap(small_gap);
                    continue;
                }
                auto line { make_row(u8"settings-signing-row-" + key, 8.0f) };
                line->add(cell(u8"settings-signing-name-" + key, row.name, true, true), table_name_width);
                line->add(cell(u8"settings-signing-effective-" + key, row.effective, true), table_effective_width);
                line->add(cell(u8"settings-signing-origin-" + key, row.origin), table_origin_width);
                line->add_flexible(cell(u8"settings-signing-local-" + key, row.local, false, true));
                line->add_flexible(cell(u8"settings-signing-global-" + key, row.global, false, true));
                page.add(std::move(line), table_row_height);
            }
        }

        // 선택지의 값은 화면 안에서 유일해야 한다(항목이 값으로 id를 얻는다). 그룹마다 접두어를 붙인다.
        std::vector<luil::choice_item> toggle_items(const std::u8string& group)
        {
            return { { group + u8":enabled", tr(u8"settings.signing.enabled") }, { group + u8":disabled", tr(u8"settings.signing.disabled") }, { group + u8":unset", tr(u8"settings.signing.unset") } };
        }

        std::u8string suffix_of(const std::u8string& value)
        {
            const std::size_t colon { value.find(u8':') };
            return colon == std::u8string::npos ? value : value.substr(colon + 1);
        }

        luil::choice_message_factory toggle_handler(const signing_key_id key)
        {
            return [key](const std::u8string& value) {
                signing_toggle_choice choice { signing_toggle_choice::unset };
                const std::u8string choice_name { suffix_of(value) };
                if (choice_name == u8"enabled")
                    choice = signing_toggle_choice::enabled;
                else if (choice_name == u8"disabled")
                    choice = signing_toggle_choice::disabled;
                return message(set_signing_toggle_intent { key, choice });
            };
        }

        void build_toggles(page_column& page, const signing_page_view& view)
        {
            const std::u8string scope { view.global_scope ? tr(u8"settings.signing.scope_global") : tr(u8"settings.signing.scope_local") };
            page.add(section_title(u8"settings-signing-section-enable", tr(u8"settings.signing.enable_section", { { u8"scope", scope } })), section_height);
            page.add(field_label(u8"settings-signing-commit-label", tr(u8"settings.signing.commit_label")), label_height);
            page.add(
                choice_row(u8"settings-signing-commit", u8"commit.gpgsign", toggle_items(u8"commit"), u8"commit:" + view.commit_choice, toggle_handler(signing_key_id::commit_gpgsign)), input_height);
            page.gap(small_gap);
            page.add(field_label(u8"settings-signing-tag-label", tr(u8"settings.signing.tag_label")), label_height);
            page.add(choice_row(u8"settings-signing-tag", u8"tag.gpgsign", toggle_items(u8"tag"), u8"tag:" + view.tag_choice, toggle_handler(signing_key_id::tag_gpgsign)), input_height);
            page.gap(large_gap);

            page.add(section_title(u8"settings-signing-section-format", tr(u8"settings.signing.format_section", { { u8"scope", scope } })), section_height);
            page.add(field_label(u8"settings-signing-format-label", u8"gpg.format"), label_height);
            page.add(choice_row(u8"settings-signing-format", u8"gpg.format",
                         { { u8"format:openpgp", u8"openpgp (gpg)" }, { u8"format:ssh", u8"ssh" }, { u8"format:unset", tr(u8"settings.signing.unset") } }, u8"format:" + view.format_choice,
                         [](const std::u8string& value) {
                             std::optional<signing_format> format {};
                             const std::u8string name { suffix_of(value) };
                             if (name == u8"openpgp")
                                 format = signing_format::openpgp;
                             else if (name == u8"ssh")
                                 format = signing_format::ssh;
                             return message(set_signing_format_intent { format });
                         }),
                input_height);
            page.add(help_line(u8"settings-signing-format-help", tr(u8"settings.signing.format_help")), line_height);
        }

        void build_keys(page_column& page, const signing_page_view& view, const float content_width)
        {
            const std::u8string scope { view.global_scope ? tr(u8"settings.signing.scope_global") : tr(u8"settings.signing.scope_local") };
            page.add(section_title(u8"settings-signing-section-key", tr(u8"settings.signing.key_section", { { u8"scope", scope } })), section_height);
            page.add(help_line(u8"settings-signing-key-help", view.key_format == signing_format::ssh ? tr(u8"settings.signing.key_help_ssh") : tr(u8"settings.signing.key_help_openpgp")), line_height);
            page.add(code_line(u8"settings-signing-key-current", u8"user.signingkey = " + (view.current_key.empty() ? std::u8string { tr(u8"settings.signing.key_not_set") } : view.current_key)),
                line_height);
            if (view.keys.empty())
                page.add(help_line(u8"settings-signing-key-empty", view.keys_message), row_height);
            for (std::size_t index { 0 }; index < view.keys.size(); ++index)
            {
                const signing_key_row_view& key { view.keys[index] };
                const std::u8string suffix { number_text(index) };
                auto row { make_row(u8"settings-signing-key-row-" + suffix, 8.0f) };
                const bool stacked { content_width < 560.0f };
                auto name { make_text(u8"settings-signing-key-name-" + suffix, key.label, text_style { .size = 12.0f, .bold = true }) };
                if (stacked)
                    row->add_flexible(std::move(name));
                else
                    row->add(std::move(name), 200.0f);
                if (stacked == false)
                    row->add_flexible(make_text(u8"settings-signing-key-detail-" + suffix, key.detail, text_style { .size = 11.0f, .color = dim }));
                if (key.selected)
                    row->add(make_text(u8"settings-signing-key-selected-" + suffix, tr(u8"settings.signing.key_in_use"), text_style { .size = 11.0f, .color = tone_color(tone::good), .bold = true }),
                        56.0f);
                row->add(button_of(u8"settings-signing-key-use-" + suffix, tr(u8"settings.signing.use_key"), message(choose_signing_key_intent { key.value }),
                             key.usable && key.selected == false && view.writing == false),
                    { .length = 56.0f, .cross_length = button_height });
                page.add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-signing-key-bar-" + suffix }, bar_style { .divider_bottom = true }, std::move(row)), row_height);
                if (stacked)
                {
                    page.add(help_line(u8"settings-signing-key-detail-" + suffix, key.detail), line_height);
                    page.gap(small_gap);
                }
            }
            const std::u8string clear_label { tr(u8"settings.signing.clear_key") };
            auto clear_row { make_row(u8"settings-signing-key-clear-row") };
            clear_row->add(button_of(u8"settings-signing-key-clear", clear_label, message(choose_signing_key_intent { std::u8string {} }), view.current_key.empty() == false && view.writing == false),
                { .length = text_button_width(clear_label) + 8.0f, .cross_length = button_height });
            page.add(std::move(clear_row), input_height);
        }

        void add_text_setting(page_column& page, const std::u8string& owner, const std::u8string& label, const luil::ui_element_kind kind, const signing_text_view& text, const signing_key_id key,
            const std::u8string& placeholder, const bool writing)
        {
            page.add(field_label(owner + u8"-label", label), label_height);
            auto row { make_row(owner + u8"-row") };
            row->add_flexible(input_of(kind, owner + u8"-input", text.input, placeholder));
            row->add(
                button_of(owner + u8"-apply", tr(u8"settings.common.apply"), message(apply_signing_text_intent { key, false }), text.can_apply && writing == false, luil::text_button_visual::accent),
                { .length = 56.0f, .cross_length = button_height });
            row->add(button_of(owner + u8"-clear", tr(u8"settings.signing.clear"), message(apply_signing_text_intent { key, true }), text.can_clear && writing == false),
                { .length = 64.0f, .cross_length = button_height });
            page.add(std::move(row), input_height);
        }

        void build_programs(page_column& page, const signing_page_view& view)
        {
            const std::u8string scope { view.global_scope ? tr(u8"settings.signing.scope_global") : tr(u8"settings.signing.scope_local") };
            page.add(section_title(u8"settings-signing-section-programs", tr(u8"settings.signing.programs_section", { { u8"scope", scope } })), section_height);

            add_text_setting(page, u8"settings-signing-gpg", tr(u8"settings.signing.gpg_program_label"), kind_settings_signing_gpg_input, view.gpg_program, signing_key_id::gpg_program,
                u8"C:/Program Files/Git/usr/bin/gpg.exe", view.writing);
            if (view.gpg_program.error.empty() == false)
                page.add(help_line(u8"settings-signing-gpg-error", view.gpg_program.error, tone_color(tone::bad)), line_height);
            else
                page.add(help_line(u8"settings-signing-gpg-error", view.gpg_program.warning, tone_color(tone::warning)), line_height);
            const std::u8string default_label { tr(u8"settings.signing.use_default_gpg") };
            auto default_row { make_row(u8"settings-signing-gpg-default-row") };
            default_row->add_flexible(code_line(u8"settings-signing-gpg-default-path",
                view.default_gpg.empty() ? tr(u8"settings.signing.default_gpg_missing") : tr(u8"settings.signing.found_gpg", { { u8"path", view.default_gpg } })));
            default_row->add(button_of(u8"settings-signing-gpg-default", default_label, message(use_default_gpg_intent {}), view.default_gpg.empty() == false && view.writing == false),
                { .length = text_button_width(default_label) + 8.0f, .cross_length = button_height });
            page.add(std::move(default_row), input_height);
            page.add(help_line(u8"settings-signing-gpg-status", view.gpg_status, tone_color(view.gpg_ok ? tone::good : tone::warning)), line_height);
            page.gap(small_gap);

            add_text_setting(page, u8"settings-signing-ssh", tr(u8"settings.signing.ssh_program_label"), kind_settings_signing_ssh_input, view.ssh_program, signing_key_id::gpg_ssh_program,
                u8"C:/Windows/System32/OpenSSH/ssh-keygen.exe", view.writing);
            if (view.ssh_program.error.empty() == false)
                page.add(help_line(u8"settings-signing-ssh-error", view.ssh_program.error, tone_color(tone::bad)), line_height);
            else
                page.add(help_line(u8"settings-signing-ssh-error", view.ssh_program.warning, tone_color(tone::warning)), line_height);
            page.gap(small_gap);

            add_text_setting(page, u8"settings-signing-allowed", tr(u8"settings.signing.allowed_signers_label"), kind_settings_signing_allowed_input, view.allowed_signers,
                signing_key_id::gpg_ssh_allowed_signers, u8"C:/Users/me/.ssh/allowed_signers", view.writing);
            if (view.allowed_signers.error.empty() == false)
                page.add(help_line(u8"settings-signing-allowed-error", view.allowed_signers.error, tone_color(tone::bad)), line_height);
            else
                page.add(help_line(u8"settings-signing-allowed-error", view.allowed_signers.warning, tone_color(tone::warning)), line_height);
            page.add(help_line(u8"settings-signing-allowed-status", view.allowed_signers_status, tone_color(view.allowed_signers_ok ? tone::good : tone::neutral)), line_height);
        }
    } // namespace

    void build_signing_page(page_column& page, const signing_page_view& view, const float content_width)
    {
        build_scope(page, view, content_width);
        page.gap(large_gap);
        build_table(page, view, content_width);
        if (view.loaded == false)
            return;
        page.gap(large_gap);
        build_toggles(page, view);
        page.gap(large_gap);
        build_keys(page, view, content_width);
        page.gap(large_gap);
        build_programs(page, view);
    }
} // namespace cyella::ui
