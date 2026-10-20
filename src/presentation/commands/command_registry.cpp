#include "presentation/commands/command_registry.h"

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
            // ---- 앱
            list.push_back(make("palette.open", u8"명령 팔레트 열기", u8"command palette search run", group::app, { u8"Ctrl+Shift+P", u8"Ctrl+P" }));
            list.push_back(make("app.settings", u8"환경 설정 열기", u8"settings preferences options", group::app, { u8"Ctrl+," }));
            list.push_back(make("app.settings.general", u8"환경 설정: 일반 (테마·git 경로)", u8"settings general theme git path appearance", group::app));
            list.push_back(make("app.settings.diff", u8"환경 설정: diff 보기·외부 도구", u8"settings diff tool external layout context", group::app));
            list.push_back(make("app.settings.ssh", u8"환경 설정: SSH 키·agent", u8"settings ssh key agent keygen", group::app));
            list.push_back(make("app.settings.profiles", u8"환경 설정: 사용자 프로필", u8"settings profile identity user name email author", group::app));
            list.push_back(make("app.settings.hosting", u8"환경 설정: 호스팅 (GitHub·GitLab)", u8"settings hosting github gitlab server host token sign out logout", group::app));
            list.push_back(make("app.settings.shortcuts", u8"환경 설정: 단축키", u8"settings shortcut keybinding keys hotkey", group::app));
            list.push_back(make("app.theme", u8"테마 바꾸기 (시스템·밝게·어둡게)", u8"theme dark light color mode", group::app));
            list.push_back(make("app.log", u8"작업 로그 보기·숨기기", u8"log operations output command history", group::app));
            // ---- 탭
            list.push_back(make("tab.new", u8"새 탭 (시작 페이지)", u8"new tab start page", group::tab, { u8"Ctrl+T" }));
            list.push_back(make("tab.close", u8"현재 탭 닫기", u8"close tab", group::tab, { u8"Ctrl+W" }));
            list.push_back(make("tab.next", u8"다음 탭", u8"next tab switch", group::tab, { u8"Ctrl+Tab" }));
            list.push_back(make("tab.previous", u8"이전 탭", u8"previous tab switch back", group::tab, { u8"Ctrl+Shift+Tab" }));
            // ---- 저장소
            list.push_back(make("repo.open", u8"저장소 열기…", u8"open repository recent switch tab", group::repository, { u8"Ctrl+O" }));
            list.push_back(make("repo.open_folder", u8"폴더에서 저장소 열기…", u8"open folder browse directory", group::repository));
            list.push_back(make("repo.clone", u8"저장소 복제 (Clone)…", u8"clone download remote url", group::repository));
            list.push_back(make("repo.init", u8"폴더에 새 저장소 만들기 (Init)…", u8"init create new repository", group::repository));
            list.push_back(make("repo.refresh", u8"저장소 새로 고침", u8"refresh reload status", group::repository, { u8"F5" }));
            // ---- git 작업
            list.push_back(make("git.fetch", u8"원격에서 가져오기 (Fetch)", u8"fetch remote download update", group::git));
            list.push_back(make("git.pull", u8"원격 변경 받아 합치기 (Pull)", u8"pull merge remote update", group::git));
            list.push_back(make("git.push", u8"원격으로 올리기 (Push)", u8"push upload remote publish", group::git));
            list.push_back(make("git.branch.new", u8"새 브랜치 만들기…", u8"branch create new", group::git));
            list.push_back(make("git.branch.checkout", u8"브랜치로 이동…", u8"branch checkout switch go", group::git));
            list.push_back(make("git.tag.new", u8"태그 만들기… (현재 커밋)", u8"tag create new release", group::git));
            list.push_back(make("git.stash", u8"변경을 임시 보관 (Stash)", u8"stash save shelve", group::git));
            list.push_back(make("git.stash.pop", u8"임시 보관한 변경 꺼내기 (Pop)", u8"stash pop apply restore", group::git));
            list.push_back(make("git.commit", u8"커밋하기", u8"commit save record", group::git, { u8"Ctrl+Enter" }, command_scope::commit_input));
            list.push_back(make("git.amend", u8"직전 커밋 고쳐 쓰기 (Amend) 켜기·끄기", u8"amend commit fix last", group::git));
            // ---- 화면
            list.push_back(make("view.drawer", u8"상세 패널 열기·닫기", u8"detail panel drawer sidebar", group::view));
            list.push_back(make("view.menu", u8"고른 항목의 메뉴 열기", u8"context menu right click actions", group::view, { u8"Shift+F10" }));
            list.push_back(make("view.close", u8"열린 패널·알림 닫기", u8"close dismiss escape overlay", group::view, { u8"Escape" }, command_scope::global, false));
            // ---- GitHub·GitLab
            list.push_back(make("hosting.sign_in", u8"GitHub·GitLab에 로그인…", u8"login sign in github gitlab credential token", group::hosting));
            list.push_back(make("hosting.refresh", u8"PR·MR 목록 새로 고침", u8"pull request merge request list refresh github gitlab", group::hosting));
            list.push_back(make("hosting.pull_request.new", u8"현재 브랜치로 PR·MR 만들기…", u8"pull request merge request create new github gitlab", group::hosting));
            // ---- 프로필
            list.push_back(make("profile.apply", u8"이 저장소에 프로필 적용…", u8"profile apply identity author user name email", group::profile));
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

    std::u8string_view command_group_title(const command_group group) noexcept
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
        if (id == "git.amend" || id == "repo.refresh")
            return availability.repository;
        if (id == "view.drawer")
            return availability.repository && availability.drawer;
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
        return default_shortcuts(command);
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
            check.error = u8"알 수 없는 명령입니다.";
            return check;
        }
        chord_list_parse parsed { parse_key_chord_list(text) };
        if (parsed.failed.empty() == false)
        {
            check.error = u8"'" + parsed.failed + u8"'은(는) 읽을 수 없는 키입니다. 예: Ctrl+Shift+K (여러 개는 ' / '로 이어 씁니다)";
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
                check.error = format_key_chord(chord) + u8"은(는) 이미 '" + (owner != nullptr ? owner->title : std::u8string(other->begin(), other->end())) + u8"'에 쓰입니다.";
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
