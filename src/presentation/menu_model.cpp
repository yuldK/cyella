#include "presentation/menu_model.h"

#include "domain/text_catalog.h"
#include "presentation/menu_extension.h"

#include "luil/generated/codicons.h"

#include <algorithm>
#include <charconv>
#include <string>
#include <utility>

namespace cyella {
    namespace {
        namespace icons = luil::codicons;

        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        std::u8string short_sha(const std::string& sha)
        {
            return to_u8(std::string_view { sha }.substr(0, std::min<std::size_t>(sha.size(), 7)));
        }

        menu_entry make_entry(std::u8string key, std::u8string label, const char32_t icon, const bool enabled = true, const bool danger = false)
        {
            return menu_entry { .key = std::move(key), .label = std::move(label), .icon = icon, .enabled = enabled, .danger = danger, .separator_above = false };
        }

        // 앞 항목과 구분선으로 가른다.
        void add_group(std::vector<menu_entry>& entries, std::vector<menu_entry> group)
        {
            if (group.empty())
                return;
            if (entries.empty() == false)
                group.front().separator_above = true;
            for (menu_entry& entry : group)
                entries.push_back(std::move(entry));
        }

        const ref* find_ref(const std::vector<ref>& refs, const ref_kind kind, const std::string& name)
        {
            for (const ref& value : refs)
                if (value.kind == kind && value.name == name)
                    return &value;
            return nullptr;
        }

        std::u8string count_suffix(const std::optional<std::uint32_t>& count)
        {
            if (count.has_value() == false)
                return {};
            if (*count == 1)
                return tr(u8"menu.commit_count_suffix_one", { { u8"count", *count } });
            return tr(u8"menu.commit_count_suffix", { { u8"count", *count } });
        }

        std::vector<menu_entry> reset_group(const std::u8string& what, const std::u8string& to, const bool enabled)
        {
            std::vector<menu_entry> group {};
            group.push_back(make_entry(u8"reset_soft", tr(u8"menu.reset_soft", { { u8"what", what }, { u8"to", to } }), icons::icon_debug_step_back, enabled));
            group.push_back(make_entry(u8"reset_mixed", tr(u8"menu.reset_mixed", { { u8"what", what }, { u8"to", to } }), icons::icon_debug_step_back, enabled));
            group.push_back(make_entry(u8"reset_hard", tr(u8"menu.reset_hard", { { u8"what", what }, { u8"to", to } }), icons::icon_trash, enabled, true));
            return group;
        }

        bool has_current_branch(const menu_context& context)
        {
            return context.detached == false && context.current_branch.empty() == false;
        }

        // ---------------------------------------------------------------- 대상별 항목

        menu_model commit_menu(const menu_target& target, const menu_context& context)
        {
            menu_model model {};
            const bool is_head { target.sha == context.head_sha };
            std::vector<menu_entry> entries {};
            model.title = short_sha(target.sha);
            add_group(entries,
                {
                    make_entry(u8"checkout_detached", tr(u8"menu.checkout_commit_detached"), icons::icon_target, !(is_head && context.detached)),
                    make_entry(u8"create_branch", tr(u8"menu.create_branch_here"), icons::icon_git_branch_create),
                    make_entry(u8"create_tag", tr(u8"menu.create_tag_here"), icons::icon_tag),
                    make_entry(u8"add_worktree", tr(u8"menu.add_worktree_here"), icons::icon_worktree),
                });
            add_group(entries,
                {
                    make_entry(u8"cherry_pick", context.commit_is_merge ? tr(u8"menu.cherry_pick_merge") : std::u8string { u8"Cherry-pick" }, icons::icon_git_pull_request_go_to_changes,
                        context.commit_is_merge == false && is_head == false),
                    make_entry(u8"revert", context.commit_is_merge ? tr(u8"menu.revert_merge") : std::u8string { u8"Revert" }, icons::icon_discard, context.commit_is_merge == false),
                });
            const std::u8string where { has_current_branch(context) ? to_u8(context.current_branch) : std::u8string { u8"HEAD" } };
            add_group(entries, reset_group(where, short_sha(target.sha), is_head == false));
            // 이 커밋 위에 쌓인 커밋들을 편집한다 (`rebase -i <이 커밋>`).
            add_group(entries, { make_entry(u8"interactive_rebase_here", tr(u8"menu.interactive_rebase_here"), icons::icon_list_ordered, is_head == false && context.head_sha.empty() == false) });
            add_group(entries, { make_entry(u8"copy_sha", tr(u8"menu.copy_sha"), icons::icon_copy) });
            model.entries = std::move(entries);
            return model;
        }

