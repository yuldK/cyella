#pragma once

#include <concepts>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>

namespace cyella {
    // 화면 문구의 언어다. 한국어가 원문이고 영어가 번역이다 (ADR-008).
    enum class ui_language : std::uint8_t
    {
        korean,
        english,
    };

    // 설정에 남기는 언어 선택이다. `system`은 Windows UI 언어를 따른다.
    enum class language_preference : std::uint8_t
    {
        system,
        korean,
        english,
    };

    [[nodiscard]] std::u8string_view language_preference_name(language_preference preference) noexcept;
    [[nodiscard]] std::optional<language_preference> parse_language_preference(std::u8string_view name) noexcept;
    // 선택과 Windows UI 언어로 실제로 쓸 언어를 고른다. 시스템 언어가 한국어가 아니면 영어다.
    [[nodiscard]] ui_language resolve_ui_language(language_preference preference, ui_language system_language) noexcept;

    // 문구 틀의 이름 있는 인자 하나다. 값은 UTF-8 문자열이고 정수는 십진 표기로 바꿔 담는다.
    struct text_argument
    {
        text_argument(std::u8string_view argument_name, std::u8string_view argument_value);
        text_argument(std::u8string_view argument_name, std::string_view argument_value);

        template<std::integral value_type>
            requires(std::same_as<value_type, bool> == false && std::same_as<value_type, char8_t> == false && std::same_as<value_type, char> == false)
        text_argument(const std::u8string_view argument_name, const value_type argument_value)
            : name { argument_name }
            , value { integer_text(static_cast<std::int64_t>(argument_value)) }
        {}

        std::u8string_view name {};
        std::u8string value {};

    private:
        [[nodiscard]] static std::u8string integer_text(std::int64_t number);
    };

    // 키 → 문구 틀 표 하나다 (언어 하나의 JSON 파일).
    struct text_catalog
    {
        ui_language language { ui_language::korean };
        std::map<std::u8string, std::u8string, std::less<>> entries {};

        [[nodiscard]] const std::u8string* find(std::u8string_view key) const;
    };

    // 틀의 `{name}`을 인자 값으로 바꾼다. `{name|을/를}`은 값 뒤에 받침에 맞는 조사를 붙인다 (`으로/로`는 ㄹ 받침도
    // `로`다). `{{`·`}}`는 중괄호 한 글자다. 없는 인자의 자리는 틀 그대로 남겨 빠진 것이 화면에서 보이게 한다.
    [[nodiscard]] std::u8string format_text(std::u8string_view pattern, std::span<const text_argument> arguments);
    // 틀이 쓰는 인자 이름이다 (조사 규칙의 이름도 포함). 중괄호가 닫히지 않은 틀이면 비어 있다. 두 언어의 틀이 같은
    // 인자를 쓰는지 검사하는 데 쓴다.
    [[nodiscard]] std::optional<std::set<std::u8string, std::less<>>> text_placeholders(std::u8string_view pattern);

    // 프로세스 전역 카탈로그를 정한다. `base`는 한국어 원문이고 `active`는 고른 언어다 (한국어면 비운다). 찾는 순서는
    // active → base → 키 자신이다. 화면 thread와 작업 thread가 잠금 없이 읽으므로 **작업 thread가 생기기 전에 한 번**
    // 부른다 (앱은 `wWinMain`, test는 시작 listener). 언어를 바꾸면 다시 시작할 때 적용한다.
    void install_text_catalogs(text_catalog base, std::optional<text_catalog> active);
    [[nodiscard]] ui_language active_ui_language() noexcept;

    // 키로 문구를 찾는다. 키(`menu.copy_path` 꼴)는 늘 이 호출의 첫 인자에 문자열 상수로 바로 적는다 (키 검사 test가 소스에서
    // 찾는다).
    [[nodiscard]] std::u8string tr(std::u8string_view key);
    [[nodiscard]] std::u8string tr(std::u8string_view key, std::initializer_list<text_argument> arguments);
    // 키가 `prefix`로 시작하는 문구 틀 전부다 (고른 언어 → 한국어 순으로 찾은 값, 인자는 채우지 않는다). diff 뷰어처럼
    // 문구를 스스로 채우는 페이지에 한꺼번에 넘길 때 쓴다.
    [[nodiscard]] std::map<std::u8string, std::u8string, std::less<>> text_patterns_with_prefix(std::u8string_view prefix);
    // `std::string`(UTF-8 바이트)으로 받는 자리를 위한 같은 함수다.
    [[nodiscard]] std::string tr_narrow(std::u8string_view key);
    [[nodiscard]] std::string tr_narrow(std::u8string_view key, std::initializer_list<text_argument> arguments);
} // namespace cyella
