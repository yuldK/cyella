#pragma once

#include "presentation/settings/settings_view.h"
#include "presentation/ui/build_ui_tree.h"

namespace cyella::ui {
    inline constexpr float theme_picker_height { 128.0f };
    [[nodiscard]] std::unique_ptr<luil::ui_element> make_theme_picker(theme_preference selected);
    [[nodiscard]] std::unique_ptr<luil::ui_element> make_font_combo(const settings_view& view, bool code);
    [[nodiscard]] built_popup build_font_popup(const settings_view& view, float scale);
} // namespace cyella::ui