        std::vector<menu_entry> merge_rebase_group(const menu_target& target, const menu_context& context)
        {
            const bool current_is_this { target.kind == menu_target_kind::local_branch && target.name == context.current_branch };
            const bool unavailable { has_current_branch(context) == false || current_is_this };
            const std::u8string name { to_u8(target.name) };
            const std::u8string current { has_current_branch(context) ? to_u8(context.current_branch) : tr(u8"menu.current_branch") };
            std::vector<menu_entry> group {};
            group.push_back(make_entry(u8"merge_into_current", tr(u8"menu.merge_into", { { u8"source", name }, { u8"target", current } }), icons::icon_git_merge, unavailable == false));
            group.push_back(make_entry(u8"rebase_current_onto", tr(u8"menu.rebase_onto", { { u8"branch", current }, { u8"onto", name } }), icons::icon_repo_forked, unavailable == false));
            group.push_back(
                make_entry(u8"interactive_rebase_current_onto", tr(u8"menu.interactive_rebase_onto", { { u8"branch", current }, { u8"onto", name } }), icons::icon_list_ordered, unavailable == false));
            return group;
        }

        // 현재 브랜치가 이 브랜치 위에 쌓여 있으면 체크아웃 없이 이 브랜치를 현재 브랜치로 앞당긴다. 작업 브랜치에서
        // main을 따라오게 하는 길이다. fast-forward로 갈 수 없으면 왜 안 되는지를 꼬리에 붙여 끈다.
        menu_entry fast_forward_to_current_entry(const menu_target& target, const menu_context& context, const ref* const branch_ref, const bool elsewhere)
        {
            const drop_facts& facts { context.facts };
            const std::u8string label { tr(u8"menu.fast_forward", { { u8"branch", to_u8(target.name) }, { u8"to", to_u8(context.current_branch) } }) };
            if (elsewhere)
                return make_entry(u8"fast_forward_to_current", label + tr(u8"menu.in_other_worktree"), icons::icon_arrow_up, false);
            if (branch_ref != nullptr && branch_ref->target_sha == context.head_sha)
                return make_entry(u8"fast_forward_to_current", label + tr(u8"menu.already_included"), icons::icon_arrow_up, false);
            if (facts.destination_is_ancestor.has_value() == false)
                return make_entry(u8"fast_forward_to_current", label, icons::icon_arrow_up, false);
            if (*facts.destination_is_ancestor == false)
                return make_entry(u8"fast_forward_to_current", label + tr(u8"menu.rebase_first"), icons::icon_arrow_up, false);
            return make_entry(u8"fast_forward_to_current", label + count_suffix(facts.only_in_source), icons::icon_arrow_up);
        }

