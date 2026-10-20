#pragma once

#include "presentation/settings/profiles_page.h"
#include "presentation/settings/shortcuts_page.h"

#include "luil/ui/ui_element.h"

#include <memory>

namespace cyella::ui {
    // 환경 설정의 한 쪽 내용이다. 쪽마다 높이를 담을 때 알아야 흘리는 창이 길이를 정하므로 높이를 함께 돌려준다.
    struct built_settings_page
    {
        std::unique_ptr<luil::ui_element> content {};
        float height { 0.0f };
    };

    // "단축키" 쪽이다. 명령을 묶음별로 보이고 줄마다 [편집]·[기본값]을 둔다. 고치는 줄 바로 아래에 입력 칸이 선다.
    [[nodiscard]] built_settings_page build_shortcuts_page(const shortcuts_page_view& view);
    // "사용자 프로필" 쪽이다. 프로필 표, 고치는 줄, 프로필 제안 규칙이다.
    [[nodiscard]] built_settings_page build_profiles_page(const profiles_page_view& view);
} // namespace cyella::ui
