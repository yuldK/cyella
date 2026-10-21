#include "presentation/menu_extension.h"

#include "domain/text_catalog.h"

#include "luil/generated/codicons.h"

#include <algorithm>
#include <utility>

namespace cyella {
    namespace {
        namespace icons = luil::codicons;

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        menu_entry make_entry(std::u8string key, std::u8string label, const char32_t icon, const bool enabled = true, const bool danger = false)
        {
            return menu_entry { .key = std::move(key), .label = std::move(label), .icon = icon, .enabled = enabled, .danger = danger, .separator_above = false };
        }

        void add_group(std::vector<menu_entry>& entries, std::vector<menu_entry> group)
        {
            if (group.empty())
                return;
            if (entries.empty() == false)
                group.front().separator_above = true;
            for (menu_entry& entry : group)
                entries.push_back(std::move(entry));
        }

        const submodule_info* find_submodule(const menu_context& context, const std::string& path)
        {
            const auto found { std::ranges::find_if(context.submodules, [&path](const submodule_info& info) { return info.path == path; }) };
            return found == context.submodules.end() ? nullptr : &*found;
        }

        const worktree_info* find_worktree(const menu_context& context, const std::string& path)
        {
            const auto found { std::ranges::find_if(context.worktrees, [&path](const worktree_info& info) { return info.path == path; }) };
            return found == context.worktrees.end() ? nullptr : &*found;
        }

        menu_model submodule_menu(const menu_target& target, const menu_context& context)
        {
            menu_model model {};
            model.title = to_u8(target.name);
            const submodule_info* const info { find_submodule(context, target.name) };
            const bool initialized { info != nullptr && info->state != submodule_state::uninitialized };
            add_group(model.entries,
                {
                    make_entry(u8"open", initialized ? tr(u8"menu.ext.open_in_new_tab") : tr(u8"menu.ext.open_in_new_tab_uninitialized"), icons::icon_repo, initialized),
                    make_entry(u8"update", info != nullptr && info->state == submodule_state::uninitialized ? tr(u8"menu.ext.submodule_init_update") : tr(u8"menu.ext.submodule_update"),
                        icons::icon_sync, info != nullptr),
                });
            add_group(model.entries, { make_entry(u8"copy_path", tr(u8"menu.copy_path"), icons::icon_copy) });
            return model;
        }

        menu_model submodules_section_menu(const menu_context& context)
        {
            menu_model model {};
            model.title = u8"SUBMODULES";
            const bool any { context.submodules.empty() == false };
            add_group(model.entries,
                {
                    make_entry(u8"update_all", tr(u8"menu.ext.submodules_update_all"), icons::icon_sync, any),
                    make_entry(u8"sync_all", tr(u8"menu.ext.submodules_sync_urls"), icons::icon_refresh, any),
                });
            return model;
        }

        menu_model worktree_menu(const menu_target& target, const menu_context& context)
        {
            menu_model model {};
            model.title = to_u8(target.name);
            const worktree_info* const info { find_worktree(context, target.name) };
            const bool is_current { info != nullptr && info->is_current };
            const bool is_main { info != nullptr && info->is_main };
            const bool missing { info != nullptr && info->prunable };
            std::u8string open_label { tr(u8"menu.ext.open_in_new_tab") };
            if (is_current)
                open_label = tr(u8"menu.ext.open_in_new_tab_current_worktree");
            else if (missing)
                open_label = tr(u8"menu.ext.open_in_new_tab_missing_folder");
            std::u8string remove_label { tr(u8"menu.ext.worktree_remove") };
            if (is_main)
                remove_label = tr(u8"menu.ext.worktree_remove_main");
            else if (is_current)
                remove_label = tr(u8"menu.ext.worktree_remove_current");
            add_group(model.entries, { make_entry(u8"open", std::move(open_label), icons::icon_worktree, info != nullptr && is_current == false && missing == false) });
            add_group(model.entries, { make_entry(u8"remove", std::move(remove_label), icons::icon_trash, info != nullptr && is_main == false && is_current == false, true) });
            add_group(model.entries, { make_entry(u8"copy_path", tr(u8"menu.copy_path"), icons::icon_copy) });
            return model;
        }

        menu_model worktrees_section_menu(const menu_context& context)
        {
            menu_model model {};
            model.title = u8"WORKTREES";
            const bool prunable { std::ranges::any_of(context.worktrees, [](const worktree_info& info) { return info.prunable; }) };
            add_group(model.entries,
                {
                    make_entry(u8"add", tr(u8"menu.ext.worktree_add"), icons::icon_add),
                    make_entry(u8"prune", prunable ? tr(u8"menu.ext.worktree_prune") : tr(u8"menu.ext.worktree_prune_nothing"), icons::icon_trash, prunable),
                });
            return model;
        }

        menu_model lfs_section_menu(const menu_context& context)
        {
            menu_model model {};
            model.title = u8"GIT LFS";
            const bool usable { context.lfs.available };
            const std::u8string note { usable ? std::u8string {} : tr(u8"menu.ext.lfs_unavailable_suffix") };
            add_group(model.entries,
                {
                    make_entry(u8"pull", tr(u8"menu.ext.lfs_pull_all") + note, icons::icon_cloud_download, usable),
                    make_entry(u8"fetch", tr(u8"menu.ext.lfs_fetch") + note, icons::icon_git_fetch, usable),
                    make_entry(u8"fetch_all", tr(u8"menu.ext.lfs_fetch_all") + note, icons::icon_git_fetch, usable),
                });
            add_group(model.entries, { make_entry(u8"track", tr(u8"menu.ext.lfs_add_track_pattern") + note, icons::icon_package, usable) });
            return model;
        }

        menu_model lfs_file_menu(const menu_target& target, const menu_context& context)
        {
            menu_model model {};
            model.title = to_u8(target.name);
            add_group(model.entries, { make_entry(u8"pull", tr(u8"menu.lfs_pull"), icons::icon_cloud_download, context.lfs.available) });
            add_group(model.entries, { make_entry(u8"copy_path", tr(u8"menu.copy_path"), icons::icon_copy) });
            return model;
        }
    } // namespace

    menu_model build_extension_menu(const menu_target& target, const menu_context& context)
    {
        switch (target.kind)
        {
        case menu_target_kind::submodule:
            return submodule_menu(target, context);
        case menu_target_kind::submodules_section:
            return submodules_section_menu(context);
        case menu_target_kind::worktree:
            return worktree_menu(target, context);
        case menu_target_kind::worktrees_section:
            return worktrees_section_menu(context);
        case menu_target_kind::lfs_section:
            return lfs_section_menu(context);
        case menu_target_kind::lfs_file:
            return lfs_file_menu(target, context);
        case menu_target_kind::commit:
        case menu_target_kind::local_branch:
        case menu_target_kind::remote_branch:
        case menu_target_kind::tag:
        case menu_target_kind::stash:
        case menu_target_kind::file:
        case menu_target_kind::tags_section:
        case menu_target_kind::drop:
        case menu_target_kind::commit_file:
            break;
        }
        return {};
    }

    std::string lfs_pattern_for_path(const std::string& path)
    {
        const std::size_t slash { path.find_last_of("/\\") };
        const std::string name { slash == std::string::npos ? path : path.substr(slash + 1) };
        const std::size_t dot { name.find_last_of('.') };
        // 점으로 시작하는 파일(`.gitignore`)이나 점으로 끝나는 이름에는 확장자가 없다.
        if (dot == std::string::npos || dot == 0 || dot + 1 >= name.size())
            return {};
        return "*" + name.substr(dot);
    }
} // namespace cyella
