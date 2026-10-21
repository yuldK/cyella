#include "presentation/commands/command_registry.h"

#include "domain/text_catalog.h"

#include <algorithm>
#include <utility>

namespace cyella {
    namespace {
        command_definition make(std::string id, std::u8string title, std::u8string keywords, const command_group group, std::vector<std::u8string> shortcuts = {},
            const command_scope scope = command_scope::global, const bool in_palette = true)
        {
            return command_definition {
                .id = std::move(id), .title = std::move(title), .keywords = std::move(keywords), .group = group, .scope = scope, .default_shortcuts = std::move(shortcuts), .in_palette = in_palette
            };
        }

        std::vector<command_definition> build_definitions()
        {
            using group = command_group;
            std::vector<command_definition> list {};
            // ---- 작업 보기
            list.push_back(make("view.changes", tr(u8"command.view.changes"), u8"changes working files stage commit workspace", group::view, { u8"Ctrl+1" }));
            list.push_back(make("view.history", tr(u8"command.view.history"), u8"history graph log commits workspace", group::view, { u8"Ctrl+2" }));
            // ---- 앱
            list.push_back(make("palette.open", tr(u8"command.palette.open"), u8"command palette search run", group::app, { u8"Ctrl+Shift+P", u8"Ctrl+P" }));
            list.push_back(make("app.settings", tr(u8"command.app.settings"), u8"settings preferences options", group::app, { u8"Ctrl+," }));
            list.push_back(make("app.settings.general", tr(u8"command.app.settings.general"), u8"settings general theme git path appearance", group::app));
            list.push_back(make("app.settings.diff", tr(u8"command.app.settings.diff"), u8"settings diff tool external layout context", group::app));
            list.push_back(make("app.settings.ssh", tr(u8"command.app.settings.ssh"), u8"settings ssh key agent keygen", group::app));
            list.push_back(make("app.settings.profiles", tr(u8"command.app.settings.profiles"), u8"settings profile identity user name email author", group::app));
            list.push_back(make("app.settings.hosting", tr(u8"command.app.settings.hosting"), u8"settings hosting github gitlab server host token sign out logout", group::app));
            list.push_back(make("app.settings.shortcuts", tr(u8"command.app.settings.shortcuts"), u8"settings shortcut keybinding keys hotkey", group::app));
            list.push_back(make("app.theme", tr(u8"command.app.theme"), u8"theme dark light color mode", group::app));
            list.push_back(make("app.log", tr(u8"command.app.log"), u8"log operations output command history", group::app));
            // ---- 탭
            list.push_back(make("tab.new", tr(u8"command.tab.new"), u8"new tab start page", group::tab, { u8"Ctrl+T" }));
            list.push_back(make("tab.close", tr(u8"command.tab.close"), u8"close tab", group::tab, { u8"Ctrl+W" }));
            list.push_back(make("tab.next", tr(u8"command.tab.next"), u8"next tab switch", group::tab, { u8"Ctrl+Tab" }));
            list.push_back(make("tab.previous", tr(u8"command.tab.previous"), u8"previous tab switch back", group::tab, { u8"Ctrl+Shift+Tab" }));
            // ---- 저장소
            list.push_back(make("repo.open", tr(u8"command.repo.open"), u8"open repository recent switch tab", group::repository, { u8"Ctrl+O" }));
            list.push_back(make("repo.open_folder", tr(u8"command.repo.open_folder"), u8"open folder browse directory", group::repository));
            list.push_back(make("repo.clone", tr(u8"command.repo.clone"), u8"clone download remote url", group::repository));
            list.push_back(make("repo.init", tr(u8"command.repo.init"), u8"init create new repository", group::repository));
            list.push_back(make("repo.refresh", tr(u8"command.repo.refresh"), u8"refresh reload status", group::repository, { u8"F5" }));
            list.push_back(make("repo.reveal", tr(u8"command.repo.reveal"), u8"open folder explorer reveal show files", group::repository));
            list.push_back(make("repo.open_vscode", tr(u8"command.repo.open_vscode"), u8"open vscode visual studio code editor", group::repository));
            // ---- git 작업
            list.push_back(make("git.fetch", tr(u8"command.git.fetch"), u8"fetch remote download update", group::git));
            list.push_back(make("git.pull", tr(u8"command.git.pull"), u8"pull merge remote update", group::git));
            list.push_back(make("git.push", tr(u8"command.git.push"), u8"push upload remote publish", group::git));
            list.push_back(make("git.branch.new", tr(u8"command.git.branch.new"), u8"branch create new", group::git));
            list.push_back(make("git.branch.checkout", tr(u8"command.git.branch.checkout"), u8"branch checkout switch go", group::git));
            list.push_back(make("git.tag.new", tr(u8"command.git.tag.new"), u8"tag create new release", group::git));
            list.push_back(make("git.stash", tr(u8"command.git.stash"), u8"stash save shelve", group::git));
            list.push_back(make("git.stash.pop", tr(u8"command.git.stash.pop"), u8"stash pop apply restore", group::git));
            list.push_back(make("git.commit", tr(u8"command.git.commit"), u8"commit save record", group::git, { u8"Ctrl+Enter" }, command_scope::commit_input));
            list.push_back(make("git.amend", tr(u8"command.git.amend"), u8"amend commit fix last", group::git));
            // ---- 화면
            list.push_back(make("view.drawer", tr(u8"command.view.drawer"), u8"detail panel drawer sidebar", group::view));
            list.push_back(make("view.menu", tr(u8"command.view.menu"), u8"context menu right click actions", group::view, { u8"Shift+F10" }));
            list.push_back(make("view.close", tr(u8"command.view.close"), u8"close dismiss escape overlay", group::view, { u8"Escape" }, command_scope::global, false));
            // ---- GitHub·GitLab
            list.push_back(make("hosting.sign_in", tr(u8"command.hosting.sign_in"), u8"login sign in github gitlab credential token", group::hosting));
            list.push_back(make("hosting.refresh", tr(u8"command.hosting.refresh"), u8"pull request merge request list refresh github gitlab", group::hosting));
            list.push_back(make("hosting.pull_request.new", tr(u8"command.hosting.pull_request.new"), u8"pull request merge request create new github gitlab", group::hosting));
            // ---- 프로필
            list.push_back(make("profile.apply", tr(u8"command.profile.apply"), u8"profile apply identity author user name email", group::profile));
            return list;
        }

