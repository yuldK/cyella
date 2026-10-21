#pragma once

#include "luil/ui/image_element.h"

#include <memory>
#include <string>
#include <string_view>

namespace cyella::ui {
    // 앱 심볼 그림이다 (assets/cyella.svg에서 만든 256px PNG). luil의 Skia 빌드에는 SVG 모듈이 없어 미리 그린 PNG를 쓴다.
    // 창을 만들기 전에 platform이 한 번 정하고 그 뒤로는 읽기만 한다. 정하지 않으면(그림 resource를 싣지 않은 test 등) 빈 그림이고,
    // 심볼을 놓는 자리는 그때 캡션 글리프로 물러서거나 비워 둔다.
    void set_app_symbol(luil::ui_image image);
    [[nodiscard]] const luil::ui_image& app_symbol() noexcept;

    // 심볼을 장식으로 놓는 element다. 설명을 비워 보조 기술에는 읽히지 않는다 (곁의 앱 이름·제목이 읽힌다).
    [[nodiscard]] std::unique_ptr<luil::ui_element> make_app_symbol(std::u8string owner);

    // 타이틀 캘리그라피 그림이다 (assets/cyella-title*.svg에서 만든 PNG). 글자색이 다른 두 장으로, 밝은 바탕에는 짙은 남색 글자를,
    // 어두운 바탕에는 옅은 바다색 글자를 쓴다. 심볼처럼 platform이 창을 만들기 전에 한 번 정한다.
    void set_app_title(luil::ui_image on_light, luil::ui_image on_dark);
    // 두 장이 모두 있는가. 없으면 타이틀 자리는 글자 제목으로 물러선다.
    [[nodiscard]] bool app_title_available() noexcept;
    // 타이틀 그림의 가로세로 비다 (폭 ÷ 높이). 그림이 없으면 0이다.
    [[nodiscard]] float app_title_aspect() noexcept;
    // 타이틀을 놓는 element다. 테마는 tree를 지을 때가 아니라 그릴 때의 팔레트로 정해지므로(시스템 테마) 그릴 때 바탕의 밝기로
    // 두 장 중 하나를 고른다. 보조 기술에는 `name`이 그림 이름으로 읽힌다.
    [[nodiscard]] std::unique_ptr<luil::ui_element> make_app_title(std::u8string owner, std::u8string name);

    // 앱 판이다 (`0.1.0`). CMake `project(VERSION)`에서 온다.
    [[nodiscard]] std::u8string_view app_version() noexcept;
} // namespace cyella::ui
