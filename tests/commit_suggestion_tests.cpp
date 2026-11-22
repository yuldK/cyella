#include "domain/commit_suggestion.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

using namespace cyella;

namespace {
    using arguments = std::vector<std::u8string>;

    bool contains(const std::u8string& text, const std::u8string_view part)
    {
        return text.find(part) != std::u8string::npos;
    }

    std::u8string file_patch(const std::u8string_view name, const std::size_t lines)
    {
        std::u8string patch { u8"diff --git a/" };
        patch.append(name).append(u8" b/").append(name).append(u8"\n--- a/").append(name).append(u8"\n+++ b/").append(name).append(u8"\n@@ -1 +1 @@\n");
        for (std::size_t index { 0 }; index < lines; ++index)
            patch.append(u8"+0123456789abcdef\n");
        return patch;
    }
} // namespace

TEST_CASE("suggestion arguments keep an empty quoted argument", "[commit_suggestion]")
{
    // Claude 프리셋의 `--tools ""`는 "도구 없음"이다. 빈 인자가 사라지면 다음 인자가 도구 목록으로 읽힌다.
    const auto result { split_commit_suggestion_arguments(u8"-p --tools \"\" --model haiku") };
    REQUIRE(result.has_value());
    CHECK(*result == arguments { u8"-p", u8"--tools", u8"", u8"--model", u8"haiku" });
}

TEST_CASE("suggestion arguments reject an unclosed quote", "[commit_suggestion]")
{
    CHECK(split_commit_suggestion_arguments(u8"exec \"unfinished").has_value() == false);
    CHECK(split_commit_suggestion_arguments(u8"").value_or(arguments { u8"x" }).empty());
}

TEST_CASE("every preset splits into arguments", "[commit_suggestion]")
{
    REQUIRE(commit_suggestion_presets().empty() == false);
    for (const commit_suggestion_preset& preset : commit_suggestion_presets())
    {
        CHECK(preset.executable.empty() == false);
        const auto split { split_commit_suggestion_arguments(preset.arguments) };
        REQUIRE(split.has_value());
        CHECK(split->empty() == false);
    }
}

TEST_CASE("lock files and secret-looking files keep their content out of the prompt", "[commit_suggestion]")
{
    CHECK(commit_suggestion_omits_content(u8"package-lock.json"));
    CHECK(commit_suggestion_omits_content(u8"web/yarn.lock"));
    CHECK(commit_suggestion_omits_content(u8"Cargo.lock"));
    CHECK(commit_suggestion_omits_content(u8"deploy/.env"));
    CHECK(commit_suggestion_omits_content(u8".env.production"));
    CHECK(commit_suggestion_omits_content(u8"keys/server.PEM"));
    CHECK(commit_suggestion_omits_content(u8"home/.ssh/id_ed25519"));
    CHECK(commit_suggestion_omits_content(u8"config/secrets.yaml"));

    CHECK(commit_suggestion_omits_content(u8"src/main.cpp") == false);
    // 공개 키와, 이름에 낱말이 들었을 뿐인 소스는 보낸다.
    CHECK(commit_suggestion_omits_content(u8"home/.ssh/id_ed25519.pub") == false);
    CHECK(commit_suggestion_omits_content(u8"src/application/hosting_credentials.cpp") == false);
    CHECK(commit_suggestion_omits_content(u8"src/infrastructure/secret_masking.h") == false);
    CHECK(commit_suggestion_omits_content(u8"") == false);
}

TEST_CASE("a patch within the limits passes through unchanged", "[commit_suggestion]")
{
    const std::u8string patch { file_patch(u8"a.txt", 3) + file_patch(u8"b.txt", 2) };
    const commit_suggestion_patch result { shorten_commit_suggestion_patch(patch) };
    CHECK(result.text == patch);
    CHECK(result.shortened == false);
}

TEST_CASE("a long file is cut at a line boundary and later files still fit", "[commit_suggestion]")
{
    const std::u8string small { file_patch(u8"small.txt", 2) };
    const std::u8string patch { file_patch(u8"big.txt", 100) + small };
    const commit_suggestion_patch result { shorten_commit_suggestion_patch(patch, 400, 4096) };

    CHECK(result.shortened);
    CHECK(contains(result.text, u8"diff --git a/big.txt"));
    CHECK(contains(result.text, u8"was cut for length"));
    // 큰 파일이 잘려도 뒤 파일은 통째로 실린다.
    CHECK(contains(result.text, small));
    // 자른 자리는 줄 끝이다 (반쯤 잘린 줄이 없다).
    CHECK(contains(result.text, u8"+0123456789abcdef\n[..."));
}

TEST_CASE("files past the total limit are left out with a note", "[commit_suggestion]")
{
    const std::u8string patch { file_patch(u8"a.txt", 10) + file_patch(u8"b.txt", 10) + file_patch(u8"c.txt", 10) };
    const std::size_t one { file_patch(u8"a.txt", 10).size() };
    const commit_suggestion_patch result { shorten_commit_suggestion_patch(patch, 4096, one + 20) };

    CHECK(result.shortened);
    CHECK(contains(result.text, u8"diff --git a/a.txt"));
    CHECK(contains(result.text, u8"diff --git a/b.txt") == false);
    CHECK(contains(result.text, u8"remaining files were left out"));
}

TEST_CASE("a diff marker inside a line does not split the patch", "[commit_suggestion]")
{
    // 추가된 줄의 내용에 든 `diff --git `은 파일 경계가 아니다.
    const std::u8string patch { u8"diff --git a/doc.md b/doc.md\n@@ -1 +1,2 @@\n+run diff --git a/x b/x to compare\n+done\n" };
    const commit_suggestion_patch result { shorten_commit_suggestion_patch(patch) };
    CHECK(result.text == patch);
}