        // 설정 파일의 조각을 단축키로 바꾼다. 읽을 수 없는 조각은 건너뛴다.
        std::vector<key_chord> parse_all(const std::vector<std::u8string>& texts)
        {
            std::vector<key_chord> chords {};
            for (const std::u8string& text : texts)
                if (const std::optional<key_chord> chord { parse_key_chord(text) }; chord.has_value() && std::ranges::find(chords, *chord) == chords.end())
                    chords.push_back(*chord);
            return chords;
        }

        std::vector<std::u8string> format_all(const std::vector<key_chord>& chords)
        {
            std::vector<std::u8string> texts {};
            for (const key_chord& chord : chords)
                texts.push_back(format_key_chord(chord));
            return texts;
        }
    } // namespace

    const std::vector<command_definition>& command_definitions()
    {
        static const std::vector<command_definition> definitions { build_definitions() };
        return definitions;
    }

    const command_definition* find_command(const std::string_view id)
    {
        for (const command_definition& definition : command_definitions())
            if (definition.id == id)
                return &definition;
        return nullptr;
    }

    std::u8string command_group_title(const command_group group)
    {
        switch (group)
        {
        case command_group::app:
            return tr(u8"command_group.app");
        case command_group::tab:
            return tr(u8"command_group.tab");
        case command_group::repository:
            return tr(u8"command_group.repository");
        case command_group::git:
            return tr(u8"command_group.git");
        case command_group::view:
            return tr(u8"command_group.view");
        case command_group::profile:
            return tr(u8"command_group.profile");
        case command_group::hosting:
            return u8"GitHub·GitLab";
        }
        return {};
    }

    std::u8string_view command_group_owner(const command_group group) noexcept
    {
        switch (group)
        {
        case command_group::app:
            return u8"앱";
        case command_group::tab:
            return u8"탭";
        case command_group::repository:
            return u8"저장소";
        case command_group::git:
            return u8"Git 작업";
        case command_group::view:
            return u8"화면";
        case command_group::profile:
            return u8"사용자 프로필";
        case command_group::hosting:
            return u8"GitHub·GitLab";
        }
        return {};
    }

    bool command_available(const std::string_view id, const command_availability& availability) noexcept
    {
        if (id == "git.fetch")
            return availability.fetch;
        if (id == "git.pull")
            return availability.pull;
        if (id == "git.push")
            return availability.push;
        if (id == "git.branch.new" || id == "git.tag.new")
            return availability.branch;
        if (id == "git.branch.checkout")
            return availability.repository;
        if (id == "git.stash")
            return availability.stash;
        if (id == "git.stash.pop")
            return availability.pop;
        if (id == "git.commit")
            return availability.commit;
        if (id == "git.amend" || id == "repo.refresh" || id == "repo.reveal" || id == "repo.open_vscode")
            return availability.repository;
        if (id == "view.drawer")
            return availability.repository && availability.drawer;
        if (id == "view.changes" || id == "view.history")
            return availability.repository;
        if (id == "view.menu")
            return availability.selection_menu;
        if (id == "profile.apply")
            return availability.repository;
        // 로그인은 서비스를 아는 원격이 있을 때, 목록·만들기는 로그인한 뒤에만 쓸 수 있다.
        if (id == "hosting.sign_in")
            return availability.hosting_supported && availability.hosting_signed_in == false;
        if (id == "hosting.refresh")
            return availability.hosting_supported && availability.hosting_signed_in;
        if (id == "hosting.pull_request.new")
            return availability.hosting_can_create;
        return true;
    }

