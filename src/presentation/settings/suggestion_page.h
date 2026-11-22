#pragma once

#include "application/file_probe.h"
#include "domain/commit_suggestion.h"
#include "presentation/settings/settings_state.h"

#include "luil/ui/text_input_state.h"
#include "luil/ui/ui_interaction.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 환경 설정 "AI" 쪽의 snapshot이다 (ADR-009). 켬 여부는 저장된 값이고, 명령 칸 셋은 [저장]을 눌러야 저장되는 초안이다.
    struct suggestion_preset_view
    {
        std::u8string id {};
        std::u8string name {};
    };

    struct suggestion_page_view
    {
        bool enabled { false };
        std::vector<suggestion_preset_view> presets {};
        luil::text_input_view executable {};
        luil::text_input_view arguments {};
        luil::text_input_view instruction {};
        // 저장을 막는 문제다. 비어 있으면 저장할 수 있다.
        std::u8string error {};
        // 저장은 되지만 알려 둘 것이다 (그 경로에 파일이 없다).
        std::u8string warning {};
        // 초안이 저장된 값과 다르다.
        bool dirty { false };
        bool can_save { false };
    };

    struct suggestion_draft_check
    {
        std::u8string error {};
        std::u8string warning {};
    };

    // 명령 초안을 검사한다. 닫히지 않은 따옴표, `.cmd`·`.bat`, 절대 경로가 아닌 경로는 저장을 막는다. 이름만 적은 실행 파일은 실행할 때
    // PATH에서 찾으므로 여기서 보지 않는다. `probe`가 있으면 절대 경로에 파일이 있는지 보고, 없으면 주의만 준다.
    [[nodiscard]] suggestion_draft_check check_suggestion_draft(std::u8string_view executable, std::u8string_view arguments, const file_probe* probe);

    [[nodiscard]] suggestion_page_view build_suggestion_page_view(
        const commit_suggestion_settings& settings, const settings_state& state, const std::optional<luil::text_composition_event>& composition, const file_probe* probe);
} // namespace cyella
