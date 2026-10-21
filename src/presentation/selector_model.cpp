#include "presentation/selector_model.h"

#include "domain/repository_path.h"
#include "domain/text_catalog.h"

#include "luil/generated/codicons.h"

#include <algorithm>
#include <set>
#include <utility>

namespace cyella {
    namespace {
        namespace icons = luil::codicons;

        std::u8string number(const std::size_t value)
        {
            const std::string digits { std::to_string(value) };
            return std::u8string { digits.begin(), digits.end() };
        }

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        menu_entry make_entry(std::u8string key, std::u8string label, const char32_t icon, const bool enabled = true, const bool separator_above = false)
        {
            return menu_entry { .key = std::move(key), .label = std::move(label), .icon = icon, .enabled = enabled, .danger = false, .separator_above = separator_above };
        }

        char8_t folded(const char8_t value) noexcept
        {
            if (value == u8'\\')
                return u8'/';
            return value >= u8'A' && value <= u8'Z' ? static_cast<char8_t>(value - u8'A' + u8'a') : value;
        }

        // 앞뒤 공백을 뗀 글이다.
        std::u8string_view trimmed(std::u8string_view text) noexcept
        {
            const auto space = [](const char8_t value) { return value == u8' ' || value == u8'\t' || value == u8'\r' || value == u8'\n'; };
            while (text.empty() == false && space(text.front()))
                text.remove_prefix(1);
            while (text.empty() == false && space(text.back()))
                text.remove_suffix(1);
            return text;
        }

        // 보일 항목을 상한까지만 남기고 나머지를 "외 N개" 줄로 줄인다.
        void limit_entries(selector_list& list)
        {
            list.matched = list.entries.size();
            if (list.entries.size() <= selector_visible_limit)
                return;
            list.hidden = list.entries.size() - selector_visible_limit;
            list.entries.resize(selector_visible_limit);
        }
    } // namespace

    bool selector_matches(const std::u8string_view text, const std::u8string_view query)
    {
        const std::u8string_view wanted { trimmed(query) };
        if (wanted.empty())
            return true;
        if (wanted.size() > text.size())
            return false;
        for (std::size_t start { 0 }; start + wanted.size() <= text.size(); ++start)
        {
            bool same { true };
            for (std::size_t offset { 0 }; offset < wanted.size(); ++offset)
                if (folded(text[start + offset]) != folded(wanted[offset]))
                {
                    same = false;
                    break;
                }
            if (same)
                return true;
        }
        return false;
    }

    std::vector<repository_choice> make_repository_choices(const repository_tabs& tabs, const app_settings& settings, const repository_exists_function& exists, const repository_tab_id selected_tab)
    {
        std::vector<repository_choice> choices {};
        std::set<std::u8string> open_keys {};
        for (const repository_tab& tab : tabs.tabs())
        {
            if (tab.kind != repository_tab_kind::repository)
                continue;
            open_keys.insert(repository_path_key(tab.path));
            choices.push_back({ .tab = tab.id, .name = tab.display_name, .path = tab.path, .current = tab.id == selected_tab, .available = true });
        }
        for (const recent_repository& recent : settings.recent_repositories)
        {
            if (open_keys.insert(repository_path_key(recent.path)).second == false)
                continue;
            const bool available { exists == nullptr || exists(recent.path) };
            choices.push_back({ .tab = 0, .name = recent.display_name, .path = recent.path, .current = false, .available = available });
        }
        return choices;
    }

    selector_list build_repository_selector(const std::vector<repository_choice>& choices, const std::u8string_view query)
    {
        selector_list list {};
        bool first_recent { true };
        for (const repository_choice& choice : choices)
        {
            if (selector_matches(choice.name, query) == false && selector_matches(choice.path, query) == false)
                continue;
            // 메뉴 항목은 글 하나라 이름 뒤에 부모 폴더를 띄워 잇는다.
            const std::u8string folder { repository_parent_folder(choice.path) };
            std::u8string label { choice.name };
            if (folder.empty() == false)
                label += u8"   " + folder;
            if (choice.available == false)
                label += tr(u8"selector.unavailable_suffix");
            if (choice.tab != 0)
            {
                list.entries.push_back(make_entry(std::u8string { selector_tab_key_prefix } + number(choice.tab), std::move(label), choice.current ? icons::icon_check : icons::icon_repo,
                    /*enabled*/ choice.current == false));
                continue;
            }
            // 최근 목록의 첫 항목 앞에 구분선을 둔다. 앞에 열린 탭이 없으면 선을 긋지 않는다.
            const bool separate { first_recent && list.entries.empty() == false };
            first_recent = false;
            list.entries.push_back(make_entry(std::u8string { selector_recent_key_prefix } + choice.path, std::move(label), icons::icon_history, choice.available, separate));
        }
        limit_entries(list);
        if (list.hidden > 0)
            list.entries.push_back(make_entry(std::u8string { selector_more_key }, tr(u8"selector.repository_more", { { u8"count", list.hidden } }), 0, false, true));
        if (list.matched == 0)
            list.entries.push_back(make_entry(std::u8string { selector_empty_key }, tr(u8"selector.repository_none"), 0, false));
        list.entries.push_back(make_entry(std::u8string { selector_open_folder_key }, tr(u8"selector.open_folder"), icons::icon_folder_opened, true, true));
        return list;
    }

    selector_list build_branch_selector(const std::vector<ref>& refs, const std::u8string_view query, const bool detached)
    {
        selector_list list {};
        for (const ref& value : refs)
        {
            if (value.kind != ref_kind::local || selector_matches(to_u8(value.name), query) == false)
                continue;
            const bool current { value.is_current && detached == false };
            std::u8string label { to_u8(value.name) };
            if (current)
                label += tr(u8"selector.current_suffix");
            if (value.ahead > 0)
                label += u8"   ↑" + number(value.ahead);
            if (value.behind > 0)
                label += u8"   ↓" + number(value.behind);
            list.entries.push_back(
                make_entry(std::u8string { selector_branch_key_prefix } + to_u8(value.name), std::move(label), current ? icons::icon_check : icons::icon_git_branch, current == false));
        }
        limit_entries(list);
        if (list.hidden > 0)
            list.entries.push_back(make_entry(std::u8string { selector_more_key }, tr(u8"selector.branch_more", { { u8"count", list.hidden } }), 0, false, true));
        if (list.matched == 0)
            list.entries.push_back(make_entry(std::u8string { selector_empty_key }, tr(u8"selector.branch_none"), 0, false));
        return list;
    }
} // namespace cyella
