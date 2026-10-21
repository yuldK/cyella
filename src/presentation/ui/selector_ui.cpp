#include "presentation/diff_view_intents.h"
#include "presentation/selector_intents.h"
#include "presentation/settings/settings_intents.h"
#include "presentation/ui/build_ui_tree.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ref_chip_element.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui/ui_parts.h"
#include "presentation/ui_intents.h"

#include "luil/ui/dialog_elements.h"
#include "luil/ui/menu_element.h"
#include "luil/ui/panel_element.h"
#include "luil/ui/stack_element.h"
#include "luil/ui/ui_tree.h"

#include <algorithm>
#include <charconv>

namespace cyella::ui {
    namespace {
        // 목록 폭의 하한·상한이다. 상자보다 좁아지지 않고, 긴 경로가 화면을 넘지 않는다 (논리 픽셀).
        constexpr float selector_minimum_width { 280.0f };
        constexpr float selector_maximum_width { 560.0f };
        constexpr float selector_chrome_width { 60.0f };
        constexpr float selector_padding { 4.0f };
        constexpr float selector_field_height { 26.0f };
    } // namespace

    built_popup build_selector_popup(const selector_view& selector, const float scale)
    {
        luil::menu_config menu {};
        switch (selector.kind)
        {
        case selector_kind::repository:
            menu.owner = u8"repository-selector-list";
            break;
        case selector_kind::branch:
            menu.owner = u8"branch-selector-list";
            break;
        case selector_kind::file_app:
            menu.owner = u8"file-app-list";
            break;
        case selector_kind::repository_app:
            menu.owner = u8"repository-app-list";
            break;
        }
        float widest { 0.0f };
        for (const menu_entry& entry : selector.entries)
        {
            menu.items.push_back({ .key = entry.key, .label = entry.label, .icon = entry.icon, .enabled = entry.enabled, .separator_above = entry.separator_above });
            widest = std::max(widest, estimate_text_width(entry.label, 12.0f));
        }
        // 폴더 대화 상자를 여는 항목은 UI thread 명령이고, 나머지는 controller가 처리하는 메시지다.
        menu.select = [](const std::u8string& key) {
            if (key == u8"file-app-settings")
                return message(open_settings_intent { settings_page::diff_tools });
            if (key.starts_with(u8"file-app:"))
            {
                std::size_t index { 0 };
                const std::string digits { to_text(key.substr(9)) };
                if (std::from_chars(digits.data(), digits.data() + digits.size(), index).ec == std::errc {})
                    return message(open_diff_file_app_intent { index });
                return message(close_selector_intent {});
            }
            if (key == selector_open_folder_key)
                return ui_command(static_cast<std::uint32_t>(ui_command_id::open_folder));
            return message(selector_select_intent { key });
        };

        built_popup popup {};
        popup.id = u8"selector";
        popup.selector = true;
        popup.x = selector.x;
        popup.y = selector.y + 2.0f;
        popup.width = std::clamp(std::max(selector.width, widest + selector_chrome_width), selector_minimum_width, selector_maximum_width);
        // 툴바 오른쪽 끝의 "열기" 단추는 단추보다 넓은 목록을 단추의 오른쪽 끝에 맞춰 왼쪽으로 펼친다. 창 밖으로 밀려나지 않게 하려는 것이다.
        if (selector.kind == selector_kind::repository_app)
            popup.x = std::max(selector.x + selector.width - popup.width, 0.0f);
        const float menu_height { luil::menu_element::height_for(menu) };
        // 둘레 여백 두 번 + 거르는 칸 + 칸과 목록 사이 한 번 + 목록.
        popup.height = 3.0f * selector_padding + selector_field_height + menu_height;

        luil::stack_config config {};
        config.padding = luil::edge_insets::all(selector_padding);
        config.spacing = selector_padding;
        auto column { std::make_unique<luil::stack_element>(luil::ui_element_id { kind_layout, u8"selector-column" }, config) };
        luil::text_input_config filter_config {};
        filter_config.placeholder = selector.placeholder;
        filter_config.leading_glyph = codicons::icon_search;
        auto filter { std::make_unique<luil::text_input_element>(luil::ui_element_id { kind_selector_filter_input, u8"selector-filter" }, selector.filter, std::move(filter_config)) };
        filter->set_access_name(selector.placeholder);
        column->add(std::move(filter), selector_field_height);
        auto list { std::make_unique<luil::menu_element>(std::move(menu)) };
        column->add(std::move(list), menu_height);

        // popup 창 전체를 칠한다. 목록은 자기 배경과 테두리를 그 위에 그린다.
        luil::panel_config background {};
        auto panel { std::make_unique<luil::panel_element>(luil::ui_element_id { kind_panel, u8"selector-panel" }, std::move(background)) };
        // 여백을 눌러도 아래 element로 새지 않는다.
        panel->set_hit_opaque(true);
        panel->set_content(std::move(column));
        popup.tree = std::make_shared<const luil::ui_tree>(luil::make_arranged_tree(std::move(panel), { 0.0f, 0.0f, popup.width * scale, popup.height * scale }, scale));
        return popup;
    }
} // namespace cyella::ui
