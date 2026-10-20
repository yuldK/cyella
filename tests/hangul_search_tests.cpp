#include "domain/hangul_search.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>

using namespace cyella;

namespace {
    bool matches(const std::u8string_view query, const std::u8string_view title, const std::u8string_view keywords = u8"")
    {
        return fuzzy_match_score(query, title, keywords).has_value();
    }
} // namespace

TEST_CASE("choseong of a syllable is its compatibility jamo", "[hangul]")
{
    REQUIRE(choseong_of(U'브') == U'ㅂ');
    REQUIRE(choseong_of(U'랜') == U'ㄹ');
    REQUIRE(choseong_of(U'치') == U'ㅊ');
    REQUIRE(choseong_of(U'까') == U'ㄲ');
    REQUIRE(choseong_of(U'힣') == U'ㅎ');
    REQUIRE(choseong_of(U'a').has_value() == false);
    REQUIRE(choseong_of(U'ㅂ').has_value() == false);
    REQUIRE(is_choseong_jamo(U'ㅂ'));
    // 받침으로만 쓰이는 자모는 초성이 아니다.
    REQUIRE(is_choseong_jamo(U'ㄳ') == false);
    REQUIRE(is_hangul_syllable(U'가'));
    REQUIRE(is_hangul_syllable(U'ㄱ') == false);
}

TEST_CASE("utf-8 decoding turns invalid bytes into the replacement character", "[hangul]")
{
    REQUIRE(decode_utf8(u8"a브") == std::u32string { U'a', U'브' });
    std::u8string broken { u8"a" };
    broken.push_back(static_cast<char8_t>(0xE0));
    REQUIRE(decode_utf8(broken).back() == U'�');
}

TEST_CASE("initial consonants find syllables", "[hangul]")
{
    REQUIRE(matches(u8"ㅂㄹㅊ", u8"브랜치로 이동…"));
    REQUIRE(matches(u8"ㅂㄹㅊ", u8"새 브랜치 만들기…"));
    REQUIRE(matches(u8"ㄱㅈㅇㄱ", u8"원격에서 가져오기 (Fetch)"));
    REQUIRE(matches(u8"ㅂㄹㅊ", u8"원격에서 가져오기 (Fetch)") == false);
    // 초성 일부만 건너뛰며 쳐도 된다 (부분 수열).
    REQUIRE(matches(u8"ㅂㅊ", u8"브랜치로 이동…"));
}

TEST_CASE("syllables match as substrings and mix with initial consonants", "[hangul]")
{
    REQUIRE(matches(u8"브랜치", u8"새 브랜치 만들기…"));
    REQUIRE(matches(u8"랜치", u8"브랜치로 이동…"));
    REQUIRE(matches(u8"브ㄹㅊ", u8"브랜치로 이동…"));
    REQUIRE(matches(u8"브랜ㅊ", u8"브랜치로 이동…"));
    REQUIRE(matches(u8"브랜시", u8"브랜치로 이동…") == false);
}

TEST_CASE("a syllable still being composed matches any final consonant", "[hangul]")
{
    // IME가 "브랜"을 치는 도중에 "브래"가 보인다.
    REQUIRE(matches(u8"브래", u8"브랜치로 이동…"));
    // 마지막 글자만 느슨하다. 가운데 글자는 정확해야 한다.
    REQUIRE(matches(u8"브래치", u8"브랜치로 이동…") == false);
}

TEST_CASE("latin letters ignore case and keywords are searched too", "[hangul]")
{
    REQUIRE(matches(u8"FETCH", u8"원격에서 가져오기 (Fetch)"));
    REQUIRE(matches(u8"fe", u8"원격에서 가져오기 (Fetch)"));
    REQUIRE(matches(u8"download", u8"원격에서 가져오기", u8"fetch remote download"));
    REQUIRE(matches(u8"zzz", u8"원격에서 가져오기", u8"fetch remote download") == false);
}

TEST_CASE("every token of the query must match", "[hangul]")
{
    REQUIRE(matches(u8"브랜치 이동", u8"브랜치로 이동…"));
    REQUIRE(matches(u8"브랜치 pull", u8"브랜치로 이동…") == false);
    REQUIRE(matches(u8"ㅂㄹㅊ ㅇㄷ", u8"브랜치로 이동…"));
}

TEST_CASE("an empty query matches everything with score zero", "[hangul]")
{
    REQUIRE(fuzzy_match_score(u8"", u8"브랜치", u8"") == 0);
    REQUIRE(fuzzy_match_score(u8"   ", u8"브랜치", u8"") == 0);
}

TEST_CASE("contiguous matches rank above scattered ones and prefixes above the middle", "[hangul]")
{
    const std::optional<int> prefix { fuzzy_match_score(u8"브랜치", u8"브랜치로 이동…", u8"") };
    const std::optional<int> middle { fuzzy_match_score(u8"브랜치", u8"새 브랜치 만들기…", u8"") };
    const std::optional<int> scattered { fuzzy_match_score(u8"브치", u8"브랜치로 이동…", u8"") };
    REQUIRE(prefix.has_value());
    REQUIRE(middle.has_value());
    REQUIRE(scattered.has_value());
    REQUIRE(*prefix > *middle);
    REQUIRE(*middle > *scattered);
}

TEST_CASE("a title match outranks a keyword-only match", "[hangul]")
{
    const std::optional<int> title { fuzzy_match_score(u8"fetch", u8"원격에서 가져오기 (Fetch)", u8"") };
    const std::optional<int> keyword { fuzzy_match_score(u8"fetch", u8"원격에서 가져오기", u8"fetch remote") };
    REQUIRE(title.has_value());
    REQUIRE(keyword.has_value());
    REQUIRE(*title > *keyword);
}