        menu_model local_branch_menu(const menu_target& target, const menu_context& context)
        {
            menu_model model {};
            model.title = to_u8(target.name);
            const bool is_current { target.name == context.current_branch && context.detached == false };
            // 다른 worktree가 쓰는 브랜치는 git이 체크아웃을 거절한다. 미리 끄고 그 worktree를 열게 한다.
            const ref* const branch_ref { find_ref(context.refs, ref_kind::local, target.name) };
            const bool has_upstream { branch_ref != nullptr && branch_ref->upstream.empty() == false };
            const bool elsewhere { branch_ref != nullptr && branch_ref->worktree_path.empty() == false };
            if (elsewhere)
                add_group(model.entries,
                    {
                        make_entry(u8"checkout", tr(u8"menu.checkout_in_other_worktree"), icons::icon_check, false),
                        make_entry(u8"open_worktree", tr(u8"menu.open_that_worktree"), icons::icon_worktree),
                    });
            else
                add_group(model.entries,
                    {
                        make_entry(u8"checkout", is_current ? tr(u8"menu.checkout_current") : tr(u8"menu.checkout"), icons::icon_check, is_current == false),
                        make_entry(u8"add_worktree", tr(u8"menu.checkout_in_new_worktree"), icons::icon_worktree, is_current == false),
                    });
            std::vector<menu_entry> integrate {};
            if (has_current_branch(context) && is_current == false)
                integrate.push_back(fast_forward_to_current_entry(target, context, branch_ref, elsewhere));
            for (menu_entry& entry : merge_rebase_group(target, context))
                integrate.push_back(std::move(entry));
            add_group(model.entries, std::move(integrate));
            add_group(model.entries,
                {
                    make_entry(u8"push", u8"Push…", icons::icon_repo_push),
                    make_entry(u8"set_upstream", tr(u8"menu.set_upstream"), icons::icon_cloud),
                    make_entry(u8"unset_upstream", has_upstream ? tr(u8"menu.unset_upstream") : tr(u8"menu.unset_upstream_none"), icons::icon_cloud, has_upstream),
                    make_entry(u8"rename", tr(u8"menu.rename"), icons::icon_edit),
                });
            add_group(model.entries, { make_entry(u8"delete", is_current ? tr(u8"menu.delete_current_branch") : tr(u8"menu.delete"), icons::icon_trash, is_current == false, true) });
            add_group(model.entries, { make_entry(u8"copy_name", tr(u8"menu.copy_name"), icons::icon_copy) });
            return model;
        }

        menu_model remote_branch_menu(const menu_target& target, const menu_context& context)
        {
            menu_model model {};
            model.title = to_u8(target.name);
            add_group(model.entries, { make_entry(u8"checkout", tr(u8"menu.checkout"), icons::icon_check) });
            add_group(model.entries, merge_rebase_group(target, context));
            add_group(model.entries, { make_entry(u8"copy_name", tr(u8"menu.copy_name"), icons::icon_copy) });
            return model;
        }

        menu_model tag_menu(const menu_target& target, const menu_context& context)
        {
            menu_model model {};
            model.title = to_u8(target.name);
            const ref* const tag { find_ref(context.refs, ref_kind::tag, target.name) };
            const auto on_remote = [tag](const std::string& remote) { return tag != nullptr && std::find(tag->remotes.begin(), tag->remotes.end(), remote) != tag->remotes.end(); };
            std::vector<menu_entry> push {};
            for (const std::string& remote : context.remotes)
            {
                const bool exists { on_remote(remote) };
                push.push_back(make_entry(u8"push:" + to_u8(remote),
                    exists ? tr(u8"menu.tag_already_on_remote", { { u8"remote", remote } }) : tr(u8"menu.push_tag_to_remote", { { u8"remote", remote } }), icons::icon_repo_push, exists == false));
            }
            add_group(model.entries, std::move(push));
            add_group(model.entries, { make_entry(u8"checkout_detached", tr(u8"menu.checkout_tag_detached"), icons::icon_target) });
            std::vector<menu_entry> removal {};
            removal.push_back(make_entry(u8"delete_local", tr(u8"menu.delete_local_tag"), icons::icon_trash, true, true));
            if (tag != nullptr)
                for (const std::string& remote : tag->remotes)
                    removal.push_back(make_entry(u8"delete_remote:" + to_u8(remote), tr(u8"menu.delete_tag_from_remote", { { u8"remote", remote } }), icons::icon_trash, true, true));
            add_group(model.entries, std::move(removal));
            add_group(model.entries, { make_entry(u8"copy_name", tr(u8"menu.copy_name"), icons::icon_copy) });
            return model;
        }

        menu_model stash_menu(const menu_target& target)
        {
            menu_model model {};
            model.title = to_u8(target.name);
            add_group(model.entries,
                {
                    make_entry(u8"apply", tr(u8"menu.stash_apply"), icons::icon_check),
                    make_entry(u8"pop", tr(u8"menu.stash_pop"), icons::icon_git_stash_pop),
                });
            add_group(model.entries, { make_entry(u8"drop", tr(u8"menu.delete"), icons::icon_trash, true, true) });
            return model;
        }

