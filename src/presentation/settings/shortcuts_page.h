#pragma once

#include "domain/app_settings.h"
#include "presentation/commands/command_registry.h"
#include "presentation/settings/settings_state.h"

#include "luil/ui/text_input_state.h"
#include "luil/ui/ui_interaction.h"

#include <optional>
#include <string>
#include <vector>

namespace cyella {
    // 환경 설정 "단축키" 쪽의 snapshot이다. 명령을 묶음(앱·탭·저장소·Git 작업·화면)별로 보이고, 줄마다 지금 단축키와 바꿨는지를 말한다.
    struct shortcut_row_view
    {
        std::string command {};
        std::u8string title {};
        // 지금 단축키다 (` / `로 이었다). 없으면 빈 글이다.
        std::u8string keys {};
        // 기본값에서 바꿨다 (되돌리기 단추가 켜진다).
        bool customized { false };
        bool editing { false };
    };

    struct shortcut_group_view
    {
        std::u8string title {};
        std::vector<shortcut_row_view> rows {};
    };

    struct shortcut_edit_view
    {
        std::string command {};
        std::u8string title {};
        luil::text_input_view input {};
        std::u8string default_keys {};
        // 적용을 막는 문제다 (읽을 수 없는 키, 예약된 키, 다른 명령이 쓰는 키). 비어 있으면 적용할 수 있다.
        std::u8string error {};
        bool can_apply { false };
    };

    struct shortcuts_page_view
    {
        std::vector<shortcut_group_view> groups {};
        std::optional<shortcut_edit_view> edit {};
        bool any_customized { false };
    };

    [[nodiscard]] shortcuts_page_view build_shortcuts_page_view(
        const app_settings& settings, const std::optional<shortcut_edit_state>& edit, const std::optional<luil::text_composition_event>& composition);
} // namespace cyella
