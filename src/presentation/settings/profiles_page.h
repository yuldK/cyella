#pragma once

#include "domain/app_settings.h"
#include "presentation/settings/settings_state.h"

#include "luil/ui/text_input_state.h"
#include "luil/ui/ui_interaction.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace cyella {
    // 환경 설정 "사용자 프로필" 쪽의 snapshot이다. 프로필 표, 고치는 줄, 프로필 제안 규칙, 선택된 저장소의 지금 신원을 보인다.
    struct profile_row_view
    {
        std::size_t index { 0 };
        std::u8string name {};
        // `user.name <user.email>`이다.
        std::u8string identity {};
        std::u8string signing_key {};
        // 선택된 저장소에 적용된(적용 기록이 이 프로필인) 프로필이다.
        bool applied { false };
        bool editing { false };
    };

    struct profile_edit_view
    {
        bool is_new { true };
        luil::text_input_view name {};
        luil::text_input_view user_name {};
        luil::text_input_view user_email {};
        luil::text_input_view signing_key {};
        // 저장을 막는 문제다. 비어 있으면 저장할 수 있다.
        std::u8string error {};
        bool can_save { false };
    };

    struct rule_row_view
    {
        std::size_t index { 0 };
        std::u8string prefix {};
        std::u8string profile {};
    };

    struct profiles_page_view
    {
        std::vector<profile_row_view> profiles {};
        std::optional<profile_edit_view> edit {};
        std::vector<rule_row_view> rules {};
        // 규칙을 더하는 입력이다.
        luil::text_input_view rule_prefix {};
        std::u8string rule_profile {};
        std::u8string rule_error {};
        bool can_add_rule { false };
        // 선택된 저장소다. 없으면 프로필 적용 단추가 꺼진다.
        bool repository_open { false };
        std::u8string repository_name {};
        // 선택된 저장소의 지금 신원과 출처다 ("홍길동 <a@b> · 로컬"). 읽는 중이면 그렇게 말한다.
        std::u8string identity_text {};
    };

    // 선택된 저장소에서 읽은 값이다 (controller가 채운다).
    struct profiles_repository_inputs
    {
        bool open { false };
        std::u8string name {};
        std::u8string identity_text {};
        std::u8string applied_profile {};
    };

    [[nodiscard]] profiles_page_view build_profiles_page_view(const app_settings& settings, const std::optional<profile_edit_state>& edit, const profile_rule_draft& rule,
        const std::optional<luil::text_composition_event>& composition, const profiles_repository_inputs& repository);

    // 편집 입력으로 만든 프로필이다.
    [[nodiscard]] user_profile make_profile_from_edit(const profile_edit_state& edit);
} // namespace cyella