        menu_model tags_section_menu(const menu_context& context)
        {
            menu_model model {};
            model.title = u8"TAGS";
            std::vector<menu_entry> push {};
            for (const std::string& remote : context.remotes)
            {
                std::size_t missing { 0 };
                for (const ref& value : context.refs)
                    if (value.kind == ref_kind::tag && std::find(value.remotes.begin(), value.remotes.end(), remote) == value.remotes.end())
                        ++missing;
                push.push_back(
                    make_entry(u8"push_all:" + to_u8(remote), tr(u8"menu.push_all_tags", { { u8"remote", remote }, { u8"count", missing } }), icons::icon_repo_push, context.refs.empty() == false));
            }
            add_group(model.entries, std::move(push));
            add_group(model.entries, { make_entry(u8"create_tag_head", tr(u8"menu.create_tag_at_head"), icons::icon_tag, context.head_sha.empty() == false) });
            return model;
        }

        // 여러 파일을 고른 채 연 메뉴다. 고른 파일 전부에 가는 동작만 둔다 (무시 목록·외부 diff·LFS는 파일 하나의 것이다).
        menu_model files_menu(const menu_target& target)
        {
            menu_model model {};
            model.title = tr(u8"menu.files_title", { { u8"count", target.paths.size() } });
            if (target.conflicted == false)
            {
                if (target.side == change_side::staged)
                    add_group(model.entries, { make_entry(u8"unstage", u8"Unstage", icons::icon_remove) });
                else
                {
                    add_group(model.entries, { make_entry(u8"stage", u8"Stage", icons::icon_add) });
                    const std::u8string label { target.untracked ? tr(u8"menu.move_to_recycle_bin") : tr(u8"menu.discard_changes") };
                    add_group(model.entries, { make_entry(u8"discard", label, target.untracked ? icons::icon_trash : icons::icon_discard, true, true) });
                }
            }
            add_group(model.entries, { make_entry(u8"copy_path", tr(u8"menu.copy_paths"), icons::icon_copy) });
            return model;
        }

        menu_model file_menu(const menu_target& target)
        {
            if (target.paths.size() > 1)
                return files_menu(target);
            menu_model model {};
            model.title = to_u8(target.name);
            if (target.conflicted)
            {
                add_group(model.entries, { make_entry(u8"copy_path", tr(u8"menu.copy_path"), icons::icon_copy) });
                return model;
            }
            if (target.side == change_side::staged)
                add_group(model.entries, { make_entry(u8"unstage", u8"Unstage", icons::icon_remove) });
            else
            {
                add_group(model.entries, { make_entry(u8"stage", u8"Stage", icons::icon_add) });
                add_group(model.entries,
                    {
                        make_entry(
                            u8"discard", target.untracked ? tr(u8"menu.move_to_recycle_bin") : tr(u8"menu.discard_changes"), target.untracked ? icons::icon_trash : icons::icon_discard, true, true),
                        make_entry(u8"ignore", tr(u8"menu.add_to_gitignore"), icons::icon_circle_slash),
                    });
            }
            add_group(model.entries,
                {
                    make_entry(u8"open_external_diff", tr(u8"menu.open_external_diff"), icons::icon_diff, target.untracked == false),
                    make_entry(u8"copy_path", tr(u8"menu.copy_path"), icons::icon_copy),
                });
            // Git LFS: 추적 중인 파일은 객체를 받고, 아닌 파일은 확장자로 추적을 시작한다.
            if (target.lfs)
                add_group(model.entries, { make_entry(u8"lfs_pull", tr(u8"menu.lfs_pull"), icons::icon_cloud_download) });
            else if (const std::string pattern { lfs_pattern_for_path(target.name) }; pattern.empty() == false && target.conflicted == false)
                add_group(model.entries, { make_entry(u8"lfs_track", tr(u8"menu.lfs_track", { { u8"pattern", pattern } }), icons::icon_package) });
            return model;
        }

