#include "presentation/ui/command_elements.h"

#include "presentation/commands/command_intents.h"
#include "presentation/palette/palette_intents.h"
#include "presentation/ui/element_kinds.h"
#include "presentation/ui/ui_helpers.h"
#include "presentation/ui_intents.h"

#include <utility>

namespace cyella::ui {
    shortcut_table_element::shortcut_table_element(std::vector<shortcut_binding> bindings)
        : ui_element { luil::ui_element_id { kind_command_marker, std::u8string { shortcut_table_owner } } }
        , bindings_ { std::move(bindings) }
    {
        set_hit_opaque(false);
    }

    void shortcut_table_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    void shortcut_table_element::draw(luil::draw_context&, const luil::interaction_snapshot&) const
    {}

    palette_enter_element::palette_enter_element(luil::input_action action)
        : ui_element { luil::ui_element_id { kind_command_marker, std::u8string { palette_enter_owner } } }
        , action_ { std::move(action) }
    {
        set_hit_opaque(false);
    }

    void palette_enter_element::arrange(const luil::arrange_context& context)
    {
        set_bounds(context.slot);
    }

    void palette_enter_element::draw(luil::draw_context&, const luil::interaction_snapshot&) const
    {}

    luil::input_action action_for_command(const std::string_view id)
    {
        if (id == "repo.open_folder")
            return ui_command(static_cast<std::uint32_t>(ui_command_id::open_folder));
        if (id == "repo.init")
            return ui_command(static_cast<std::uint32_t>(ui_command_id::init_folder));
        return message(run_command_intent { std::string { id } });
    }

    luil::input_action palette_item_action(const palette_item_view& item)
    {
        if (item.ui_thread_command && item.key.starts_with(palette_command_key_prefix))
        {
            const std::u8string id { item.key.substr(palette_command_key_prefix.size()) };
            return action_for_command(std::string { id.begin(), id.end() });
        }
        return message(palette_pick_intent { item.key });
    }
} // namespace cyella::ui
