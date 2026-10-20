#include "presentation/dialog_intents.h"
#include "presentation/extension_intents.h"
#include "presentation/form_extension.h"
#include "presentation/shell_controller.h"

#include <algorithm>
#include <utility>

// submodule·worktree·Git LFS의 controller 몫이다: 사이드바 행 활성화, 메뉴 항목 실행, worktree 추가·LFS 추적 대화 상자.
namespace cyella {
    namespace {
        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::u8string short_sha(const std::string_view sha)
        {
            return to_u8(sha.substr(0, std::min<std::size_t>(sha.size(), 7)));
        }

        // 저장소 폴더와 저장소 기준 상대 경로(`/` 구분)를 잇는다.
        std::u8string join_repository_path(const std::u8string& repository, const std::string& relative)
        {
            std::u8string path { repository };
            if (path.empty() == false && path.back() != u8'/' && path.back() != u8'\\')
                path.push_back(u8'\\');
            for (const char value : relative)
                path.push_back(value == '/' ? u8'\\' : static_cast<char8_t>(value));
            return path;
        }

        const submodule_info* find_submodule(const repository_snapshot& snapshot, const std::string& path)
        {
            const auto found { std::ranges::find_if(snapshot.submodules, [&path](const submodule_info& info) { return info.path == path; }) };
            return found == snapshot.submodules.end() ? nullptr : &*found;
        }

        const worktree_info* find_worktree(const repository_snapshot& snapshot, const std::string& path)
        {
            const auto found { std::ranges::find_if(snapshot.worktrees, [&path](const worktree_info& info) { return info.path == path; }) };
            return found == snapshot.worktrees.end() ? nullptr : &*found;
        }
    } // namespace

    bool shell_controller::handle_extension_message(const luil::app_message& message)
    {
        if (const auto* const intent { message.get<activate_sidebar_item_intent>() }; intent != nullptr)
        {
            activate_extension_row(*intent);
            return true;
        }
        if (const auto* const intent { message.get<open_submodule_intent>() }; intent != nullptr)
        {
            open_submodule(intent->path);
            return true;
        }
        return false;
    }

    void shell_controller::open_submodule(const std::string& path)
    {
        repository_session* const session { selected_session() };
        if (session == nullptr)
            return;
        const submodule_info* const info { find_submodule(session->snapshot(), path) };
        if (info == nullptr)
            return;
        if (info->state == submodule_state::uninitialized)
        {
            show_notice(u8"'" + to_u8(path) + u8"' submodule이 아직 초기화되지 않았습니다. 업데이트(init)한 뒤 여세요.", true);
            return;
        }
        // submodule은 자기 `.git`을 가진 저장소다. 상위 저장소와 별개의 탭이 된다.
        open_repository(join_repository_path(session->path(), path));
    }

    void shell_controller::open_worktree(const std::string& path)
    {
        repository_session* const session { selected_session() };
        if (session == nullptr)
            return;
        const worktree_info* const info { find_worktree(session->snapshot(), path) };
        if (info == nullptr)
            return;
        if (info->is_current)
            return;
        if (info->prunable)
        {
            show_notice(u8"'" + to_u8(path) + u8"' 폴더가 사라졌습니다. 정리(prune)할 수 있습니다.", true);
            return;
        }
        open_repository(to_u8(path));
    }

    void shell_controller::activate_extension_row(const activate_sidebar_item_intent& intent)
    {
        repository_session* const session { selected_session() };
        if (session == nullptr)
            return;
        switch (intent.item)
        {
        case sidebar_item_kind::submodule: {
            const submodule_info* const info { find_submodule(session->snapshot(), intent.path) };
            if (info == nullptr)
                return;
            if (info->state == submodule_state::uninitialized)
                static_cast<void>(submit(submodule_update_request { .init = true, .recursive = true, .paths = { intent.path } }));
            else
                open_submodule(intent.path);
            return;
        }
        case sidebar_item_kind::worktree:
            open_worktree(intent.path);
            return;
        case sidebar_item_kind::ref:
        case sidebar_item_kind::lfs:
            return;
        }
    }

