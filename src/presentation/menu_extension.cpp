#include "presentation/menu_extension.h"

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
                    make_entry(u8"open", initialized ? u8"새 탭에서 열기" : u8"새 탭에서 열기 (먼저 초기화하세요)", icons::icon_repo, initialized),
                    make_entry(u8"update", info != nullptr && info->state == submodule_state::uninitialized ? u8"초기화하고 업데이트 (init)" : u8"업데이트 (기록된 커밋으로)", icons::icon_sync,
                        info != nullptr),
                });
            add_group(model.entries, { make_entry(u8"copy_path", u8"경로 복사", icons::icon_copy) });
            return model;
        }

        menu_model submodules_section_menu(const menu_context& context)
        {
            menu_model model {};
            model.title = u8"SUBMODULES";
            const bool any { context.submodules.empty() == false };
            add_group(model.entries,
                {
                    make_entry(u8"update_all", u8"모두 업데이트 (init, recursive)", icons::icon_sync, any),
                    make_entry(u8"sync_all", u8"url 동기화 (.gitmodules → 설정)", icons::icon_refresh, any),
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
            std::u8string open_label { u8"새 탭에서 열기" };
            if (is_current)
                open_label = u8"새 탭에서 열기 (지금 보고 있는 worktree)";
            else if (missing)
                open_label = u8"새 탭에서 열기 (폴더가 사라졌습니다)";
            std::u8string remove_label { u8"제거…" };
            if (is_main)
                remove_label = u8"제거 (주 작업 트리는 지울 수 없습니다)";
            else if (is_current)
                remove_label = u8"제거 (지금 보고 있는 worktree는 이 탭에서 지울 수 없습니다)";
            add_group(model.entries, { make_entry(u8"open", std::move(open_label), icons::icon_worktree, info != nullptr && is_current == false && missing == false) });
            add_group(model.entries, { make_entry(u8"remove", std::move(remove_label), icons::icon_trash, info != nullptr && is_main == false && is_current == false, true) });
            add_group(model.entries, { make_entry(u8"copy_path", u8"경로 복사", icons::icon_copy) });
            return model;
        }

        menu_model worktrees_section_menu(const menu_context& context)
        {
            menu_model model {};
            model.title = u8"WORKTREES";
            const bool prunable { std::ranges::any_of(context.worktrees, [](const worktree_info& info) { return info.prunable; }) };
            add_group(model.entries,
                {
                    make_entry(u8"add", u8"worktree 추가…", icons::icon_add),
                    make_entry(u8"prune", prunable ? u8"사라진 worktree 정리 (prune)" : u8"사라진 worktree 정리 (정리할 것이 없습니다)", icons::icon_trash, prunable),
                });
            return model;
        }

        menu_model lfs_section_menu(const menu_context& context)
        {
            menu_model model {};
            model.title = u8"GIT LFS";
            const bool usable { context.lfs.available };
            const std::u8string note { usable ? std::u8string {} : u8" (git-lfs를 찾을 수 없습니다)" };
            add_group(model.entries,
                {
                    make_entry(u8"pull", u8"LFS pull (받고 작업 트리에 채움)" + note, icons::icon_cloud_download, usable),
                    make_entry(u8"fetch", u8"LFS fetch (객체만 받음)" + note, icons::icon_git_fetch, usable),
                    make_entry(u8"fetch_all", u8"LFS fetch --all (모든 ref의 객체)" + note, icons::icon_git_fetch, usable),
                });
            add_group(model.entries, { make_entry(u8"track", u8"추적 패턴 추가…" + note, icons::icon_package, usable) });
            return model;
        }

        menu_model lfs_file_menu(const menu_target& target, const menu_context& context)
        {
            menu_model model {};
            model.title = to_u8(target.name);
            add_group(model.entries, { make_entry(u8"pull", u8"LFS 객체 가져오기 (pull)", icons::icon_cloud_download, context.lfs.available) });
            add_group(model.entries, { make_entry(u8"copy_path", u8"경로 복사", icons::icon_copy) });
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