        // 커밋 상세의 변경 파일이다. 커밋 안의 파일은 고칠 것이 없으니 diff 보기와 복사만 둔다.
        menu_model commit_file_menu(const menu_target& target)
        {
            menu_model model {};
            model.title = to_u8(target.name);
            add_group(model.entries, { make_entry(u8"open_diff", tr(u8"menu.view_changes"), icons::icon_diff) });
            add_group(model.entries,
                {
                    make_entry(u8"copy_path", tr(u8"menu.copy_path"), icons::icon_copy),
                    make_entry(u8"copy_name", tr(u8"menu.copy_file_name"), icons::icon_copy),
                    // `git show abc1234:src/a.cpp`에 그대로 쓰는 꼴이다.
                    make_entry(u8"copy_revision_path", tr(u8"menu.copy_revision_path", { { u8"revision", short_sha(target.sha) + u8":" + to_u8(target.name) } }), icons::icon_copy),
                });
            return model;
        }

        // ---------------------------------------------------------------- 끌어 놓기

        menu_model drop_menu(const menu_target& target, const menu_context& context)
        {
            const drop_party& source { target.source };
            const drop_party& destination { target.destination };
            menu_model model {};
            const std::u8string a { to_u8(source.name) };
            const std::u8string b { destination.is_commit ? short_sha(destination.sha) : to_u8(destination.name) };
            model.title = a + u8" → " + b;
            if (can_drop_on(source, destination) == false)
                return model;
            const drop_facts& facts { context.facts };
            const bool source_is_current { source.kind == ref_kind::local && source.name == context.current_branch && context.detached == false };
            const bool destination_is_current { destination.is_commit == false && destination.kind == ref_kind::local && destination.name == context.current_branch && context.detached == false };

            if (destination.is_commit)
            {
                add_group(model.entries, { make_entry(u8"create_branch_here", tr(u8"menu.create_branch_from_commit"), icons::icon_git_branch_create) });
                if (source_is_current && destination.sha != context.head_sha)
                    add_group(model.entries, reset_group(a, b, true));
                return model;
            }

            const bool same_tip { source.sha == destination.sha };
            if (destination.kind == ref_kind::remote)
            {
                // 원격 브랜치는 직접 바꾸지 않는다. 로컬 끈 쪽을 바꾸는 merge와 rebase만 있다.
                std::vector<menu_entry> group {};
                const bool contained { facts.destination_is_ancestor.value_or(false) };
                group.push_back(make_entry(u8"merge_remote_into_source",
                    tr(u8"menu.merge_into", { { u8"source", b }, { u8"target", a } }) + (contained ? tr(u8"menu.already_included") : count_suffix(facts.only_in_destination)), icons::icon_git_merge,
                    contained == false));
                group.push_back(
                    make_entry(u8"rebase", tr(u8"menu.rebase_onto", { { u8"branch", a }, { u8"onto", b } }) + (contained ? tr(u8"menu.already_on_top") : count_suffix(facts.only_in_source)),
                        icons::icon_repo_forked, contained == false && source.kind == ref_kind::local));
                add_group(model.entries, std::move(group));
                return model;
            }

            std::vector<menu_entry> group {};
            if (same_tip == false && facts.destination_is_ancestor.value_or(true))
                group.push_back(make_entry(u8"fast_forward", tr(u8"menu.fast_forward", { { u8"branch", b }, { u8"to", a } }) + count_suffix(facts.only_in_source), icons::icon_arrow_up,
                    facts.destination_is_ancestor.has_value()));
            const bool contained { same_tip || facts.source_is_ancestor.value_or(false) };
            group.push_back(make_entry(u8"merge", tr(u8"menu.merge_into", { { u8"source", a }, { u8"target", b } }) + (contained ? tr(u8"menu.already_included") : count_suffix(facts.only_in_source)),
                icons::icon_git_merge, contained == false));
            if (source.kind == ref_kind::local)
            {
                const bool on_top { same_tip || facts.destination_is_ancestor.value_or(false) };
                group.push_back(make_entry(u8"rebase", tr(u8"menu.rebase_onto", { { u8"branch", a }, { u8"onto", b } }) + (on_top ? tr(u8"menu.already_on_top") : count_suffix(facts.only_in_source)),
                    icons::icon_repo_forked, on_top == false));
                group.push_back(
                    make_entry(u8"interactive_rebase", tr(u8"menu.interactive_rebase_onto", { { u8"branch", a }, { u8"onto", b } }) + (on_top ? std::u8string {} : count_suffix(facts.only_in_source)),
                        icons::icon_list_ordered, same_tip == false));
            }
            add_group(model.entries, std::move(group));
            if (destination_is_current && same_tip == false)
                add_group(model.entries, reset_group(b, a, true));
            return model;
        }
    } // namespace

