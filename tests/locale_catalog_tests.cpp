// 실린 문구 파일(assets/locales/*.json)과 코드가 쓰는 키가 맞는지 검사한다 (ADR-008). 두 언어의 키 집합과 틀의
// 인자가 같아야 하고, 코드(`tr(u8"…")`·`tr_narrow(u8"…")`)와 diff 뷰어(`t('…')`)가 쓰는 키는 모두 있어야 하며, 아무도
// 쓰지 않는 키는 없어야 한다. 키는 두 파일에서 같은 사전순이고, 한국어 값은 자리표시자 뒤에 조사를 글자로 적지 않으며
// (`{name|을/를}`을 쓴다), 소스의 한글 문구와 뷰어 JS의 영어 문구는 카탈로그를 거쳐야 한다.
#include "domain/text_catalog.h"
#include "infrastructure/json_text_catalog.h"
#include "platform/win32/win32_locale.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
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

    // 파일에 적힌 순서 그대로의 키다 (map으로 읽으면 순서가 사라진다).
    std::vector<std::string> keys_in_file_order(const std::filesystem::path& path)
    {
        std::vector<std::string> keys {};
        std::istringstream lines { read_file(path) };
        const std::regex key_line { R"re(^ {4}"([^"\\]+)": )re" };
        for (std::string line; std::getline(lines, line);)
        {
            std::smatch match {};
            if (std::regex_search(line, match, key_line))
                keys.push_back(match[1].str());
        }
        return keys;
    }

    // 값에서 `{name}` 바로 뒤에 조사를 글자로 적은 자리(`{name}를`)를 모은다. `{name|을/를}` 같은 규칙과 `을(를)` 꼴은 괜찮다.
    // `{{`·`}}`는 중괄호 한 글자라 자리표시자가 아니다. 조사 뒤에 한글이 이어지면 다른 낱말의 첫 글자다(`{name}이름`).
    std::vector<std::string> literal_particles_after_placeholders(const std::u8string_view value)
    {
        // 받침에 따라 꼴이 바뀌는 조사다. 두 글자인 `으로`를 한 글자인 `로`보다 먼저 본다.
        constexpr std::u8string_view particles[] { u8"으로", u8"을", u8"를", u8"이", u8"가", u8"은", u8"는", u8"과", u8"와", u8"로" };
        std::vector<std::string> found {};
        std::size_t index { 0 };
        while (index < value.size())
        {
            const char8_t current { value[index] };
            if ((current == u8'{' || current == u8'}') && index + 1 < value.size() && value[index + 1] == current)
            {
                index += 2;
                continue;
            }
            if (current != u8'{')
            {
                ++index;
                continue;
            }
            const std::size_t close { value.find(u8'}', index) };
            if (close == std::u8string_view::npos)
                break;
            const std::size_t open { index };
            index = close + 1;
            if (value.substr(open, close - open).find(u8'|') != std::u8string_view::npos)
                continue;
            const std::u8string_view rest { value.substr(index) };
            for (const std::u8string_view particle : particles)
            {
                if (rest.starts_with(particle) == false)
                    continue;
                // UTF-8에서 한글 음절의 첫 바이트는 0xEA~0xED다.
                const std::u8string_view after { rest.substr(particle.size()) };
                if (after.empty() == false && ((after.front() >= 0xEA && after.front() <= 0xED) || after.front() == u8'('))
                    continue;
                found.push_back(text_of(value.substr(open, index - open + particle.size())));
                break;
            }
        }
        return found;
    }

    // 조사를 글자로 적어 두어야 하는 값이다. 이름이 읽히는 소리가 끝 글자 규칙(영어는 n·m·l, 숫자는 0·1·3·6·7·8)과 달라 규칙을 쓰면 오히려 틀린다.
    //  - `{kind}`는 PR이나 MR이다. 피알·엠알로 읽어 `을`·`이`가 맞는데, 규칙은 끝 글자 R을 받침 없음으로 보아 `PR를`·`PR가`를 만든다.
    //  - 서명 설정 이름(`gpg.format`, `gpg.ssh.allowedSignersFile`)은 포맷·파일로 읽어 `을`이 맞는데, 규칙은 `를`을 만든다.
    const std::set<std::string>& keys_with_literal_particles()
    {
        static const std::set<std::string> keys {
            "hosting.create_failed",
            "hosting.create_from_current_tooltip",
            "hosting.creating",
            "hosting.no_open",
            "hosting.push_then_create",
            "signing_page.summary_set",
        };
        return keys;
    }

    // 한 줄이 통째로 주석인가. 줄 끝 주석은 문자열 찾기가 `//`에서 멈추는 것으로 거른다.
    bool is_comment_line(const std::string_view line)
    {
        const std::size_t first { line.find_first_not_of(" \t") };
        if (first == std::string_view::npos)
            return false;
        const std::string_view rest { line.substr(first) };
        return rest.starts_with("//") || rest.starts_with("/*") || rest.starts_with("*");
    }

    // 줄의 `quote`로 묶인 문자열 내용을 모은다. 줄 주석(`//`)부터는 보지 않고, C++의 문자 리터럴(`'"'`)은 건너뛴다.
    std::vector<std::string> quoted_strings_in(const std::string_view line, const char quote)
    {
        std::vector<std::string> strings {};
        std::size_t index { 0 };
        while (index < line.size())
        {
            if (line.substr(index).starts_with("//"))
                break;
            if (quote == '"' && line[index] == '\'')
            {
                ++index;
                while (index < line.size() && line[index] != '\'')
                    index += line[index] == '\\' ? 2 : 1;
                ++index;
                continue;
            }
            if (line[index] != quote)
            {
                ++index;
                continue;
            }
            std::string content {};
            ++index;
            while (index < line.size() && line[index] != quote)
            {
                if (line[index] == '\\' && index + 1 < line.size())
                    content.push_back(line[index++]);
                content.push_back(line[index++]);
            }
            ++index;
            strings.push_back(std::move(content));
        }
        return strings;
    }

    // UTF-8에서 한글 음절의 첫 바이트는 0xEA~0xED다.
    bool has_hangul(const std::string_view text)
    {
        return std::ranges::any_of(text, [](const char byte) {
            const auto value { static_cast<unsigned char>(byte) };
            return value >= 0xEA && value <= 0xED;
        });
    }

    struct korean_literal
    {
        std::string file {};
        std::string text {};
    };

    // `src/`의 주석이 아닌 줄에서 한글이 든 문자열 리터럴을 모은다. 파일은 `src/` 밑 경로다.
    std::vector<korean_literal> korean_literals_in_source()
    {
        const std::filesystem::path source_root { std::filesystem::path { CYELLA_PROJECT_DIRECTORY } / "src" };
        std::vector<korean_literal> found {};
        for (const auto& entry : std::filesystem::recursive_directory_iterator { source_root })
        {
            const std::string extension { entry.path().extension().string() };
            if (entry.is_regular_file() == false || (extension != ".cpp" && extension != ".h"))
                continue;
            const std::string file { entry.path().lexically_relative(source_root).generic_string() };
            std::istringstream lines { read_file(entry.path()) };
            for (std::string line; std::getline(lines, line);)
            {
                if (is_comment_line(line))
                    continue;
                for (std::string& text : quoted_strings_in(line, '"'))
                {
                    if (has_hangul(text))
                        found.push_back({ .file = file, .text = std::move(text) });
                }
            }
        }
        return found;
    }

    struct allowed_korean_literal
    {
        std::string_view file {};
        std::u8string_view text {};
    };

    // 화면에 보이는 글이 아니라서 카탈로그에 두지 않는 한국어 리터럴이다.
    const std::vector<allowed_korean_literal>& allowed_korean_literals()
    {
        static const std::vector<allowed_korean_literal> allowed {
            // UI Automation owner 이름이다. 화면 언어를 바꿔도 요소 id가 같도록 처음의 한국어로 고정한다.
            { "presentation/commands/command_registry.cpp", u8"앱" },
            { "presentation/commands/command_registry.cpp", u8"탭" },
            { "presentation/commands/command_registry.cpp", u8"저장소" },
            { "presentation/commands/command_registry.cpp", u8"Git 작업" },
            { "presentation/commands/command_registry.cpp", u8"화면" },
            { "presentation/commands/command_registry.cpp", u8"사용자 프로필" },
            { "presentation/ui/repository_ui.cpp", u8"로컬 브랜치" },
            { "presentation/ui/repository_ui.cpp", u8"원격 브랜치" },
            { "presentation/ui/repository_ui.cpp", u8"태그" },
            { "presentation/ui/repository_ui.cpp", u8"스태시" },
            { "presentation/ui/repository_ui.cpp", u8"서브모듈" },
            { "presentation/ui/repository_ui.cpp", u8"워크트리" },
            // 설정 파일에 남는 외부 앱 이름이다.
            { "domain/external_diff_tool.cpp", u8"Windows 탐색기" },
            // 조사 규칙의 이름 자체다 (`{name|으로/로}`).
            { "domain/text_catalog.cpp", u8"으로" },
            // 저장소에 기록되는 stash 메시지다.
            { "infrastructure/git_write_operations.cpp", u8" 체크아웃 전에 보관" },
        };
        return allowed;
    }

    // 뷰어 JS에서 사람이 읽는 영어 문구를 그대로 적은 자리를 모은다. 클래스 이름과 action 이름은 소문자 한 낱말이라 걸리지 않는다.
    std::vector<std::string> hardcoded_viewer_wording(const std::string& source)
    {
        // 단추 라벨 인자가 리터럴이다(`button('…', …)`).
        static const std::regex button_literal { R"re(\bbutton\(\s*')re" };
        // 태그 사이의 글(`>inline</button>`)과 영어 문구 리터럴(`'Stage hunk'`)이다.
        static const std::regex element_text { R"re(>[^<>]*[A-Za-z]{3,}[^<>]*</)re" };
        static const std::regex english_phrase { R"re(^[A-Z][a-z]+( [A-Za-z]+)+$)re" };
        std::vector<std::string> found {};
        std::istringstream lines { source };
        std::size_t number { 0 };
        for (std::string line; std::getline(lines, line);)
        {
            ++number;
            if (is_comment_line(line))
                continue;
            if (std::regex_search(line, button_literal))
                found.push_back("line " + std::to_string(number) + ": the label of button() is a literal");
            for (const std::string& content : quoted_strings_in(line, '\''))
            {
                if (std::regex_search(content, element_text) || std::regex_match(content, english_phrase))
                    found.push_back("line " + std::to_string(number) + ": " + content);
            }
        }
        return found;
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

TEST_CASE("locale files list their keys in byte order and in the same order", "[locale]")
{
    const std::filesystem::path directory { std::filesystem::path { CYELLA_PROJECT_DIRECTORY } / "assets" / "locales" };
    const std::vector<std::string> korean { keys_in_file_order(directory / "ko.json") };
    const std::vector<std::string> english { keys_in_file_order(directory / "en.json") };
    REQUIRE_FALSE(korean.empty());
    CHECK(korean == english);

    std::vector<std::string> misplaced {};
    for (std::size_t index { 1 }; index < korean.size(); ++index)
    {
        if (korean[index - 1] >= korean[index])
            misplaced.push_back(korean[index - 1] + "  >=  " + korean[index]);
    }
    INFO("neighbours that are not in ascending order:" << listed(misplaced));
    CHECK(misplaced.empty());
}

TEST_CASE("Korean values leave the josa to the template rule instead of spelling it after a placeholder", "[locale]")
{
    const loaded_catalogs catalogs { load() };
    std::vector<std::string> spelled {};
    std::vector<std::string> stale {};
    for (const auto& [key, korean] : catalogs.korean.catalog.entries)
    {
        const bool allowed { keys_with_literal_particles().contains(text_of(key)) };
        const std::vector<std::string> found { literal_particles_after_placeholders(korean) };
        if (found.empty() == false && allowed == false)
            spelled.push_back(text_of(key) + " : " + found.front());
        if (found.empty() && allowed)
            stale.push_back(text_of(key));
    }
    INFO("values that spell a josa after a placeholder (use {name|을/를}, {name|이/가}, {name|으로/로}…):" << listed(spelled));
    INFO("allowed keys that no longer spell a josa (remove them from the list):" << listed(stale));
    CHECK(spelled.empty());
    CHECK(stale.empty());
}

TEST_CASE("the Korean josa rules in the catalog follow the final syllable of the value", "[locale]")
{
    // 예전에는 `{name}를`처럼 적어 branch main이 `main를`이 됐다.
    CHECK(tr(u8"git.merge.done", { { u8"name", u8"main" } }) == u8"main을 병합했습니다.");
    CHECK(tr(u8"git.merge.done", { { u8"name", u8"feature" } }) == u8"feature를 병합했습니다.");
    CHECK(tr(u8"git.checkout.done", { { u8"name", u8"main" } }) == u8"main으로 체크아웃했습니다.");
    // 끝 숫자 1은 일이라 ㄹ 받침이고, ㄹ 받침 뒤에는 `으로`가 아니라 `로`다.
    CHECK(tr(u8"git.checkout.done", { { u8"name", u8"feature/x1" } }) == u8"feature/x1로 체크아웃했습니다.");
    // PR 번호 12(십이)는 받침이 없고 1(일)·7(칠)은 받침이 있다.
    CHECK(tr(u8"hosting.created", { { u8"kind", u8"PR" }, { u8"number", 12 }, { u8"url", u8"U" } }) == u8"PR #12를 만들었습니다: U");
    CHECK(tr(u8"hosting.created", { { u8"kind", u8"PR" }, { u8"number", 1 }, { u8"url", u8"U" } }) == u8"PR #1을 만들었습니다: U");
    CHECK(tr(u8"hosting.created", { { u8"kind", u8"MR" }, { u8"number", 7 }, { u8"url", u8"U" } }) == u8"MR #7을 만들었습니다: U");
    // ssh는 `에이치`로 끝나 받침이 없고 ssh-keygen은 `젠`으로 끝나 받침이 있다.
    CHECK(tr(u8"ssh.tool_timed_out", { { u8"tool", u8"ssh" } }) == u8"ssh가 시간 안에 끝나지 않았습니다.");
    CHECK(tr(u8"ssh.tool_timed_out", { { u8"tool", u8"ssh-keygen" } }) == u8"ssh-keygen이 시간 안에 끝나지 않았습니다.");
    CHECK(tr(u8"ssh.tool_run_failed", { { u8"tool", u8"ssh" } }) == u8"ssh를 실행하지 못했습니다.");
    CHECK(tr(u8"signing_page.summary_disable", { { u8"name", u8"commit.gpgsign" } }) == u8"commit.gpgsign을 끕니다");
    // git 설정 이름은 규칙이 아니라 글자로 적은 `을`이 맞다 (포맷·파일로 읽는다).
    CHECK(tr(u8"signing_page.summary_set", { { u8"name", u8"gpg.format" }, { u8"value", u8"ssh" } }) == u8"gpg.format을 ssh로 정합니다");
    // 브랜치 이름만 있는 rebase 안내도 동사로 끝나는 온전한 문장이다.
    CHECK(tr(u8"dialog.conflict.rebase_detail_branch", { { u8"branch", u8"main" } }) == u8"main을 옮기는 중");
    CHECK(tr(u8"dialog.conflict.rebase_detail", { { u8"branch", u8"feature" }, { u8"onto", u8"main" } }) == u8"feature를 main 위로 옮기는 중");
}

TEST_CASE("the source keeps Korean wording in the catalog except a few fixed names", "[locale]")
{
    const std::vector<allowed_korean_literal>& allowed { allowed_korean_literals() };
    std::vector<bool> matched(allowed.size(), false);
    std::vector<std::string> hardcoded {};
    for (const korean_literal& literal : korean_literals_in_source())
    {
        const auto known { std::ranges::find_if(allowed, [&literal](const allowed_korean_literal& entry) { return entry.file == literal.file && text_of(entry.text) == literal.text; }) };
        if (known == allowed.end())
            hardcoded.push_back(literal.file + " : " + literal.text);
        else
            matched[static_cast<std::size_t>(known - allowed.begin())] = true;
    }
    std::vector<std::string> stale {};
    for (std::size_t index { 0 }; index < allowed.size(); ++index)
    {
        if (matched[index] == false)
            stale.push_back(std::string { allowed[index].file } + " : " + text_of(allowed[index].text));
    }
    INFO("Korean string literals outside the catalog (use tr(u8\"…\") and add the key to both locale files):" << listed(hardcoded));
    INFO("allowed literals that no longer exist (remove them from the list):" << listed(stale));
    CHECK(hardcoded.empty());
    CHECK(stale.empty());
}

TEST_CASE("the diff viewer takes its wording from the catalog instead of hard-coding English", "[locale]")
{
    const std::string source { read_file(std::filesystem::path { CYELLA_PROJECT_DIRECTORY } / "assets" / "diff-viewer" / "viewer.js") };
    REQUIRE_FALSE(source.empty());
    const std::vector<std::string> hardcoded { hardcoded_viewer_wording(source) };
    INFO("viewer.js wording outside t('diff_viewer.…') (add a key to both locale files):" << listed(hardcoded));
    CHECK(hardcoded.empty());
}
