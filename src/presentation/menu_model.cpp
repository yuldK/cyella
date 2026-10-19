#include "presentation/menu_model.h"

#include "presentation/korean_josa.h"

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
            return u8" · 커밋 " + to_u8(std::to_string(*count)) + u8"개";
        }

        std::vector<menu_entry> reset_group(const std::u8string& what, const std::u8string& to, const bool enabled)
        {
            std::vector<menu_entry> group {};
            group.push_back(make_entry(u8"reset_soft", with_eul_reul(what) + u8" " + with_euro_ro(to) + u8" reset (soft)", icons::icon_debug_step_back, enabled));
            group.push_back(make_entry(u8"reset_mixed", with_eul_reul(what) + u8" " + with_euro_ro(to) + u8" reset (mixed)", icons::icon_debug_step_back, enabled));
            group.push_back(make_entry(u8"reset_hard", with_eul_reul(what) + u8" " + with_euro_ro(to) + u8" reset (hard · 변경을 버림)", icons::icon_trash, enabled, true));
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
                    make_entry(u8"checkout_detached", u8"이 커밋 체크아웃 (detached HEAD)", icons::icon_target, !(is_head && context.detached)),
                    make_entry(u8"create_branch", u8"여기서 브랜치 만들기…", icons::icon_git_branch_create),
                    make_entry(u8"create_tag", u8"여기에 태그 만들기…", icons::icon_tag),
                });
            add_group(entries,
                {
                    make_entry(u8"cherry_pick", context.commit_is_merge ? u8"Cherry-pick (병합 커밋은 부모를 골라야 합니다)" : u8"Cherry-pick", icons::icon_git_pull_request_go_to_changes,
                        context.commit_is_merge == false && is_head == false),
                    make_entry(u8"revert", context.commit_is_merge ? u8"Revert (병합 커밋은 부모를 골라야 합니다)" : u8"Revert", icons::icon_discard, context.commit_is_merge == false),
                });
            const std::u8string where { has_current_branch(context) ? to_u8(context.current_branch) : std::u8string { u8"HEAD" } };
            add_group(entries, reset_group(where, short_sha(target.sha), is_head == false));
            add_group(entries, { make_entry(u8"copy_sha", u8"SHA 복사", icons::icon_copy) });
            model.entries = std::move(entries);
            return model;
        }

        std::vector<menu_entry> merge_rebase_group(const menu_target& target, const menu_context& context)
        {
            const bool current_is_this { target.kind == menu_target_kind::local_branch && target.name == context.current_branch };
            const bool unavailable { has_current_branch(context) == false || current_is_this };
            const std::u8string name { to_u8(target.name) };
            const std::u8string current { has_current_branch(context) ? to_u8(context.current_branch) : std::u8string { u8"현재 브랜치" } };
            std::vector<menu_entry> group {};
            group.push_back(make_entry(u8"merge_into_current", with_eul_reul(name) + u8" " + current + u8"에 merge", icons::icon_git_merge, unavailable == false));
            group.push_back(make_entry(u8"rebase_current_onto", with_eul_reul(current) + u8" " + name + u8" 위로 rebase", icons::icon_repo_forked, unavailable == false));
            return group;
        }

        menu_model local_branch_menu(const menu_target& target, const menu_context& context)
        {
            menu_model model {};
            model.title = to_u8(target.name);
            const bool is_current { target.name == context.current_branch && context.detached == false };
            add_group(model.entries, { make_entry(u8"checkout", is_current ? u8"체크아웃 (현재 브랜치)" : u8"체크아웃", icons::icon_check, is_current == false) });
            add_group(model.entries, merge_rebase_group(target, context));
            add_group(model.entries,
                {
                    make_entry(u8"push", u8"Push…", icons::icon_repo_push),
                    make_entry(u8"set_upstream", u8"upstream 설정…", icons::icon_cloud),
                    make_entry(u8"rename", u8"이름 바꾸기…", icons::icon_edit),
                });
            add_group(model.entries, { make_entry(u8"delete", is_current ? u8"삭제 (현재 브랜치는 삭제할 수 없습니다)" : u8"삭제…", icons::icon_trash, is_current == false, true) });
            add_group(model.entries, { make_entry(u8"copy_name", u8"이름 복사", icons::icon_copy) });
            return model;
        }

        menu_model remote_branch_menu(const menu_target& target, const menu_context& context)
        {
            menu_model model {};
            model.title = to_u8(target.name);
            add_group(model.entries, { make_entry(u8"checkout", u8"체크아웃", icons::icon_check) });
            add_group(model.entries, merge_rebase_group(target, context));
            add_group(model.entries, { make_entry(u8"copy_name", u8"이름 복사", icons::icon_copy) });
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
                push.push_back(make_entry(u8"push:" + to_u8(remote), exists ? to_u8(remote) + u8"에 이미 있음" : to_u8(remote) + u8"으로 push", icons::icon_repo_push, exists == false));
            }
            add_group(model.entries, std::move(push));
            add_group(model.entries, { make_entry(u8"checkout_detached", u8"이 태그 체크아웃 (detached HEAD)", icons::icon_target) });
            std::vector<menu_entry> removal {};
            removal.push_back(make_entry(u8"delete_local", u8"로컬 태그 삭제…", icons::icon_trash, true, true));
            if (tag != nullptr)
                for (const std::string& remote : tag->remotes)
                    removal.push_back(make_entry(u8"delete_remote:" + to_u8(remote), to_u8(remote) + u8"에서도 삭제…", icons::icon_trash, true, true));
            add_group(model.entries, std::move(removal));
            add_group(model.entries, { make_entry(u8"copy_name", u8"이름 복사", icons::icon_copy) });
            return model;
        }

        menu_model stash_menu(const menu_target& target)
        {
            menu_model model {};
            model.title = to_u8(target.name);
            add_group(model.entries,
                {
                    make_entry(u8"apply", u8"Apply (stash를 남겨 둠)", icons::icon_check),
                    make_entry(u8"pop", u8"Pop (적용하고 삭제)", icons::icon_git_stash_pop),
                });
            add_group(model.entries, { make_entry(u8"drop", u8"삭제…", icons::icon_trash, true, true) });
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
                push.push_back(make_entry(u8"push_all:" + to_u8(remote), u8"모든 태그를 " + to_u8(remote) + u8"으로 push · 원격에 없는 태그 " + to_u8(std::to_string(missing)) + u8"개",
                    icons::icon_repo_push, context.refs.empty() == false));
            }
            add_group(model.entries, std::move(push));
            add_group(model.entries, { make_entry(u8"create_tag_head", u8"HEAD에 새 태그 만들기…", icons::icon_tag, context.head_sha.empty() == false) });
            return model;
        }

        menu_model file_menu(const menu_target& target)
        {
            menu_model model {};
            model.title = to_u8(target.name);
            if (target.conflicted)
            {
                add_group(model.entries, { make_entry(u8"copy_path", u8"경로 복사", icons::icon_copy) });
                return model;
            }
            if (target.side == change_side::staged)
                add_group(model.entries, { make_entry(u8"unstage", u8"Unstage", icons::icon_remove) });
            else
            {
                add_group(model.entries, { make_entry(u8"stage", u8"Stage", icons::icon_add) });
                add_group(model.entries,
                    {
                        make_entry(u8"discard", target.untracked ? u8"변경 버리기 (추적하지 않는 파일은 버릴 수 없습니다)" : u8"변경 버리기…", icons::icon_discard, target.untracked == false, true),
                        make_entry(u8"ignore", u8"무시 목록(.gitignore)에 추가", icons::icon_circle_slash),
                    });
            }
            add_group(model.entries,
                {
                    make_entry(u8"open_external_diff", u8"외부 diff 도구로 열기", icons::icon_diff, target.untracked == false),
                    make_entry(u8"copy_path", u8"경로 복사", icons::icon_copy),
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
                add_group(model.entries, { make_entry(u8"create_branch_here", u8"이 커밋에서 브랜치 만들기…", icons::icon_git_branch_create) });
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
                group.push_back(
                    make_entry(u8"merge_remote_into_source", with_eul_reul(b) + u8" " + a + u8"에 merge" + (contained ? std::u8string { u8" · 이미 포함" } : count_suffix(facts.only_in_destination)),
                        icons::icon_git_merge, contained == false));
                group.push_back(make_entry(u8"rebase", with_eul_reul(a) + u8" " + b + u8" 위로 rebase" + (contained ? std::u8string { u8" · 이미 위에 있음" } : count_suffix(facts.only_in_source)),
                    icons::icon_repo_forked, contained == false && source.kind == ref_kind::local));
                add_group(model.entries, std::move(group));
                return model;
            }

            std::vector<menu_entry> group {};
            if (same_tip == false && facts.destination_is_ancestor.value_or(true))
                group.push_back(make_entry(u8"fast_forward", with_eul_reul(b) + u8" " + with_euro_ro(a) + u8" fast-forward" + count_suffix(facts.only_in_source), icons::icon_arrow_up,
                    facts.destination_is_ancestor.has_value()));
            const bool contained { same_tip || facts.source_is_ancestor.value_or(false) };
            group.push_back(make_entry(u8"merge", with_eul_reul(a) + u8" " + b + u8"에 merge" + (contained ? std::u8string { u8" · 이미 포함" } : count_suffix(facts.only_in_source)),
                icons::icon_git_merge, contained == false));
            if (source.kind == ref_kind::local)
            {
                const bool on_top { same_tip || facts.destination_is_ancestor.value_or(false) };
                group.push_back(make_entry(u8"rebase", with_eul_reul(a) + u8" " + b + u8" 위로 rebase" + (on_top ? std::u8string { u8" · 이미 위에 있음" } : count_suffix(facts.only_in_source)),
                    icons::icon_repo_forked, on_top == false));
                group.push_back(make_entry(u8"interactive_rebase", with_eul_reul(a) + u8" " + b + u8" 위로 인터랙티브 rebase", icons::icon_list_ordered, false));
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
        case menu_target_kind::tags_section:
            return tags_section_menu(context);
        case menu_target_kind::drop:
            return drop_menu(target, context);
        }
        return {};
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
            return short_sha(destination.sha) + u8" 커밋에서 브랜치 만들기" + (can_reset ? std::u8string { u8" 또는 reset" } : std::u8string {});
        }
        const std::u8string b { to_u8(destination.name) };
        if (source.sha == destination.sha)
            return a + u8" · " + b + u8": 같은 커밋이라 바꿀 것이 없습니다";
        if (destination.kind == ref_kind::remote)
            return with_eul_reul(a) + u8" " + b + u8" 위로 rebase" + count_suffix(facts.only_in_source);
        if (facts.destination_is_ancestor.value_or(false))
            return with_eul_reul(b) + u8" " + with_euro_ro(a) + u8" fast-forward" + count_suffix(facts.only_in_source);
        if (facts.source_is_ancestor.value_or(false))
            return with_eun_neun(a) + u8" 이미 " + b + u8"에 들어 있음";
        if (source.kind == ref_kind::remote)
            return with_eul_reul(a) + u8" " + b + u8"에 merge" + count_suffix(facts.only_in_source);
        return with_eul_reul(a) + u8" " + b + u8" 위로 rebase" + count_suffix(facts.only_in_source);
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
