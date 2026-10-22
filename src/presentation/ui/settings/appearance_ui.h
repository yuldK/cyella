#pragma once

#include "presentation/settings/settings_view.h"
#include "presentation/ui/build_ui_tree.h"

#include "luil/theme/ui_theme.h"

#include <string>
#include <string_view>

namespace cyella::ui {
    inline constexpr float theme_picker_height { 128.0f };
    // 저장된 키 컬러 id를 luil에 넘길 id로 바꾼다. 비면 luil의 기본 키 컬러다.
    [[nodiscard]] std::u8string effective_accent_id(std::u8string_view stored);
    // 견본의 툴팁·보조 기술 이름이다.
    [[nodiscard]] std::u8string accent_title(const luil::accent_definition& accent);
    [[nodiscard]] std::unique_ptr<luil::ui_element> make_theme_picker(theme_preference selected, std::u8string_view accent);
    // 키 컬러 견본 격자다. 운영체제 키 컬러를 읽었으면 그것이 맨 앞에 선다. 높이는 폭에서 나온다 (`accent_picker_height`).
    [[nodiscard]] std::unique_ptr<luil::ui_element> make_accent_picker(std::u8string_view accent);
    [[nodiscard]] float accent_picker_height(float width);
    [[nodiscard]] std::unique_ptr<luil::ui_element> make_font_combo(const settings_view& view, bool code);
    [[nodiscard]] built_popup build_font_popup(const settings_view& view, float scale);
} // namespace cyella::ui
