// 실린 문구 파일(assets/locales/*.json)과 코드가 쓰는 키가 맞는지 검사한다 (ADR-008). 두 언어의 키 집합과 틀의
// 인자가 같아야 하고, 코드(`tr(u8"…")`·`tr_narrow(u8"…")`)와 diff 뷰어(`t('…')`)가 쓰는 키는 모두 있어야 하며, 아무도
// 쓰지 않는 키는 없어야 한다.
#include "domain/text_catalog.h"
#include "infrastructure/json_text_catalog.h"
#include "platform/win32/win32_locale.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace cyella;

namespace {
    std::string text_of(const std::u8string_view value)
    {
        return std::string { reinterpret_cast<const char*>(value.data()), value.size() };
    }

    std::string read_file(const std::filesystem::path& path)
    {
        std::ifstream stream { path, std::ios::binary };
        return std::string { std::istreambuf_iterator<char> { stream }, std::istreambuf_iterator<char> {} };
    }

    // 소스 파일에서 정규식의 첫 묶음(키)을 모두 모은다.
    void collect_keys(const std::filesystem::path& directory, const std::set<std::string>& extensions, const std::regex& pattern, std::set<std::string>& keys)
    {
        for (const auto& entry : std::filesystem::recursive_directory_iterator { directory })
        {
            if (entry.is_regular_file() == false || extensions.contains(entry.path().extension().string()) == false)
                continue;
            const std::string source { read_file(entry.path()) };
            for (auto match { std::sregex_iterator { source.begin(), source.end(), pattern } }; match != std::sregex_iterator {}; ++match)
                keys.insert((*match)[1].str());
        }
    }

    std::set<std::string> catalog_keys(const text_catalog& catalog)
    {
        std::set<std::string> keys {};
        for (const auto& [key, value] : catalog.entries)
            keys.insert(text_of(key));
        return keys;
    }

    std::string listed(const std::vector<std::string>& keys)
    {
        std::ostringstream stream {};
        for (const std::string& key : keys)
            stream << "\n  " << key;
        return stream.str();
    }

    struct loaded_catalogs
    {
        text_catalog_parse_result korean {};
        text_catalog_parse_result english {};
    };

    loaded_catalogs load()
    {
        return {
            .korean = parse_text_catalog_json(win32::locale_resource(ui_language::korean), ui_language::korean),
            .english = parse_text_catalog_json(win32::locale_resource(ui_language::english), ui_language::english),
        };
    }
} // namespace

TEST_CASE("locale files parse cleanly and share one key set", "[locale]")
{
    const loaded_catalogs catalogs { load() };
    for (const std::u8string& problem : catalogs.korean.problems)
        FAIL_CHECK("ko.json: " << text_of(problem));
    for (const std::u8string& problem : catalogs.english.problems)
        FAIL_CHECK("en.json: " << text_of(problem));
    REQUIRE_FALSE(catalogs.korean.catalog.entries.empty());

    const std::set<std::string> korean { catalog_keys(catalogs.korean.catalog) };
    const std::set<std::string> english { catalog_keys(catalogs.english.catalog) };
    std::vector<std::string> only_korean {};
    std::vector<std::string> only_english {};
    std::ranges::set_difference(korean, english, std::back_inserter(only_korean));
    std::ranges::set_difference(english, korean, std::back_inserter(only_english));
    INFO("keys only in ko.json:" << listed(only_korean));
    INFO("keys only in en.json:" << listed(only_english));
    CHECK(only_korean.empty());
    CHECK(only_english.empty());
}

TEST_CASE("translations use the same placeholders as the Korean original", "[locale]")
{
    const loaded_catalogs catalogs { load() };
    std::vector<std::string> mismatched {};
    for (const auto& [key, korean] : catalogs.korean.catalog.entries)
    {
        const auto korean_names { text_placeholders(korean) };
        if (korean_names.has_value() == false || korean.empty())
        {
            mismatched.push_back(text_of(key) + " (ko)");
            continue;
        }
        const std::u8string* const english { catalogs.english.catalog.find(key) };
        if (english == nullptr)
            continue;
        const auto english_names { text_placeholders(*english) };
        if (english_names != korean_names || english->empty())
            mismatched.push_back(text_of(key));
    }
    INFO("keys whose placeholders differ or do not parse:" << listed(mismatched));
    CHECK(mismatched.empty());
}

TEST_CASE("every key the code uses exists and every key is used", "[locale]")
{
    const std::filesystem::path root { CYELLA_PROJECT_DIRECTORY };
    std::set<std::string> used {};
    collect_keys(root / "src", { ".cpp", ".h" }, std::regex { R"(\btr(?:_narrow)?\(\s*u8"([^"]+)\")" }, used);
    collect_keys(root / "assets" / "diff-viewer", { ".js" }, std::regex { R"(\bt\(\s*'([^']+)')" }, used);
    REQUIRE_FALSE(used.empty());

    const std::set<std::string> known { catalog_keys(load().korean.catalog) };
    std::vector<std::string> missing {};
    std::vector<std::string> unused {};
    std::ranges::set_difference(used, known, std::back_inserter(missing));
    std::ranges::set_difference(known, used, std::back_inserter(unused));
    INFO("keys used in code but missing from ko.json:" << listed(missing));
    INFO("keys in ko.json that no code uses:" << listed(unused));
    CHECK(missing.empty());
    CHECK(unused.empty());
}
