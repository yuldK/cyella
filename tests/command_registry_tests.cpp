#include "infrastructure/json_app_settings_store.h"
#include "presentation/commands/command_registry.h"
#include "presentation/commands/key_chord.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>
#include <string>

using namespace cyella;

namespace {
    key_chord chord(const std::u8string_view text)
    {
        const std::optional<key_chord> parsed { parse_key_chord(text) };
        REQUIRE(parsed.has_value());
        return *parsed;
    }

    std::optional<std::string> command_for(const std::vector<shortcut_binding>& bindings, const key_chord& wanted)
    {
        for (const shortcut_binding& binding : bindings)
            if (binding.chord == wanted)
                return binding.command;
        return std::nullopt;
    }
} // namespace

TEST_CASE("key chords parse and print in the canonical form", "[commands]")
{
    REQUIRE(format_key_chord(chord(u8"Ctrl+Shift+P")) == u8"Ctrl+Shift+P");
    // 수정키 순서와 대소문자는 자유이고 출력은 Ctrl → Alt → Shift다.
    REQUIRE(format_key_chord(chord(u8"shift+ctrl+p")) == u8"Ctrl+Shift+P");
    REQUIRE(format_key_chord(chord(u8"alt+control+left")) == u8"Ctrl+Alt+Left");
    REQUIRE(format_key_chord(chord(u8"ctrl+enter")) == u8"Ctrl+Enter");
    REQUIRE(format_key_chord(chord(u8"f5")) == u8"F5");
    REQUIRE(format_key_chord(chord(u8"Ctrl+,")) == u8"Ctrl+,");
    REQUIRE(format_key_chord(chord(u8"Ctrl+Shift+Tab")) == u8"Ctrl+Shift+Tab");
    REQUIRE(format_key_chord(chord(u8"escape")) == u8"Escape");
    REQUIRE(normalize_key_chord_text(u8" ctrl+1 ") == u8"Ctrl+1");
    REQUIRE(normalize_key_chord_text(u8"Ctrl+Nope").empty());
    REQUIRE(parse_key_chord(u8"").has_value() == false);
    REQUIRE(parse_key_chord(u8"Ctrl+").has_value() == false);
}

TEST_CASE("a key event becomes a chord", "[commands]")
{
    luil::key_pressed_event event {};
    event.key = luil::key_code::key_p;
    event.control = true;
    event.shift = true;
    REQUIRE(chord_of(event) == chord(u8"Ctrl+Shift+P"));
    luil::key_pressed_event bare {};
    REQUIRE(chord_of(bare).valid() == false);
}

TEST_CASE("only keys that reach the app can be shortcuts", "[commands]")
{
    REQUIRE(check_key_chord(chord(u8"Ctrl+K")).empty());
    REQUIRE(check_key_chord(chord(u8"Alt+K")).empty());
    REQUIRE(check_key_chord(chord(u8"F5")).empty());
    REQUIRE(check_key_chord(chord(u8"Shift+F10")).empty());
    REQUIRE(check_key_chord(chord(u8"Escape")).empty());
    // 글자는 수정키 없이 문자 입력으로만 온다.
    REQUIRE(check_key_chord(chord(u8"K")).empty() == false);
    REQUIRE(check_key_chord(chord(u8"Shift+K")).empty() == false);
    REQUIRE(check_key_chord(chord(u8"Enter")).empty() == false);
    // 텍스트 칸의 편집 키와 창 닫기는 예약이다.
    REQUIRE(check_key_chord(chord(u8"Ctrl+C")).empty() == false);
    REQUIRE(check_key_chord(chord(u8"Ctrl+V")).empty() == false);
    REQUIRE(check_key_chord(chord(u8"Alt+F4")).empty() == false);
    REQUIRE(check_key_chord(key_chord {}).empty() == false);
}

