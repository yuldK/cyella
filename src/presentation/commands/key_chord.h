#pragma once

#include "luil/ui/ui_events.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 단축키 하나(수정키 조합 + 키)다. 설정 파일에는 `Ctrl+Shift+P` 같은 글로 남는다.
    struct key_chord
    {
        luil::key_code key { luil::key_code::none };
        bool control { false };
        bool alt { false };
        bool shift { false };

        [[nodiscard]] bool operator==(const key_chord&) const noexcept = default;
        [[nodiscard]] bool valid() const noexcept
        {
            return key != luil::key_code::none;
        }
    };

    // `Ctrl+Shift+P`, `Alt+Left`, `F5`, `Ctrl+,`를 읽는다. 수정키 이름은 대소문자를 가리지 않고(`ctrl`·`control`) 순서는 자유다.
    // 모르는 키 이름이면 값이 없다.
    [[nodiscard]] std::optional<key_chord> parse_key_chord(std::u8string_view text);
    // 정규 표기다. 수정키는 Ctrl → Alt → Shift 순서이고 키 이름은 첫 글자만 대문자다. 키가 없으면 빈 글이다.
    [[nodiscard]] std::u8string format_key_chord(const key_chord& chord);
    // 같은 단축키를 정규 표기로 맞춘다 (`ctrl+shift+p` → `Ctrl+Shift+P`). 읽을 수 없으면 빈 글이다.
    [[nodiscard]] std::u8string normalize_key_chord_text(std::u8string_view text);

    // 눌린 키를 단축키로 바꾼다. 수정키만 눌린 이벤트(키가 없음)는 빈 단축키다.
    [[nodiscard]] key_chord chord_of(const luil::key_pressed_event& event) noexcept;

    // 단축키로 쓸 수 있는가. 비어 있으면 쓸 수 있고 아니면 이유다.
    //  - 글자·숫자·기호는 Ctrl이나 Alt와 함께여야 한다 (글자 키는 수정키 없이는 문자 입력으로만 온다).
    //  - 수정키 없이 쓸 수 있는 것은 기능 키(F1~F12)와 Esc뿐이다.
    //  - 텍스트 칸의 편집 키(Ctrl+A·C·V·X·Y·Z)와 창을 닫는 Alt+F4는 예약이다.
    [[nodiscard]] std::u8string check_key_chord(const key_chord& chord);

    // `Ctrl+P / Ctrl+Shift+P`처럼 ` / `로 이은 목록이다. 하나라도 읽을 수 없으면 `failed`에 그 조각을 담는다.
    struct chord_list_parse
    {
        std::vector<key_chord> chords {};
        std::u8string failed {};
    };
    [[nodiscard]] chord_list_parse parse_key_chord_list(std::u8string_view text);
    [[nodiscard]] std::u8string format_key_chord_list(const std::vector<key_chord>& chords);
} // namespace cyella