    std::vector<key_chord> default_shortcuts(const command_definition& command)
    {
        return parse_all(command.default_shortcuts);
    }

    std::vector<key_chord> effective_shortcuts(const app_settings& settings, const command_definition& command)
    {
        if (const auto found { settings.shortcuts.find(command.id) }; found != settings.shortcuts.end())
            return parse_all(found->second);
        std::vector<key_chord> defaults { default_shortcuts(command) };
        // 새 기본 단축키가 이전에 사용자가 직접 지정한 키를 가로채지 않게 한다.
        std::erase_if(defaults, [&settings, &command](const key_chord& chord) {
            for (const auto& [id, texts] : settings.shortcuts)
                if (id != command.id && find_command(id) != nullptr)
                    if (const auto custom { parse_all(texts) }; std::ranges::find(custom, chord) != custom.end())
                        return true;
            return false;
        });
        return defaults;
    }

    bool shortcuts_customized(const app_settings& settings, const std::string_view id)
    {
        return settings.shortcuts.contains(std::string { id });
    }

    std::vector<shortcut_binding> resolve_shortcut_bindings(const app_settings& settings)
    {
        std::vector<shortcut_binding> bindings {};
        for (const command_definition& command : command_definitions())
            for (const key_chord& chord : effective_shortcuts(settings, command))
                bindings.push_back(shortcut_binding { .chord = chord, .command = command.id, .scope = command.scope });
        return bindings;
    }

    std::optional<std::string> find_shortcut_conflict(const app_settings& settings, const std::string_view command, const key_chord& chord)
    {
        for (const command_definition& other : command_definitions())
        {
            if (other.id == command)
                continue;
            const std::vector<key_chord> chords { effective_shortcuts(settings, other) };
            if (std::ranges::find(chords, chord) != chords.end())
                return other.id;
        }
        return std::nullopt;
    }

    shortcut_edit_check check_shortcut_edit(const app_settings& settings, const std::string_view command, const std::u8string_view text)
    {
        shortcut_edit_check check {};
        const command_definition* const definition { find_command(command) };
        if (definition == nullptr)
        {
            check.error = tr(u8"command.unknown_command");
            return check;
        }
        chord_list_parse parsed { parse_key_chord_list(text) };
        if (parsed.failed.empty() == false)
        {
            check.error = tr(u8"command.unreadable_key", { { u8"key", parsed.failed } });
            return check;
        }
        for (const key_chord& chord : parsed.chords)
        {
            if (const std::u8string problem { check_key_chord(chord) }; problem.empty() == false)
            {
                check.error = format_key_chord(chord) + u8": " + problem;
                return check;
            }
            if (const std::optional<std::string> other { find_shortcut_conflict(settings, command, chord) }; other.has_value())
            {
                const command_definition* const owner { find_command(*other) };
                check.error = tr(u8"command.shortcut_in_use", { { u8"key", format_key_chord(chord) }, { u8"command", owner != nullptr ? owner->title : std::u8string(other->begin(), other->end()) } });
                return check;
            }
        }
        check.chords = std::move(parsed.chords);
        return check;
    }

    void set_command_shortcuts(app_settings& settings, const std::string_view command, const std::vector<key_chord>& chords)
    {
        const command_definition* const definition { find_command(command) };
        if (definition == nullptr)
            return;
        // 기본값과 같으면 바꾼 기록을 남기지 않는다 (순서를 바꾼 것도 같은 것으로 본다).
        std::vector<key_chord> defaults { default_shortcuts(*definition) };
        std::vector<key_chord> wanted { chords };
        const auto by_text = [](const key_chord& left, const key_chord& right) { return format_key_chord(left) < format_key_chord(right); };
        std::ranges::sort(defaults, by_text);
        std::ranges::sort(wanted, by_text);
        if (defaults == wanted)
        {
            settings.shortcuts.erase(std::string { command });
            return;
        }
        settings.shortcuts[std::string { command }] = format_all(chords);
    }

    void reset_command_shortcuts(app_settings& settings, const std::string_view command)
    {
        settings.shortcuts.erase(std::string { command });
    }

    void reset_all_shortcuts(app_settings& settings)
    {
        settings.shortcuts.clear();
    }

    std::u8string shortcut_text(const app_settings& settings, const std::string_view command)
    {
        const command_definition* const definition { find_command(command) };
        return definition == nullptr ? std::u8string {} : format_key_chord_list(effective_shortcuts(settings, *definition));
    }
} // namespace cyella