TEST_CASE("chord lists split on slash with spaces", "[commands]")
{
    const chord_list_parse parsed { parse_key_chord_list(u8"Ctrl+P / Ctrl+Shift+P") };
    REQUIRE(parsed.failed.empty());
    REQUIRE(parsed.chords.size() == 2);
    REQUIRE(format_key_chord_list(parsed.chords) == u8"Ctrl+P / Ctrl+Shift+P");
    // 같은 키를 두 번 쓰면 하나로 합친다. 빈 글은 키 없음이다.
    REQUIRE(parse_key_chord_list(u8"Ctrl+P / ctrl+p").chords.size() == 1);
    REQUIRE(parse_key_chord_list(u8"").chords.empty());
    // `Ctrl+/`의 슬래시는 구분자가 아니다.
    REQUIRE(parse_key_chord_list(u8"Ctrl+/").chords.size() == 1);
    const chord_list_parse broken { parse_key_chord_list(u8"Ctrl+P / Ctrl+Nope") };
    REQUIRE(broken.failed == u8"Ctrl+Nope");
    REQUIRE(broken.chords.empty());
}

TEST_CASE("the registry has unique ids and valid, conflict-free defaults", "[commands]")
{
    std::set<std::string> ids {};
    std::set<std::u8string> used {};
    for (const command_definition& command : command_definitions())
    {
        REQUIRE(ids.insert(command.id).second);
        REQUIRE(command.title.empty() == false);
        for (const std::u8string& text : command.default_shortcuts)
        {
            const std::optional<key_chord> parsed { parse_key_chord(text) };
            INFO(command.id);
            REQUIRE(parsed.has_value());
            REQUIRE(format_key_chord(*parsed) == text);
            REQUIRE(check_key_chord(*parsed).empty());
            REQUIRE(used.insert(text).second);
        }
    }
}

TEST_CASE("the existing hard-coded keys are registry entries", "[commands]")
{
    const std::vector<shortcut_binding> bindings { resolve_shortcut_bindings(app_settings {}) };
    REQUIRE(command_for(bindings, chord(u8"Ctrl+Enter")) == "git.commit");
    REQUIRE(command_for(bindings, chord(u8"Shift+F10")) == "view.menu");
    REQUIRE(command_for(bindings, chord(u8"F5")) == "repo.refresh");
    REQUIRE(command_for(bindings, chord(u8"Ctrl+W")) == "tab.close");
    REQUIRE(command_for(bindings, chord(u8"Ctrl+Tab")) == "tab.next");
    REQUIRE(command_for(bindings, chord(u8"Ctrl+Shift+Tab")) == "tab.previous");
    REQUIRE(command_for(bindings, chord(u8"Escape")) == "view.close");
    REQUIRE(command_for(bindings, chord(u8"Ctrl+Shift+P")) == "palette.open");
    REQUIRE(command_for(bindings, chord(u8"Ctrl+P")) == "palette.open");
    const auto commit { std::ranges::find(bindings, std::string { "git.commit" }, &shortcut_binding::command) };
    REQUIRE(commit != bindings.end());
    REQUIRE(commit->scope == command_scope::commit_input);
}

TEST_CASE("a customised shortcut replaces the default and frees the old key", "[commands]")
{
    app_settings settings {};
    set_command_shortcuts(settings, "tab.close", { chord(u8"Ctrl+Q") });
    REQUIRE(shortcuts_customized(settings, "tab.close"));
    std::vector<shortcut_binding> bindings { resolve_shortcut_bindings(settings) };
    REQUIRE(command_for(bindings, chord(u8"Ctrl+Q")) == "tab.close");
    REQUIRE(command_for(bindings, chord(u8"Ctrl+W")).has_value() == false);
    // 비워 두면 키 없음이다.
    set_command_shortcuts(settings, "tab.close", {});
    REQUIRE(settings.shortcuts.at("tab.close").empty());
    bindings = resolve_shortcut_bindings(settings);
    REQUIRE(command_for(bindings, chord(u8"Ctrl+Q")).has_value() == false);
    // 기본값으로 되돌린다.
    reset_command_shortcuts(settings, "tab.close");
    REQUIRE(shortcuts_customized(settings, "tab.close") == false);
    REQUIRE(command_for(resolve_shortcut_bindings(settings), chord(u8"Ctrl+W")) == "tab.close");
    // 기본값과 같은 값을 넣으면 바꾼 기록이 남지 않는다.
    set_command_shortcuts(settings, "tab.close", { chord(u8"Ctrl+W") });
    REQUIRE(settings.shortcuts.empty());
}

