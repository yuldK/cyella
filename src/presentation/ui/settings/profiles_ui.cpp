#include "presentation/settings/profiles_intents.h"
#include "presentation/ui/segmented_control.h"
#include "presentation/ui/settings/settings_page_parts.h"
#include "presentation/ui/settings/settings_pages_ui.h"
#include "presentation/ui_intents.h"

#include "luil/ui/choice_group_element.h"

#include <algorithm>
#include <utility>

namespace cyella::ui {
    namespace {
        using namespace pages;

        constexpr float name_width { 120.0f };
        constexpr float apply_width { 128.0f };
        constexpr float small_button_width { 52.0f };

        std::u8string index_key(const std::size_t index)
        {
            return number_text(index);
        }

        std::unique_ptr<luil::ui_element> build_profile_row(const profile_row_view& profile, const bool can_apply, const bool editing, const bool stacked)
        {
            const std::u8string key { index_key(profile.index) };
            auto row { make_row(u8"profile-row-" + key, 8.0f) };
            auto name { make_text(u8"profile-name-" + key, profile.name, text_style { .size = 12.0f, .bold = true }) };
            std::u8string identity { profile.identity };
            if (profile.signing_key.empty() == false)
                identity += u8"  · 서명 키 " + profile.signing_key;
            auto identity_text { make_text(u8"profile-identity-" + key, identity, text_style { .size = 11.0f, .color = dim, .code = true }) };
            if (stacked == false)
            {
                row->add(std::move(name), name_width);
                row->add_flexible(std::move(identity_text));
            }
            // 선택된 저장소에 이미 적용된 프로필은 "적용됨"으로 말한다.
            row->add(button_of(u8"profile-apply-" + key, profile.applied ? u8"적용됨" : u8"현재 저장소에 적용", message(apply_profile_intent { profile.name }), can_apply && profile.applied == false,
                         luil::text_button_visual::accent),
                { .length = apply_width, .cross_length = button_height });
            row->add(button_of(u8"profile-edit-" + key, u8"편집", message(edit_profile_intent { profile.index }), editing == false), { .length = small_button_width, .cross_length = button_height });
            row->add(button_of(u8"profile-remove-" + key, u8"삭제", message(remove_profile_intent { profile.index }), editing == false, luil::text_button_visual::danger),
                { .length = small_button_width, .cross_length = button_height });
            if (stacked == false)
                return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"profile-bar-" + key }, bar_style { .divider_bottom = true }, std::move(row));
            auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"profile-stack-" + key }, luil::stack_config {}) };
            column->add(std::move(name), 24.0f);
            column->add(std::move(identity_text), line_height);
            column->add(std::move(row), row_height);
            column->add_gap(small_gap);
            return std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"profile-bar-" + key }, bar_style { .divider_bottom = true }, std::move(column));
        }

        void add_edit_form(page_column& page, const profile_edit_view& edit)
        {
            page_column form { u8"settings-profile-form-column" };
            form.add(section_title(u8"settings-profile-form-title", edit.is_new ? u8"프로필 추가" : u8"프로필 편집"), section_height);
            form.add(field_label(u8"settings-profile-name-label", u8"프로필 이름"), label_height);
            form.add(input_of(kind_profile_name_input, u8"settings-profile-name-input", edit.name, u8"회사", u8"프로필 이름"), input_height);
            form.gap(small_gap);
            form.add(field_label(u8"settings-profile-user-name-label", u8"user.name (커밋 작성자 이름)"), label_height);
            form.add(input_of(kind_profile_user_name_input, u8"settings-profile-user-name-input", edit.user_name, u8"홍길동", u8"user.name"), input_height);
            form.gap(small_gap);
            form.add(field_label(u8"settings-profile-email-label", u8"user.email"), label_height);
            form.add(input_of(kind_profile_email_input, u8"settings-profile-email-input", edit.user_email, u8"hong@example.com", u8"user.email"), input_height);
            form.gap(small_gap);
            form.add(field_label(u8"settings-profile-signing-label", u8"서명 키 (선택, 커밋 서명 설정이 사용합니다)"), label_height);
            form.add(input_of(kind_profile_signing_key_input, u8"settings-profile-signing-input", edit.signing_key, u8"예: 0A46826A… 또는 ~/.ssh/id_ed25519.pub", u8"서명 키"), input_height);
            form.add(help_line(u8"settings-profile-error", edit.error, error_color()), line_height);
            auto buttons { make_row(u8"settings-profile-buttons") };
            buttons->add_flexible_gap();
            buttons->add(button_of(u8"settings-profile-cancel", u8"취소", message(cancel_profile_edit_intent {})), { .length = 64.0f, .cross_length = button_height });
            buttons->add(
                button_of(u8"settings-profile-save", u8"저장", message(save_profile_intent {}), edit.can_save, luil::text_button_visual::accent), { .length = 64.0f, .cross_length = button_height });
            form.add(std::move(buttons), input_height);

            const float height { form.height() + 24.0f };
            const luil::stack_config padding_config { .padding = luil::edge_insets::all(12.0f) };
            auto padded { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"settings-profile-form-padding" }, padding_config) };
            padded->add_flexible(form.take());
            page.add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"settings-profile-form" },
                         bar_style { .background = [](const luil::ui_color_palette& palette) { return palette.window_background; } }, std::move(padded)),
                height);
        }

        void add_rules(page_column& page, const profiles_page_view& view, const float content_width)
        {
            page.add(section_title(u8"settings-section-rules", u8"자동 제안 규칙"), section_height);
            add_help(page, u8"settings-rules-help-1", u8"지정한 폴더의 저장소를 열면 프로필을 제안합니다. [적용]을 눌러야 저장소 설정에 저장됩니다.", content_width);
            add_help(page, u8"settings-rules-help-2", u8"경로가 가장 구체적인 규칙을 사용합니다. 커밋 작성자가 제안된 프로필과 다르면 커밋 전에 알립니다.", content_width);
            page.gap(small_gap);
            if (view.rules.empty())
                page.add(help_line(u8"settings-rules-empty", u8"등록한 규칙이 없습니다. 아래에서 폴더와 프로필을 정해 더하세요."), row_height);
            for (const rule_row_view& rule : view.rules)
            {
                const std::u8string key { index_key(rule.index) };
                auto row { make_row(u8"rule-row-" + key, 8.0f) };
                row->add_flexible(make_text(u8"rule-prefix-" + key, rule.prefix, text_style { .size = 12.0f, .code = true }));
                row->add(make_text(u8"rule-profile-" + key, u8"→ " + rule.profile, text_style { .size = 12.0f, .bold = true }), name_width + 40.0f);
                row->add(button_of(u8"rule-remove-" + key, u8"삭제", message(remove_profile_rule_intent { rule.index }), true, luil::text_button_visual::danger),
                    { .length = small_button_width, .cross_length = button_height });
                page.add(std::make_unique<bar_element>(luil::ui_element_id { kind_panel, u8"rule-bar-" + key }, bar_style { .divider_bottom = true }, std::move(row)), row_height);
            }
            page.gap(small_gap);
            page.add(field_label(u8"settings-rule-prefix-label", u8"폴더 (이 폴더 아래의 저장소)"), label_height);
            page.add(input_of(kind_profile_rule_prefix_input, u8"settings-rule-prefix-input", view.rule_prefix, u8"E:\\work", u8"규칙 폴더"), input_height);
            if (view.profiles.empty() == false)
            {
                page.add(field_label(u8"settings-rule-profile-label", u8"제안할 프로필"), label_height);
                luil::choice_group_config choice {};
                choice.owner = u8"settings-rule-profile";
                choice.style = luil::choice_style::toggle;
                choice.name = u8"제안할 프로필";
                for (const profile_row_view& profile : view.profiles)
                    choice.items.push_back({ profile.name, profile.name });
                choice.selected = view.rule_profile;
                choice.select = [](const std::u8string& value) { return message(choose_rule_profile_intent { value }); };
                auto choice_row { make_row(u8"settings-rule-profile-row") };
                choice_row->add_flexible(make_segmented_control(std::move(choice)));
                page.add(std::move(choice_row), input_height);
            }
            page.add(help_line(u8"settings-rule-error", view.rule_error, view.can_add_rule ? dim : warning_color()), line_height);
            auto add_row { make_row(u8"settings-rule-add-row") };
            add_row->add(button_of(u8"settings-rule-add", u8"규칙 추가", message(add_profile_rule_intent {}), view.can_add_rule, luil::text_button_visual::accent),
                { .length = 92.0f, .cross_length = button_height });
            page.add(std::move(add_row), input_height);
        }
    } // namespace

    built_settings_page build_profiles_page(const profiles_page_view& view, const float content_width)
    {
        page_column page { u8"settings-profiles-page" };
        const bool stacked { content_width < 560.0f };
        page.add(section_title(u8"settings-section-profiles", u8"사용자 프로필"), section_height);
        add_help(page, u8"settings-profiles-help-1", u8"작성자 이름·이메일·서명 키를 프로필로 저장하고, 저장소마다 적용할 수 있습니다.", content_width);
        add_help(page, u8"settings-profiles-help-2", u8"적용하면 해당 저장소의 .git/config에 저장합니다. 전역 Git 설정은 읽기만 합니다.", content_width);
        page.gap(small_gap);
        // 선택된 저장소의 지금 신원이다. 어느 설정 파일의 값인지(로컬·전역·includeIf)도 말한다.
        if (view.repository_open)
            page.add(make_text(u8"settings-profiles-repository", u8"현재 저장소 " + view.repository_name + u8" · 작성자 " + view.identity_text, text_style { .size = 12.0f, .bold = true }),
                line_height + 4.0f);
        else
            page.add(help_line(u8"settings-profiles-repository", u8"열린 저장소가 없어 프로필을 적용할 수 없습니다. 저장소 탭에서 환경 설정을 열면 적용할 수 있습니다."), line_height + 4.0f);
        page.gap(small_gap);
        if (view.profiles.empty())
            page.add(help_line(u8"settings-profiles-empty", u8"등록한 프로필이 없습니다. [프로필 추가]로 만드세요."), row_height);
        const bool editing { view.edit.has_value() };
        for (const profile_row_view& profile : view.profiles)
            page.add(build_profile_row(profile, view.repository_open, editing, stacked), stacked ? 24.0f + line_height + row_height + small_gap : row_height);
        page.gap(small_gap);
        auto add_row { make_row(u8"settings-profile-add-row") };
        add_row->add(
            button_of(u8"profile-add", u8"프로필 추가", message(add_profile_intent {}), editing == false, luil::text_button_visual::accent), { .length = 104.0f, .cross_length = button_height });
        page.add(std::move(add_row), input_height);
        if (view.edit.has_value())
        {
            page.gap(small_gap);
            add_edit_form(page, *view.edit);
        }
        page.gap(large_gap);
        add_rules(page, view, content_width);
        const float height { page.height() };
        return built_settings_page { page.take(), height };
    }
} // namespace cyella::ui
