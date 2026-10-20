#pragma once

#include "domain/app_settings.h"
#include "presentation/commands/key_chord.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 명령 레지스트리다. 앱이 할 수 있는 일마다 id·한글 제목·영문 키워드·기본 단축키를 한 표에 둔다.
    // 명령 팔레트(목록·검색)와 단축키(설정 화면·키 처리)가 이 표 하나를 읽는다. 순수 데이터와 규칙이라 UI 없이 test한다.

    // 어느 입력 상황에서 단축키가 듣는가. 팔레트와 메뉴에서 실행하는 길은 상황과 무관하다.
    enum class command_scope : std::uint8_t
    {
        // 어디서나 (텍스트 칸에 초점이 있어도) 듣는다.
        global,
        // 커밋 상자(제목·본문 칸)에 초점이 있을 때만 듣는다 (Ctrl+Enter).
        commit_input,
    };

    // 설정 화면이 명령을 묶어 보이는 머리글이다.
    enum class command_group : std::uint8_t
    {
        app,
        tab,
        repository,
        git,
        view,
        profile,
        // GitHub·GitLab 연동이다.
        hosting,
    };

    struct command_definition
    {
        // `git.fetch`처럼 점으로 가른 영문 id다. 설정 파일의 키이므로 바꾸지 않는다.
        std::string id {};
        std::u8string title {};
        // 영문 검색어다. 공백으로 가른다 (한글 제목으로 찾지 못하는 사용자가 영어로 찾는다).
        std::u8string keywords {};
        command_group group { command_group::app };
        command_scope scope { command_scope::global };
        // 정규 표기(`Ctrl+Shift+P`)의 기본 단축키다. 없으면 키가 없는 명령이다.
        std::vector<std::u8string> default_shortcuts {};
        // 팔레트 목록에 보이는가. 키로만 쓰는 명령(Esc로 닫기)은 거짓이다.
        bool in_palette { true };
    };

    [[nodiscard]] const std::vector<command_definition>& command_definitions();
    [[nodiscard]] const command_definition* find_command(std::string_view id);
    [[nodiscard]] std::u8string_view command_group_title(command_group group) noexcept;

    // ---- 지금 할 수 있는 명령인가 (팔레트가 못 하는 것은 목록에서 뺀다)
    struct command_availability
    {
        // 선택된 탭이 열린 저장소이고 세션이 있다.
        bool repository { false };
        bool fetch { false };
        bool pull { false };
        bool push { false };
        bool branch { false };
        bool stash { false };
        bool pop { false };
        bool commit { false };
        // 상세 서랍 단추가 있는 폭이다 (medium·narrow).
        bool drawer { false };
        // 고른 항목의 우클릭 메뉴가 있다.
        bool selection_menu { false };
        bool profiles_exist { false };
        // 저장소의 원격이 GitHub·GitLab으로 알려진 호스트이고, 그 호스트에 로그인했으며, 현재 브랜치로 PR을 만들 수 있다.
        bool hosting_supported { false };
        bool hosting_signed_in { false };
        bool hosting_can_create { false };
    };
    [[nodiscard]] bool command_available(std::string_view id, const command_availability& availability) noexcept;

    // ---- 단축키: 기본값과 사용자가 바꾼 값을 합친다
    // 명령의 지금 단축키다. 설정에 바꾼 값이 있으면 그것(읽을 수 없는 조각은 건너뛴다)이고 없으면 기본값이다.
    [[nodiscard]] std::vector<key_chord> effective_shortcuts(const app_settings& settings, const command_definition& command);
    [[nodiscard]] std::vector<key_chord> default_shortcuts(const command_definition& command);
    [[nodiscard]] bool shortcuts_customized(const app_settings& settings, std::string_view id);

    // 키 처리가 읽는 표다. 같은 단축키를 둔 명령이 둘이면 표에서 앞에 있는 명령이 이긴다.
    struct shortcut_binding
    {
        key_chord chord {};
        std::string command {};
        command_scope scope { command_scope::global };

        [[nodiscard]] bool operator==(const shortcut_binding&) const noexcept = default;
    };
    [[nodiscard]] std::vector<shortcut_binding> resolve_shortcut_bindings(const app_settings& settings);

    // 이 단축키를 이미 쓰는 **다른** 명령의 id다. 없으면 값이 없다. `settings`는 바꾸기 전의 설정이다.
    [[nodiscard]] std::optional<std::string> find_shortcut_conflict(const app_settings& settings, std::string_view command, const key_chord& chord);

    // 편집 결과의 검사다. 비어 있으면 적용할 수 있다. 입력은 ` / `로 이은 단축키 글이다 (빈 글은 "키 없음").
    struct shortcut_edit_check
    {
        std::vector<key_chord> chords {};
        // 적용을 막는 문제다.
        std::u8string error {};
    };
    [[nodiscard]] shortcut_edit_check check_shortcut_edit(const app_settings& settings, std::string_view command, std::u8string_view text);

    // 설정을 고친다 (`app_settings_service::update` 안에서 부른다). 기본값과 같아지면 바꾼 기록을 지운다.
    void set_command_shortcuts(app_settings& settings, std::string_view command, const std::vector<key_chord>& chords);
    void reset_command_shortcuts(app_settings& settings, std::string_view command);
    void reset_all_shortcuts(app_settings& settings);

    // 툴팁이나 목록 옆에 보이는 글이다 (`Ctrl+Shift+P / Ctrl+P`). 단축키가 없으면 빈 글이다.
    [[nodiscard]] std::u8string shortcut_text(const app_settings& settings, std::string_view command);
} // namespace cyella