    void shell_controller::open_add_worktree_dialog(const std::string& subject, const std::size_t mode, const std::string& branch)
    {
        repository_session* const session { selected_session() };
        if (session == nullptr)
            return;
        const repository_summary& summary { session->snapshot().summary };
        dialog_state dialog {};
        dialog.kind = dialog_kind::add_worktree;
        dialog.serial = ++ui_.dialog_serial;
        dialog.subject = subject;
        dialog.choice = std::min<std::size_t>(mode, 2);
        if (subject.empty())
            dialog.subject_label = summary.is_detached ? u8"HEAD (detached " + short_sha(summary.head_sha) + u8")" : u8"HEAD (" + to_u8(summary.current_branch) + u8")";
        else
            dialog.subject_label = commit_label(*session, subject);
        clear_form_inputs();
        const std::string label { branch.empty() ? (subject.empty() ? std::string {} : subject.substr(0, 7)) : branch };
        set_form_field(0, default_worktree_folder(session->path(), label));
        if (branch.empty() == false)
            set_form_field(1, to_u8(branch));
        ui_.form.identity = dialog_identity(dialog);
        ui_.dialog = std::move(dialog);
        ui_.menu.reset();
    }

    void shell_controller::open_lfs_track_dialog(const std::string& pattern)
    {
        dialog_state dialog {};
        dialog.kind = dialog_kind::lfs_track;
        dialog.serial = ++ui_.dialog_serial;
        clear_form_inputs();
        if (pattern.empty() == false)
            set_form_field(0, to_u8(pattern));
        ui_.form.identity = dialog_identity(dialog);
        ui_.dialog = std::move(dialog);
        ui_.menu.reset();
    }

    void shell_controller::run_extension_menu_action(const menu_target& target, const std::u8string& key, const menu_context& context)
    {
        repository_session* const session { selected_session() };
        if (session == nullptr)
            return;
        switch (target.kind)
        {
        case menu_target_kind::submodule:
            if (key == u8"open")
                open_submodule(target.name);
            else if (key == u8"update")
                static_cast<void>(submit(submodule_update_request { .init = true, .recursive = true, .paths = { target.name } }));
            else if (key == u8"copy_path")
                copy_text(to_u8(target.name));
            return;
        case menu_target_kind::submodules_section:
            if (key == u8"update_all")
                static_cast<void>(submit(submodule_update_request {}));
            else if (key == u8"sync_all")
                static_cast<void>(submit(submodule_sync_request {}));
            return;
        case menu_target_kind::worktree: {
            if (key == u8"open")
                open_worktree(target.name);
            else if (key == u8"copy_path")
                copy_text(to_u8(target.name));
            else if (key == u8"remove")
            {
                const worktree_info* const info { find_worktree(session->snapshot(), target.name) };
                if (info == nullptr)
                    return;
                dialog_state dialog {};
                dialog.title = u8"worktree를 제거할까요?";
                dialog.lines.push_back(u8"'" + to_u8(target.name) + u8"' 폴더와 그 worktree의 git 기록을 지웁니다. 브랜치와 커밋은 남습니다.");
                dialog.lines.push_back(u8"커밋하지 않은 변경이 있으면 거절됩니다.");
                if (info->locked)
                    dialog.lines.push_back(
                        u8"잠긴 worktree입니다" + (info->lock_reason.empty() ? std::u8string {} : u8" (" + to_u8(info->lock_reason) + u8")") + u8". 강제로 지울지 아래에서 정하세요.");
                dialog.confirm_label = u8"제거";
                dialog.danger = true;
                dialog.check_label = u8"변경이 있거나 잠겨 있어도 강제로 제거";
                dialog.plan.primary.push_back(worktree_remove_request { .path = to_u8(target.name), .force = false });
                dialog.plan.alternate.push_back(worktree_remove_request { .path = to_u8(target.name), .force = true });
                open_confirm(std::move(dialog));
            }
            return;
        }
        case menu_target_kind::worktrees_section:
            if (key == u8"add")
                open_add_worktree_dialog({}, 0, {});
            else if (key == u8"prune")
                static_cast<void>(submit(worktree_prune_request {}));
            return;
        case menu_target_kind::lfs_section:
            if (key == u8"pull")
                static_cast<void>(submit(lfs_pull_request {}));
            else if (key == u8"fetch")
                static_cast<void>(submit(lfs_fetch_request { .all = false }));
            else if (key == u8"fetch_all")
                static_cast<void>(submit(lfs_fetch_request { .all = true }));
            else if (key == u8"track")
                open_lfs_track_dialog({});
            return;
        case menu_target_kind::lfs_file:
            if (key == u8"pull")
                static_cast<void>(submit(lfs_pull_request {}));
            else if (key == u8"copy_path")
                copy_text(to_u8(target.name));
            return;
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
        static_cast<void>(context);
    }
} // namespace cyella
