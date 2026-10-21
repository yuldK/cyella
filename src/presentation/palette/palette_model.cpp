#include "presentation/palette/palette_model.h"

#include "domain/hangul_search.h"
#include "domain/text_catalog.h"

#include <algorithm>
#include <string>
#include <utility>

namespace cyella {
    namespace {
        // 최근에 쓴 명령에 더하는 점수다. 가장 최근이 가장 크고 한 칸 멀어질 때마다 줄어 `recency_steps`번째부터는 없다.
        // 일치 점수의 층(글머리 일치 +300, 낱말 머리 +150)보다 작게 둬서 훨씬 나은 일치를 뒤집지는 않되 비슷한 일치끼리는 최근 것이 앞서게 한다.
        constexpr int recency_bonus { 250 };
        constexpr int recency_step { 20 };

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::u8string number_text(const std::size_t value)
        {
            const std::string text { std::to_string(value) };
            return std::u8string { text.begin(), text.end() };
        }

        bool is_ui_thread_command(const std::string_view id) noexcept
        {
            return id == "repo.open_folder" || id == "repo.init";
        }

        struct candidate
        {
            palette_item item {};
            int recency { 0 };
            std::size_t order { 0 };
        };

        std::vector<candidate> command_candidates(const palette_context& context)
        {
            std::vector<candidate> candidates {};
            const std::vector<std::string>* history { context.settings != nullptr ? &context.settings->command_history : nullptr };
            std::size_t order { 0 };
            for (const command_definition& command : command_definitions())
            {
                if (command.in_palette == false || command_available(command.id, context.availability) == false)
                    continue;
                palette_item item {};
                item.key = std::u8string { palette_command_key_prefix } + to_u8(command.id);
                item.title = command.title;
                item.keywords = command.keywords;
                if (context.settings != nullptr)
                    item.detail = shortcut_text(*context.settings, command.id);
                item.ui_thread_command = is_ui_thread_command(command.id);
                int recency { 0 };
                if (history != nullptr)
                    if (const auto found { std::ranges::find(*history, command.id) }; found != history->end())
                        recency = std::max(1, recency_bonus - static_cast<int>(found - history->begin()) * recency_step);
                candidates.push_back(candidate { std::move(item), recency, order++ });
            }
            // 고른 항목의 메뉴 항목이다 (체크아웃·merge·복사 …). 명령과 같은 자리에서 찾는다.
            for (const menu_entry& entry : context.selection_menu.entries)
            {
                if (entry.enabled == false || entry.label.empty())
                    continue;
                palette_item item {};
                item.key = std::u8string { palette_menu_key_prefix } + entry.key;
                item.title = entry.label;
                if (context.selection_menu.title.empty() == false)
                    item.title += u8" — " + context.selection_menu.title;
                item.detail = tr(u8"palette.selection_item");
                item.keywords = entry.key;
                std::ranges::replace(item.keywords, u8'_', u8' ');
                candidates.push_back(candidate { std::move(item), 0, order++ });
            }
            return candidates;
        }

        std::vector<candidate> branch_candidates(const palette_context& context)
        {
            std::vector<candidate> candidates {};
            std::size_t order { 0 };
            for (const ref& value : context.refs)
            {
                if (value.kind != ref_kind::local && value.kind != ref_kind::remote)
                    continue;
                // `origin/HEAD` 같은 기호 ref는 이동할 브랜치가 아니다.
                if (value.kind == ref_kind::remote && value.name.ends_with("/HEAD"))
                    continue;
                palette_item item {};
                item.title = to_u8(value.name);
                item.keywords = u8"branch checkout";
                if (value.kind == ref_kind::local)
                {
                    item.key = std::u8string { palette_branch_key_prefix } + to_u8(value.name);
                    const bool current { context.detached == false && (value.is_current || value.name == context.current_branch) };
                    if (current)
                    {
                        item.detail = tr(u8"palette.current_branch");
                        item.enabled = false;
                    }
                    else if (value.worktree_path.empty() == false)
                    {
                        item.detail = tr(u8"palette.branch_in_other_worktree");
                        item.enabled = false;
                    }
                    else
                    {
                        std::u8string detail {};
                        if (value.ahead > 0)
                            detail += u8"↑" + number_text(value.ahead);
                        if (value.behind > 0)
                            detail += (detail.empty() ? u8"" : u8" ") + std::u8string { u8"↓" } + number_text(value.behind);
                        item.detail = std::move(detail);
                    }
                }
                else
                {
                    item.key = std::u8string { palette_remote_branch_key_prefix } + to_u8(value.name);
                    item.detail = tr(u8"palette.remote_branch");
                }
                candidates.push_back(candidate { std::move(item), 0, order++ });
            }
            return candidates;
        }

