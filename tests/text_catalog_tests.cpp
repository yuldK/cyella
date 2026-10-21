// 화면 문구 카탈로그의 틀 채우기(인자·조사·중괄호)와 문구 파일 읽기를 검증한다 (ADR-008).
#include "domain/text_catalog.h"
#include "infrastructure/json_text_catalog.h"
#include "platform/win32/win32_locale.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string>

using namespace cyella;

TEST_CASE("text patterns fill named arguments", "[domain][text]")
{
    const std::array arguments { text_argument { u8"name", u8"main" }, text_argument { u8"count", 3 } };
    CHECK(format_text(u8"{name}에 커밋 {count}개", arguments) == u8"main에 커밋 3개");
    CHECK(format_text(u8"{count} commits on {name}", arguments) == u8"3 commits on main");
    // 없는 인자는 틀 그대로 남아 화면에서 빠진 것이 보인다.
    CHECK(format_text(u8"{missing} 그대로", arguments) == u8"{missing} 그대로");
    CHECK(format_text(u8"{{name}} 중괄호", arguments) == u8"{name} 중괄호");
    CHECK(format_text(u8"닫히지 않은 {name", arguments) == u8"닫히지 않은 {name");
}

TEST_CASE("josa follows the final syllable", "[domain][text]")
{
    const auto particle = [](const std::u8string_view value, const std::u8string_view pattern) {
        const std::array arguments { text_argument { u8"name", value } };
        return format_text(pattern, arguments);
    };
    CHECK(particle(u8"main", u8"{name|을/를}") == u8"main을");
    CHECK(particle(u8"feature", u8"{name|을/를}") == u8"feature를");
    CHECK(particle(u8"graph", u8"{name|을/를}") == u8"graph를");
    CHECK(particle(u8"브랜치", u8"{name|을/를}") == u8"브랜치를");
    CHECK(particle(u8"한글", u8"{name|을/를}") == u8"한글을");
    CHECK(particle(u8"main", u8"{name|으로/로}") == u8"main으로");
    CHECK(particle(u8"feature/x1", u8"{name|으로/로}") == u8"feature/x1으로");
    CHECK(particle(u8"abc1234", u8"{name|으로/로}") == u8"abc1234로");
    // ㄹ 받침은 `으로`가 아니라 `로`다.
    CHECK(particle(u8"한글", u8"{name|으로/로}") == u8"한글로");
    CHECK(particle(u8"main", u8"{name|은/는}") == u8"main은");
    CHECK(particle(u8"태그", u8"{name|이/가}") == u8"태그가");
}

TEST_CASE("text placeholders list the argument names", "[domain][text]")
{
    const auto names { text_placeholders(u8"{what|을/를} {to|으로/로} reset · {{literal}}") };
    REQUIRE(names.has_value());
    CHECK(*names == std::set<std::u8string, std::less<>> { u8"to", u8"what" });
    CHECK_FALSE(text_placeholders(u8"닫히지 않은 {name").has_value());
    CHECK_FALSE(text_placeholders(u8"짝 없는 } 중괄호").has_value());
}

TEST_CASE("language preference names round-trip", "[domain][text]")
{
    for (const language_preference preference : { language_preference::system, language_preference::korean, language_preference::english })
        CHECK(parse_language_preference(language_preference_name(preference)) == preference);
    CHECK_FALSE(parse_language_preference(u8"fr").has_value());
    CHECK(resolve_ui_language(language_preference::system, ui_language::english) == ui_language::english);
    CHECK(resolve_ui_language(language_preference::korean, ui_language::english) == ui_language::korean);
    CHECK(resolve_ui_language(language_preference::english, ui_language::korean) == ui_language::english);
}

TEST_CASE("text catalog files report duplicate and non-string entries", "[infrastructure][text]")
{
    const text_catalog_parse_result parsed { parse_text_catalog_json(R"({ "a.one": "하나", "a.two": 2, "a.one": "again" })", ui_language::korean) };
    CHECK(parsed.problems.size() == 2);
    CHECK(parsed.catalog.find(u8"a.two") == nullptr);
    REQUIRE(parsed.catalog.find(u8"a.one") != nullptr);

    CHECK_FALSE(parse_text_catalog_json("[1, 2]", ui_language::english).problems.empty());
    CHECK_FALSE(parse_text_catalog_json("{ broken", ui_language::english).problems.empty());
}

TEST_CASE("lookups fall back from the chosen language to Korean and then to the key", "[domain][text]")
{
    text_catalog korean { .language = ui_language::korean, .entries = { { u8"t.both", u8"둘 다" }, { u8"t.korean_only", u8"{name|을/를} 고름" } } };
    text_catalog english { .language = ui_language::english, .entries = { { u8"t.both", u8"Both" } } };
    install_text_catalogs(korean, english);
    CHECK(active_ui_language() == ui_language::english);
    CHECK(tr(u8"t.both") == u8"Both");
    CHECK(tr(u8"t.korean_only", { { u8"name", u8"main" } }) == u8"main을 고름");
    CHECK(tr(u8"t.missing") == u8"t.missing");
    CHECK(tr_narrow(u8"t.both") == "Both");

    install_text_catalogs(korean, std::nullopt);
    CHECK(active_ui_language() == ui_language::korean);
    CHECK(tr(u8"t.both") == u8"둘 다");

    // 같은 프로세스의 다음 test가 실제 한국어 문구를 보도록 되돌린다.
    win32::install_ui_language(ui_language::korean);
}