    menu_model build_context_menu(const menu_target& target, const menu_context& context)
    {
        switch (target.kind)
        {
        case menu_target_kind::commit:
            return commit_menu(target, context);
        case menu_target_kind::local_branch:
            return local_branch_menu(target, context);
        case menu_target_kind::remote_branch:
            return remote_branch_menu(target, context);
        case menu_target_kind::tag:
            return tag_menu(target, context);
        case menu_target_kind::stash:
            return stash_menu(target);
        case menu_target_kind::file:
            return file_menu(target);
        case menu_target_kind::commit_file:
            return commit_file_menu(target);
        case menu_target_kind::tags_section:
            return tags_section_menu(context);
        case menu_target_kind::drop:
            return drop_menu(target, context);
        case menu_target_kind::submodule:
        case menu_target_kind::submodules_section:
        case menu_target_kind::worktree:
        case menu_target_kind::worktrees_section:
        case menu_target_kind::lfs_section:
        case menu_target_kind::lfs_file:
            return build_extension_menu(target, context);
        case menu_target_kind::tab:
        case menu_target_kind::tab_list:
            // 탭 메뉴는 저장소 상태가 아니라 탭 목록으로 정한다 (`build_tab_menu`·`build_tab_list_menu`).
            break;
        }
        return {};
    }

    menu_model build_tab_menu(const repository_tabs& tabs, const repository_tab_id id)
    {
        const std::optional<std::size_t> index { tabs.index_of(id) };
        if (index.has_value() == false)
            return {};
        const std::vector<repository_tab>& list { tabs.tabs() };
        const repository_tab& tab { list[*index] };
        const bool repository { tab.kind == repository_tab_kind::repository };
        menu_model model {};
        model.title = repository ? tab.display_name : tr(u8"menu.start_tab");
        // 시작 탭 하나만 남았으면 닫아도 같은 시작 탭이 다시 서므로 닫기를 끈다.
        const bool only_start { list.size() == 1 && repository == false };
        add_group(model.entries,
            {
                make_entry(u8"close", tr(u8"menu.close_tab"), icons::icon_close, only_start == false),
                make_entry(u8"close_others", tr(u8"menu.close_other_tabs"), icons::icon_close_all, list.size() > 1),
                make_entry(u8"close_right", tr(u8"menu.close_tabs_right"), icons::icon_close_all, *index + 1 < list.size()),
            });
        if (repository)
        {
            add_group(model.entries, { make_entry(u8"copy_path", tr(u8"menu.copy_path"), icons::icon_copy) });
            add_group(model.entries,
                {
                    make_entry(u8"reveal", tr(u8"menu.reveal_in_explorer"), icons::icon_folder_opened),
                    make_entry(u8"open_vscode", tr(u8"menu.open_in_vscode"), icons::icon_vscode),
                });
        }
        return model;
    }

    menu_model build_tab_list_menu(const repository_tabs& tabs, const repository_tab_id selected)
    {
        menu_model model {};
        model.title = tr(u8"menu.all_tabs");
        for (const repository_tab& tab : tabs.tabs())
        {
            const bool repository { tab.kind == repository_tab_kind::repository };
            const char32_t icon { tab.id == selected ? icons::icon_check : repository ? icons::icon_repo : icons::icon_home };
            model.entries.push_back(make_entry(u8"tab-" + to_u8(std::to_string(tab.id)), repository ? tab.display_name : tr(u8"menu.start_tab"), icon));
        }
        return model;
    }