TEST_CASE("the prompt carries the files, the recent subjects and the diff", "[commit_suggestion]")
{
    const std::vector<std::u8string> subjects { u8"fix: 토스트가 가려지지 않는다", u8"build: 경로를 남기지 않는다" };
    const std::vector<commit_suggestion_file> files {
        { .path = u8"src/timer.cpp", .original_path = {}, .lines_added = 4, .lines_removed = 1, .is_binary = false },
        { .path = u8"assets/icon.png", .original_path = {}, .lines_added = {}, .lines_removed = {}, .is_binary = true },
        { .path = u8"src/new_name.h", .original_path = u8"src/old_name.h", .lines_added = 0, .lines_removed = 0, .is_binary = false },
        { .path = u8"package-lock.json", .original_path = {}, .lines_added = 900, .lines_removed = 850, .is_binary = false },
    };
    commit_suggestion_prompt_inputs inputs {};
    inputs.instruction = u8"제목은 한국어로\n쓴다";
    inputs.recent_subjects = subjects;
    inputs.files = files;
    inputs.patch = { .text = file_patch(u8"src/timer.cpp", 2), .shortened = false };
    const std::u8string prompt { build_commit_suggestion_prompt(inputs) };

    CHECK(contains(prompt, u8"- fix: 토스트가 가려지지 않는다\n"));
    CHECK(contains(prompt, u8"Follow the language, prefix convention and tone"));
    CHECK(contains(prompt, u8"- src/timer.cpp (+4 -1)\n"));
    CHECK(contains(prompt, u8"- assets/icon.png (binary)\n"));
    CHECK(contains(prompt, u8"- src/old_name.h -> src/new_name.h (+0 -0)\n"));
    CHECK(contains(prompt, u8"- package-lock.json (+900 -850) [content not included]\n"));
    // 추가 지시는 한 줄로 들어간다.
    CHECK(contains(prompt, u8"제목은 한국어로쓴다\n"));
    CHECK(contains(prompt, u8"Diff:\ndiff --git a/src/timer.cpp"));
    CHECK(prompt.back() == u8'\n');
}

TEST_CASE("without recent subjects the prompt names the fallback language", "[commit_suggestion]")
{
    commit_suggestion_prompt_inputs inputs {};
    inputs.language = ui_language::korean;
    CHECK(contains(build_commit_suggestion_prompt(inputs), u8"Write the message in Korean."));
    inputs.language = ui_language::english;
    const std::u8string prompt { build_commit_suggestion_prompt(inputs) };
    CHECK(contains(prompt, u8"Write the message in English."));
    CHECK(contains(prompt, u8"Additional instructions") == false);
    CHECK(contains(prompt, u8"No diff text is included."));
}

TEST_CASE("a very long file list is cut with a count of the rest", "[commit_suggestion]")
{
    std::vector<commit_suggestion_file> files {};
    for (std::size_t index { 0 }; index < commit_suggestion_file_list_limit + 3; ++index)
    {
        commit_suggestion_file file {};
        file.path = u8"assets/icon-";
        for (const char digit : std::to_string(index))
            file.path.push_back(static_cast<char8_t>(digit));
        file.path.append(u8".svg");
        files.push_back(std::move(file));
    }
    commit_suggestion_prompt_inputs inputs {};
    inputs.files = files;
    const std::u8string prompt { build_commit_suggestion_prompt(inputs) };
    CHECK(contains(prompt, u8"- assets/icon-0.svg\n"));
    CHECK(contains(prompt, u8"- assets/icon-199.svg\n"));
    CHECK_FALSE(contains(prompt, u8"assets/icon-200.svg"));
    CHECK(contains(prompt, u8"- ... and 3 more files\n"));
}

TEST_CASE("a shortened diff is announced in the prompt", "[commit_suggestion]")
{
    commit_suggestion_prompt_inputs inputs {};
    inputs.patch = { .text = u8"diff --git a/a b/a\n", .shortened = true };
    CHECK(contains(build_commit_suggestion_prompt(inputs), u8"Diff (shortened"));
}

TEST_CASE("the output splits into subject and body", "[commit_suggestion]")
{
    const suggested_commit_message message { parse_suggested_commit_message(u8"\n  fix: 간격을 최소값으로 맞춘다  \r\n\r\n- 0 이하는 무시한다\r\n- 최소 10ms\r\n\r\n") };
    CHECK(message.subject == u8"fix: 간격을 최소값으로 맞춘다");
    CHECK(message.body == u8"- 0 이하는 무시한다\n- 최소 10ms");
}

TEST_CASE("a subject alone leaves the body empty", "[commit_suggestion]")
{
    CHECK(parse_suggested_commit_message(u8"docs: README를 고친다\n") == suggested_commit_message { u8"docs: README를 고친다", {} });
    CHECK(parse_suggested_commit_message(u8"  \r\n \n").subject.empty());
    CHECK(parse_suggested_commit_message(u8"").subject.empty());
}

TEST_CASE("a code fence around the whole answer is removed", "[commit_suggestion]")
{
    const suggested_commit_message message { parse_suggested_commit_message(u8"```text\nfeat: add timer floor\n\nbody line\n```\n") };
    CHECK(message.subject == u8"feat: add timer floor");
    CHECK(message.body == u8"body line");
}

TEST_CASE("control characters are dropped and paragraphs keep one blank line", "[commit_suggestion]")
{
    const suggested_commit_message message { parse_suggested_commit_message(u8"sub\tject\x1b[0m\n\n\nfirst\x07\n\n\n\nsecond\tend\n") };
    CHECK(message.subject == u8"sub ject[0m");
    CHECK(message.body == u8"first\n\nsecond    end");
}