TEST_CASE("a shortcut that another command uses is a conflict", "[commands]")
{
    app_settings settings {};
    REQUIRE(find_shortcut_conflict(settings, "git.fetch", chord(u8"Ctrl+W")) == std::string { "tab.close" });
    // 자기 자신의 키는 충돌이 아니다.
    REQUIRE(find_shortcut_conflict(settings, "tab.close", chord(u8"Ctrl+W")).has_value() == false);
    REQUIRE(find_shortcut_conflict(settings, "git.fetch", chord(u8"Ctrl+Alt+K")).has_value() == false);

    const shortcut_edit_check taken { check_shortcut_edit(settings, "git.fetch", u8"Ctrl+W") };
    REQUIRE(taken.error.empty() == false);
    REQUIRE(taken.chords.empty());
    REQUIRE(taken.error.find(u8"현재 탭 닫기") != std::u8string::npos);

    const shortcut_edit_check free_key { check_shortcut_edit(settings, "git.fetch", u8"Ctrl+Alt+K / F7") };
    REQUIRE(free_key.error.empty());
    REQUIRE(free_key.chords.size() == 2);

    REQUIRE(check_shortcut_edit(settings, "git.fetch", u8"K").error.empty() == false);
    REQUIRE(check_shortcut_edit(settings, "git.fetch", u8"Ctrl+Nope").error.empty() == false);
    REQUIRE(check_shortcut_edit(settings, "no.such.command", u8"Ctrl+K").error.empty() == false);
    // 비운 글은 "키 없음"으로 적용할 수 있다.
    REQUIRE(check_shortcut_edit(settings, "git.fetch", u8"").error.empty());

    // 옮겨 간 키는 다른 명령이 쓸 수 있다.
    set_command_shortcuts(settings, "tab.close", { chord(u8"Ctrl+Q") });
    REQUIRE(find_shortcut_conflict(settings, "git.fetch", chord(u8"Ctrl+W")).has_value() == false);
    REQUIRE(find_shortcut_conflict(settings, "git.fetch", chord(u8"Ctrl+Q")) == std::string { "tab.close" });
}

TEST_CASE("customised shortcuts survive a settings round trip", "[commands]")
{
    app_settings settings {};
    set_command_shortcuts(settings, "tab.close", { chord(u8"Ctrl+Q"), chord(u8"Ctrl+Alt+W") });
    set_command_shortcuts(settings, "view.menu", {});
    record_command_use(settings, "git.fetch");
    record_command_use(settings, "git.pull");
    record_command_use(settings, "git.fetch");
    REQUIRE(settings.command_history == std::vector<std::string> { "git.fetch", "git.pull" });

    const std::u8string json { serialize_app_settings_json(settings, u8"{\"future_key\": 7}") };
    const app_settings_load_result loaded { parse_app_settings_json(json) };
    REQUIRE(loaded.settings.shortcuts == settings.shortcuts);
    REQUIRE(loaded.settings.command_history == settings.command_history);
    REQUIRE(json.find(u8"future_key") != std::u8string::npos);
    REQUIRE(command_for(resolve_shortcut_bindings(loaded.settings), chord(u8"Ctrl+Q")) == "tab.close");
}

TEST_CASE("the command history keeps the latest uses first within its capacity", "[commands]")
{
    app_settings settings {};
    for (std::size_t index { 0 }; index < command_history_capacity + 5; ++index)
        record_command_use(settings, "command." + std::to_string(index));
    REQUIRE(settings.command_history.size() == command_history_capacity);
    REQUIRE(settings.command_history.front() == "command." + std::to_string(command_history_capacity + 4));
    record_command_use(settings, "");
    REQUIRE(settings.command_history.size() == command_history_capacity);
}

TEST_CASE("availability follows the repository state", "[commands]")
{
    command_availability none {};
    REQUIRE(command_available("git.fetch", none) == false);
    REQUIRE(command_available("app.settings", none));
    REQUIRE(command_available("tab.new", none));
    command_availability open {};
    open.repository = true;
    open.fetch = true;
    REQUIRE(command_available("git.fetch", open));
    REQUIRE(command_available("git.pull", open) == false);
    REQUIRE(command_available("git.branch.checkout", open));
    REQUIRE(command_available("view.drawer", open) == false);
}