    std::optional<repository_tab_id> tab_list_entry_id(const std::u8string_view key)
    {
        constexpr std::u8string_view prefix { u8"tab-" };
        if (key.starts_with(prefix) == false)
            return std::nullopt;
        const std::u8string_view digits { key.substr(prefix.size()) };
        repository_tab_id id { 0 };
        const char* const begin { reinterpret_cast<const char*>(digits.data()) };
        const auto [end, error] { std::from_chars(begin, begin + digits.size(), id) };
        if (error != std::errc {} || end != begin + digits.size() || digits.empty())
            return std::nullopt;
        return id;
    }

    bool can_drop_on(const drop_party& source, const drop_party& destination) noexcept
    {
        if (source.is_commit || (source.kind != ref_kind::local && source.kind != ref_kind::remote))
            return false;
        if (destination.is_commit)
            return source.kind == ref_kind::local || source.kind == ref_kind::remote;
        if (destination.kind != ref_kind::local && destination.kind != ref_kind::remote)
            return false;
        if (destination.kind == source.kind && destination.name == source.name)
            return false;
        // 원격에서 원격으로는 바꿀 것이 없다.
        return (source.kind == ref_kind::remote && destination.kind == ref_kind::remote) == false;
    }

    std::optional<std::u8string> describe_drop(const drop_party& source, const drop_party& destination, const std::string& current_branch, const drop_facts& facts)
    {
        if (can_drop_on(source, destination) == false)
            return std::nullopt;
        const std::u8string a { to_u8(source.name) };
        if (destination.is_commit)
        {
            const bool can_reset { source.kind == ref_kind::local && source.name == current_branch && destination.sha != source.sha };
            return can_reset ? tr(u8"menu.drop_hint_branch_or_reset", { { u8"sha", short_sha(destination.sha) } }) : tr(u8"menu.drop_hint_branch", { { u8"sha", short_sha(destination.sha) } });
        }
        const std::u8string b { to_u8(destination.name) };
        if (source.sha == destination.sha)
            return tr(u8"menu.drop_hint_same_commit", { { u8"source", a }, { u8"target", b } });
        if (destination.kind == ref_kind::remote)
            return tr(u8"menu.rebase_onto", { { u8"branch", a }, { u8"onto", b } }) + count_suffix(facts.only_in_source);
        if (facts.destination_is_ancestor.value_or(false))
            return tr(u8"menu.fast_forward", { { u8"branch", b }, { u8"to", a } }) + count_suffix(facts.only_in_source);
        if (facts.source_is_ancestor.value_or(false))
            return tr(u8"menu.drop_hint_already_in", { { u8"source", a }, { u8"target", b } });
        if (source.kind == ref_kind::remote)
            return tr(u8"menu.merge_into", { { u8"source", a }, { u8"target", b } }) + count_suffix(facts.only_in_source);
        return tr(u8"menu.rebase_onto", { { u8"branch", a }, { u8"onto", b } }) + count_suffix(facts.only_in_source);
    }

    std::optional<std::string> menu_key_argument(const std::u8string& key, const std::u8string_view prefix)
    {
        if (key.size() <= prefix.size() || std::u8string_view { key }.substr(0, prefix.size()) != prefix)
            return std::nullopt;
        return std::string { key.begin() + static_cast<std::ptrdiff_t>(prefix.size()), key.end() };
    }

    std::u8string branch_drag_key(const ref_kind kind, const std::string& name)
    {
        switch (kind)
        {
        case ref_kind::local:
            return u8"l:" + to_u8(name);
        case ref_kind::remote:
            return u8"rb:" + to_u8(name);
        case ref_kind::tag:
        case ref_kind::stash:
        case ref_kind::head:
            break;
        }
        return {};
    }

    std::optional<drop_party> parse_branch_drag_key(const std::u8string& key)
    {
        if (const auto name { menu_key_argument(key, u8"l:") }; name.has_value())
            return drop_party { .kind = ref_kind::local, .name = *name };
        if (const auto name { menu_key_argument(key, u8"rb:") }; name.has_value())
            return drop_party { .kind = ref_kind::remote, .name = *name };
        return std::nullopt;
    }
} // namespace cyella