        std::vector<candidate> repository_candidates(const palette_context& context)
        {
            std::vector<candidate> candidates {};
            std::size_t order { 0 };
            for (const repository_choice& choice : context.repositories)
            {
                palette_item item {};
                item.title = choice.name;
                item.keywords = choice.path;
                if (choice.tab != 0)
                {
                    item.key = std::u8string { palette_tab_key_prefix } + number_text(choice.tab);
                    item.detail = choice.current ? tr(u8"palette.current_tab") : tr(u8"palette.open_tab");
                    item.enabled = choice.current == false;
                }
                else
                {
                    item.key = std::u8string { palette_recent_key_prefix } + choice.path;
                    item.detail = choice.available ? choice.path : tr(u8"palette.repository_unavailable", { { u8"path", choice.path } });
                    item.enabled = choice.available;
                }
                candidates.push_back(candidate { std::move(item), 0, order++ });
            }
            palette_item folder {};
            folder.key = std::u8string { palette_command_key_prefix } + u8"repo.open_folder";
            folder.title = tr(u8"command.repo.open_folder");
            folder.keywords = u8"open folder browse directory";
            folder.detail = tr(u8"palette.folder_dialog");
            folder.ui_thread_command = true;
            candidates.push_back(candidate { std::move(folder), 0, order++ });
            return candidates;
        }

        std::vector<candidate> profile_candidates(const palette_context& context)
        {
            std::vector<candidate> candidates {};
            std::size_t order { 0 };
            if (context.settings != nullptr)
                for (const user_profile& profile : context.settings->profiles)
                {
                    palette_item item {};
                    item.key = std::u8string { palette_profile_key_prefix } + profile.name;
                    item.title = profile.name;
                    item.detail = profile.user_name + u8" <" + profile.user_email + u8">";
                    if (profile.name == context.current_profile)
                        item.detail += tr(u8"palette.profile_applied_suffix");
                    else if (profile.name == context.suggested_profile)
                        item.detail += tr(u8"palette.profile_suggested_suffix");
                    item.keywords = profile.user_name + u8" " + profile.user_email + u8" profile identity";
                    candidates.push_back(candidate { std::move(item), 0, order++ });
                }
            palette_item manage {};
            manage.key = std::u8string { palette_manage_profiles_key };
            manage.title = tr(u8"palette.manage_profiles");
            manage.keywords = u8"manage settings profiles add edit";
            manage.detail = tr(u8"palette.settings");
            candidates.push_back(candidate { std::move(manage), 0, order++ });
            return candidates;
        }
    } // namespace

    std::u8string palette_mode_title(const palette_mode mode)
    {
        switch (mode)
        {
        case palette_mode::commands:
            return tr(u8"palette.mode.commands");
        case palette_mode::branches:
            return tr(u8"palette.mode.branches");
        case palette_mode::repositories:
            return tr(u8"palette.mode.repositories");
        case palette_mode::profiles:
            return tr(u8"palette.mode.profiles");
        }
        return {};
    }

    std::u8string palette_mode_placeholder(const palette_mode mode)
    {
        switch (mode)
        {
        case palette_mode::commands:
            return tr(u8"palette.placeholder.commands");
        case palette_mode::branches:
            return tr(u8"palette.placeholder.branches");
        case palette_mode::repositories:
            return tr(u8"palette.placeholder.repositories");
        case palette_mode::profiles:
            return tr(u8"palette.placeholder.profiles");
        }
        return {};
    }

    std::vector<palette_item> rank_palette_items(const palette_context& context, const std::u8string_view query)
    {
        std::vector<candidate> candidates {};
        switch (context.mode)
        {
        case palette_mode::commands:
            candidates = command_candidates(context);
            break;
        case palette_mode::branches:
            candidates = branch_candidates(context);
            break;
        case palette_mode::repositories:
            candidates = repository_candidates(context);
            break;
        case palette_mode::profiles:
            candidates = profile_candidates(context);
            break;
        }
        struct scored
        {
            candidate entry {};
            int score { 0 };
        };
        std::vector<scored> matched {};
        for (candidate& entry : candidates)
        {
            const std::optional<int> score { fuzzy_match_score(query, entry.item.title, entry.item.keywords) };
            if (score.has_value() == false)
                continue;
            // 비활성 항목(현재 브랜치 등)은 맞더라도 맨 뒤로 보낸다. 활성 항목이 위에 서야 Enter가 쓸모 있다.
            matched.push_back(scored { std::move(entry), *score });
        }
        std::ranges::stable_sort(matched, [](const scored& left, const scored& right) {
            if (left.entry.item.enabled != right.entry.item.enabled)
                return left.entry.item.enabled;
            const int left_total { left.score + left.entry.recency };
            const int right_total { right.score + right.entry.recency };
            if (left_total != right_total)
                return left_total > right_total;
            return left.entry.order < right.entry.order;
        });
        std::vector<palette_item> items {};
        items.reserve(matched.size());
        for (scored& value : matched)
            items.push_back(std::move(value.entry.item));
        return items;
    }

    palette_list build_palette_list(const palette_context& context, const std::u8string_view query)
    {
        palette_list list {};
        list.items = rank_palette_items(context, query);
        list.matched = list.items.size();
        if (list.items.size() > palette_visible_limit)
        {
            list.hidden = list.items.size() - palette_visible_limit;
            list.items.resize(palette_visible_limit);
        }
        if (list.hidden > 0)
            list.items.push_back(palette_item { .key = std::u8string { palette_more_key }, .title = tr(u8"palette.more_items", { { u8"count", list.hidden } }), .enabled = false });
        if (list.matched == 0)
            list.items.push_back(palette_item { .key = std::u8string { palette_empty_key }, .title = tr(u8"palette.no_match"), .enabled = false });
        return list;
    }
} // namespace cyella
