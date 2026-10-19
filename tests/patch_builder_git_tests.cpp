// 줄 단위 patch를 실제 git에 적용해 결과를 확인하는 통합 test다. git이 만든 diff를 해석기로 읽고,
// patch 조립기가 만든 patch를 `git apply`에 넘겨 index와 작업 트리가 기대한 내용이 되는지 본다.
// 모두 `[integration]` 태그를 가진다.
#include "domain/patch_builder.h"
#include "infrastructure/git_command_builder.h"
#include "infrastructure/git_diff_parser.h"

#include "git_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using namespace cyella;
    using cyella::test::git_run_result;
    using cyella::test::run_git;
    using strings = std::vector<std::string>;

    git_run_result run_ok(const std::filesystem::path& repo, const git_arguments& arguments, const std::string_view standard_input = {})
    {
        git_run_result result { run_git(repo, arguments, standard_input) };
        INFO("stderr: " << result.standard_error);
        INFO("stdout: " << result.standard_output);
        REQUIRE(result.exit_code == 0);
        return result;
    }

    git_run_result run_ok(const std::filesystem::path& repo, const strings& arguments, const std::string_view standard_input = {})
    {
        git_arguments converted {};
        for (const std::string& argument : arguments)
            converted.emplace_back(argument.begin(), argument.end());
        return run_ok(repo, converted, standard_input);
    }

    // 줄마다 '\n'을 붙인다.
    std::string lines_text(const std::vector<std::string>& lines)
    {
        std::string text {};
        for (const std::string& line : lines)
        {
            text += line;
            text.push_back('\n');
        }
        return text;
    }

    // 임시 저장소 하나다. 소멸할 때 지워진다.
    class repository
    {
    public:
        repository()
        {
            run_ok(directory_.path(), make_git_init_args((directory_.path() / "repo").string(), "main"));
            path_ = directory_.path() / "repo";
        }

        [[nodiscard]] const std::filesystem::path& path() const noexcept
        {
            return path_;
        }

        void write(const std::string_view relative, const std::string_view content) const
        {
            cyella::test::write_file(path_ / std::filesystem::path { std::u8string { relative.begin(), relative.end() } }, content);
        }

        [[nodiscard]] std::string read(const std::string_view relative) const
        {
            return cyella::test::read_file(path_ / std::filesystem::path { std::u8string { relative.begin(), relative.end() } });
        }

        void commit_all(const std::string_view message) const
        {
            run_ok(path_, strings { "add", "-A" });
            run_ok(path_, make_git_commit_args(), std::string { message } + "\n");
        }

        void git(const strings& arguments) const
        {
            run_ok(path_, arguments);
        }

        [[nodiscard]] std::string index_content(const std::string& relative) const
        {
            return run_ok(path_, strings { "show", ":" + relative }).standard_output;
        }

        [[nodiscard]] bool index_has(const std::string& relative) const
        {
            return run_ok(path_, strings { "ls-files", "--cached", "--", relative }).standard_output.empty() == false;
        }

        // 지정한 방식의 diff에서 파일 하나를 읽는다.
        [[nodiscard]] diff_file diff(const std::string& relative, const diff_source source, const std::uint32_t context = 3) const
        {
            const std::vector<diff_file> files { parse_git_diff(run_ok(path_, make_git_diff_args(relative, source, context)).standard_output) };
            REQUIRE(files.size() == 1);
            return files.front();
        }

        [[nodiscard]] diff_file untracked_diff(const std::string& relative, const std::uint32_t context = 3) const
        {
            const git_run_result result { run_git(path_, make_git_diff_untracked_args(relative, context)) };
            REQUIRE((result.exit_code == 0 || result.exit_code == 1));
            const std::vector<diff_file> files { parse_git_diff(result.standard_output) };
            REQUIRE(files.size() == 1);
            return files.front();
        }

        // patch를 `git apply`에 넘긴다. 줄 수가 틀리면 git이 거절하도록 --recount는 끈다.
        git_run_result apply(const std::string& patch, const patch_direction direction, const bool recount = false) const
        {
            const patch_apply_mode mode { patch_apply_mode_for(direction) };
            git_apply_options options {};
            options.cached = mode.cached;
            options.reverse = mode.reverse;
            options.recount = recount;
            return run_ok(path_, make_git_apply_args(options), patch);
        }

    private:
        cyella::test::temp_directory directory_ {};
        std::filesystem::path path_ {};
    };

    // hunk 하나가 파일 전체를 덮을 때, 고른 줄만 되돌리거나 올린 결과 파일을 git과 무관하게 계산한다.
    // 이 함수가 "고른 변경만 일어난 파일"의 정의다.
    std::string expected_content(const diff_hunk& hunk, const std::set<std::size_t>& selected, const patch_direction direction)
    {
        std::string text {};
        for (std::size_t index { 0 }; index < hunk.lines.size(); ++index)
        {
            const diff_line& line { hunk.lines[index] };
            const bool chosen { selected.contains(index) };
            bool keep { false };
            switch (line.kind)
            {
            case diff_line_kind::context:
                keep = true;
                break;
            case diff_line_kind::add:
                // stage는 고른 add만 들어온다. 되돌릴 때는 고르지 않은 add가 남는다.
                keep = direction == patch_direction::stage ? chosen : (chosen == false);
                break;
            case diff_line_kind::del:
                // stage는 고르지 않은 del이 남는다. 되돌릴 때는 고른 del이 되살아난다.
                keep = direction == patch_direction::stage ? (chosen == false) : chosen;
                break;
            case diff_line_kind::no_newline:
                break;
            }
            if (keep && line.kind != diff_line_kind::no_newline)
            {
                text += line.content;
                text.push_back('\n');
            }
        }
        return text;
    }

    const std::vector<std::string> old_lines { "a1", "a2", "a3", "a4", "a5", "a6", "a7", "a8", "a9", "a10", "a11", "a12", "a13", "a14" };
    // a2 수정, a5 삭제, a8 뒤에 두 줄 삽입, a11 수정. hunk 하나로 이어지는 간격이다.
    const std::vector<std::string> new_lines { "a1", "A2", "a3", "a4", "a6", "a7", "a8", "N1", "N2", "a9", "a10", "A11", "a12", "a13", "a14" };

    // 선택 집합을 hunk의 add·del 위치 목록에서 뽑는다. 비트가 켜진 순서의 변경 줄을 고른다.
    std::vector<std::set<std::size_t>> sample_selections(const std::vector<std::size_t>& changes)
    {
        std::vector<std::set<std::size_t>> selections {};
        const std::size_t count { changes.size() };
        // 하나씩, 하나만 빼고 전부, 앞 절반, 뒤 절반, 홀수 번째, 짝수 번째, 전부.
        for (std::size_t index { 0 }; index < count; ++index)
        {
            selections.push_back({ changes[index] });
            std::set<std::size_t> all_but_one { changes.begin(), changes.end() };
            all_but_one.erase(changes[index]);
            selections.push_back(std::move(all_but_one));
        }
        selections.push_back({ changes.begin(), changes.begin() + static_cast<std::ptrdiff_t>(count / 2) });
        selections.push_back({ changes.begin() + static_cast<std::ptrdiff_t>(count / 2), changes.end() });
        std::set<std::size_t> odd {};
        std::set<std::size_t> even {};
        for (std::size_t index { 0 }; index < count; ++index)
            (index % 2 == 0 ? even : odd).insert(changes[index]);
        selections.push_back(std::move(odd));
        selections.push_back(std::move(even));
        selections.push_back({ changes.begin(), changes.end() });
        return selections;
    }
} // namespace

TEST_CASE("integration: every partial selection stages exactly the chosen lines", "[git][integration][patch]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    const repository repo {};
    repo.write("f.txt", lines_text(old_lines));
    repo.commit_all("base");
    repo.write("f.txt", lines_text(new_lines));

    // 파일 전체가 hunk 하나가 되도록 문맥을 넉넉히 준다.
    const diff_file file { repo.diff("f.txt", diff_source::unstaged, 100) };
    REQUIRE(file.hunks.size() == 1);
    const std::vector<std::size_t> changes { changed_line_indices(file.hunks[0]) };
    REQUIRE(changes.size() == 7);

    for (const std::set<std::size_t>& selected : sample_selections(changes))
    {
        const std::vector<std::size_t> chosen { selected.begin(), selected.end() };
        CAPTURE(chosen);
        const std::optional<std::string> patch { build_line_patch(file, 0, chosen, patch_direction::stage) };
        REQUIRE(patch.has_value());
        CAPTURE(*patch);
        repo.apply(*patch, patch_direction::stage);
        CHECK(repo.index_content("f.txt") == expected_content(file.hunks[0], selected, patch_direction::stage));
        // 작업 트리는 건드리지 않는다.
        CHECK(repo.read("f.txt") == lines_text(new_lines));
        repo.git({ "reset", "-q" });
    }
}

TEST_CASE("integration: every partial selection unstages exactly the chosen lines", "[git][integration][patch]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    const repository repo {};
    repo.write("f.txt", lines_text(old_lines));
    repo.commit_all("base");
    repo.write("f.txt", lines_text(new_lines));
    repo.git({ "add", "f.txt" });

    const diff_file file { repo.diff("f.txt", diff_source::staged, 100) };
    REQUIRE(file.hunks.size() == 1);
    const std::vector<std::size_t> changes { changed_line_indices(file.hunks[0]) };

    for (const std::set<std::size_t>& selected : sample_selections(changes))
    {
        const std::vector<std::size_t> chosen { selected.begin(), selected.end() };
        CAPTURE(chosen);
        const std::optional<std::string> patch { build_line_patch(file, 0, chosen, patch_direction::unstage) };
        REQUIRE(patch.has_value());
        CAPTURE(*patch);
        repo.apply(*patch, patch_direction::unstage);
        CHECK(repo.index_content("f.txt") == expected_content(file.hunks[0], selected, patch_direction::unstage));
        CHECK(repo.read("f.txt") == lines_text(new_lines));
        repo.git({ "add", "f.txt" });
    }
}

TEST_CASE("integration: every partial selection discards exactly the chosen lines from the worktree", "[git][integration][patch]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    const repository repo {};
    repo.write("f.txt", lines_text(old_lines));
    repo.commit_all("base");
    repo.write("f.txt", lines_text(new_lines));

    const diff_file file { repo.diff("f.txt", diff_source::unstaged, 100) };
    REQUIRE(file.hunks.size() == 1);
    const std::vector<std::size_t> changes { changed_line_indices(file.hunks[0]) };

    for (const std::set<std::size_t>& selected : sample_selections(changes))
    {
        const std::vector<std::size_t> chosen { selected.begin(), selected.end() };
        CAPTURE(chosen);
        const std::optional<std::string> patch { build_line_patch(file, 0, chosen, patch_direction::discard) };
        REQUIRE(patch.has_value());
        CAPTURE(*patch);
        repo.apply(*patch, patch_direction::discard);
        CHECK(repo.read("f.txt") == expected_content(file.hunks[0], selected, patch_direction::discard));
        // index는 건드리지 않는다.
        CHECK(repo.index_content("f.txt") == lines_text(old_lines));
        repo.write("f.txt", lines_text(new_lines));
    }
}

TEST_CASE("integration: whole-hunk patches match stage, unstage and discard of the whole file", "[git][integration][patch]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    const repository repo {};
    repo.write("f.txt", lines_text(old_lines));
    repo.commit_all("base");
    repo.write("f.txt", lines_text(new_lines));

    SECTION("stage hunk")
    {
        const std::optional<std::string> patch { build_hunk_patch(repo.diff("f.txt", diff_source::unstaged), 0, patch_direction::stage) };
        REQUIRE(patch.has_value());
        repo.apply(*patch, patch_direction::stage, true);
        CHECK(repo.index_content("f.txt") == lines_text(new_lines));
    }

    SECTION("unstage hunk")
    {
        repo.git({ "add", "f.txt" });
        const std::optional<std::string> patch { build_hunk_patch(repo.diff("f.txt", diff_source::staged), 0, patch_direction::unstage) };
        REQUIRE(patch.has_value());
        repo.apply(*patch, patch_direction::unstage, true);
        CHECK(repo.index_content("f.txt") == lines_text(old_lines));
    }

    SECTION("discard hunk")
    {
        const std::optional<std::string> patch { build_hunk_patch(repo.diff("f.txt", diff_source::unstaged), 0, patch_direction::discard) };
        REQUIRE(patch.has_value());
        repo.apply(*patch, patch_direction::discard, true);
        CHECK(repo.read("f.txt") == lines_text(old_lines));
    }
}

TEST_CASE("integration: one hunk of a multi-hunk file moves independently", "[git][integration][patch]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    std::vector<std::string> before {};
    for (int line { 1 }; line <= 40; ++line)
        before.push_back("line" + std::to_string(line));
    std::vector<std::string> after { before };
    after[2] = "THREE";
    after.insert(after.begin() + 20, "inserted");
    // 삽입으로 한 칸 밀려 있으므로 원래 35번째 줄이다.
    after[35] = "THIRTY-FIVE";

    const repository repo {};
    repo.write("m.txt", lines_text(before));
    repo.commit_all("base");
    repo.write("m.txt", lines_text(after));

    const diff_file file { repo.diff("m.txt", diff_source::unstaged) };
    REQUIRE(file.hunks.size() == 3);

    SECTION("stage the middle hunk only")
    {
        const std::optional<std::string> patch { build_hunk_patch(file, 1, patch_direction::stage) };
        REQUIRE(patch.has_value());
        repo.apply(*patch, patch_direction::stage);
        std::vector<std::string> expected { before };
        expected.insert(expected.begin() + 20, "inserted");
        CHECK(repo.index_content("m.txt") == lines_text(expected));

        // 이어서 마지막 hunk의 삭제 줄 하나만 stage한다. 앞 hunk가 줄 수를 바꾼 뒤라도 새 diff로 만든 patch는 맞는다.
        const diff_file again { repo.diff("m.txt", diff_source::unstaged) };
        REQUIRE(again.hunks.size() == 2);
        const std::vector<std::size_t> changes { changed_line_indices(again.hunks[1]) };
        REQUIRE(changes.size() == 2);
        const std::optional<std::string> second { build_line_patch(again, 1, { changes[0] }, patch_direction::stage) };
        REQUIRE(second.has_value());
        repo.apply(*second, patch_direction::stage);
        expected.erase(expected.begin() + 35);
        CHECK(repo.index_content("m.txt") == lines_text(expected));
    }

    SECTION("discard the last hunk only")
    {
        const std::optional<std::string> patch { build_hunk_patch(file, 2, patch_direction::discard) };
        REQUIRE(patch.has_value());
        repo.apply(*patch, patch_direction::discard);
        std::vector<std::string> expected { after };
        expected[35] = "line35";
        CHECK(repo.read("m.txt") == lines_text(expected));
    }
}

TEST_CASE("integration: untracked files stage partially and unstage back", "[git][integration][patch]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    const repository repo {};
    repo.write("base.txt", "base\n");
    repo.commit_all("base");
    repo.write("new.txt", "one\ntwo\nthree\nfour\n");

    SECTION("stage two of four lines")
    {
        const diff_file file { repo.untracked_diff("new.txt") };
        REQUIRE(file.status == 'A');
        const std::optional<std::string> patch { build_line_patch(file, 0, { 1, 3 }, patch_direction::stage) };
        REQUIRE(patch.has_value());
        repo.apply(*patch, patch_direction::stage);
        CHECK(repo.index_content("new.txt") == "two\nfour\n");
        CHECK(repo.read("new.txt") == "one\ntwo\nthree\nfour\n");

        // 이어서 staged diff에서 한 줄만 내린다. 파일은 남는다.
        const diff_file staged { repo.diff("new.txt", diff_source::staged) };
        const std::optional<std::string> back { build_line_patch(staged, 0, { 0 }, patch_direction::unstage) };
        REQUIRE(back.has_value());
        repo.apply(*back, patch_direction::unstage);
        CHECK(repo.index_content("new.txt") == "four\n");
    }

    SECTION("unstaging every line of a staged new file removes it from the index")
    {
        repo.git({ "add", "new.txt" });
        const diff_file staged { repo.diff("new.txt", diff_source::staged) };
        const std::optional<std::string> patch { build_hunk_patch(staged, 0, patch_direction::unstage) };
        REQUIRE(patch.has_value());
        repo.apply(*patch, patch_direction::unstage);
        CHECK_FALSE(repo.index_has("new.txt"));
        CHECK(repo.read("new.txt") == "one\ntwo\nthree\nfour\n");
    }

    SECTION("unstaging some lines of a staged new file keeps the others")
    {
        repo.git({ "add", "new.txt" });
        const diff_file staged { repo.diff("new.txt", diff_source::staged) };
        const std::optional<std::string> patch { build_line_patch(staged, 0, { 0, 1 }, patch_direction::unstage) };
        REQUIRE(patch.has_value());
        repo.apply(*patch, patch_direction::unstage);
        CHECK(repo.index_content("new.txt") == "three\nfour\n");
    }
}

TEST_CASE("integration: deleted files stage and restore partially", "[git][integration][patch]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    const repository repo {};
    repo.write("d.txt", "one\ntwo\nthree\nfour\n");
    repo.commit_all("base");
    std::filesystem::remove(repo.path() / "d.txt");

    SECTION("staging some deletions leaves the file in the index with the rest")
    {
        const diff_file file { repo.diff("d.txt", diff_source::unstaged) };
        REQUIRE(file.status == 'D');
        const std::optional<std::string> patch { build_line_patch(file, 0, { 0, 1 }, patch_direction::stage) };
        REQUIRE(patch.has_value());
        repo.apply(*patch, patch_direction::stage);
        CHECK(repo.index_content("d.txt") == "three\nfour\n");
    }

    SECTION("staging every deletion removes the file from the index")
    {
        const diff_file file { repo.diff("d.txt", diff_source::unstaged) };
        const std::optional<std::string> patch { build_hunk_patch(file, 0, patch_direction::stage) };
        REQUIRE(patch.has_value());
        repo.apply(*patch, patch_direction::stage);
        CHECK_FALSE(repo.index_has("d.txt"));
    }

    SECTION("unstaging some deletions of a staged removal restores just those lines")
    {
        repo.git({ "add", "d.txt" });
        const diff_file file { repo.diff("d.txt", diff_source::staged) };
        REQUIRE(file.status == 'D');
        const std::optional<std::string> patch { build_line_patch(file, 0, { 1, 3 }, patch_direction::unstage) };
        REQUIRE(patch.has_value());
        repo.apply(*patch, patch_direction::unstage);
        CHECK(repo.index_content("d.txt") == "two\nfour\n");
    }

    SECTION("discarding some deletions recreates the file with just those lines")
    {
        const diff_file file { repo.diff("d.txt", diff_source::unstaged) };
        const std::optional<std::string> patch { build_line_patch(file, 0, { 0, 2 }, patch_direction::discard) };
        REQUIRE(patch.has_value());
        repo.apply(*patch, patch_direction::discard);
        CHECK(repo.read("d.txt") == "one\nthree\n");
    }
}

TEST_CASE("integration: a missing newline at the end of the file survives partial staging", "[git][integration][patch]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    const repository repo {};

    SECTION("a line appended to a file that lacked a final newline")
    {
        // 옛 파일은 줄바꿈 없이 끝나고, 새 파일은 줄 하나가 늘어나며 줄바꿈으로 끝난다.
        repo.write("e.txt", "a\nb");
        repo.commit_all("base");
        repo.write("e.txt", "a\nb\nc\n");
        const diff_file file { repo.diff("e.txt", diff_source::unstaged) };
        REQUIRE(file.hunks.size() == 1);
        // a, -b, 표지, +b, +c. 마지막 add만 골라도 줄바꿈이 바뀐 b의 짝이 함께 올라간다.
        REQUIRE(file.hunks[0].lines.size() == 5);
        const std::optional<std::string> patch { build_line_patch(file, 0, { 4 }, patch_direction::stage) };
        REQUIRE(patch.has_value());
        repo.apply(*patch, patch_direction::stage);
        CHECK(repo.index_content("e.txt") == "a\nb\nc\n");
    }

    SECTION("only the new last line without a newline is staged")
    {
        repo.write("e.txt", "a\nb\n");
        repo.commit_all("base");
        repo.write("e.txt", "a\nb\nc");
        const diff_file file { repo.diff("e.txt", diff_source::unstaged) };
        REQUIRE(file.hunks.size() == 1);
        const std::vector<std::size_t> changes { changed_line_indices(file.hunks[0]) };
        REQUIRE_FALSE(changes.empty());
        const std::optional<std::string> patch { build_line_patch(file, 0, changes, patch_direction::stage) };
        REQUIRE(patch.has_value());
        repo.apply(*patch, patch_direction::stage);
        CHECK(repo.index_content("e.txt") == "a\nb\nc");
    }

    SECTION("discarding the unselected part of an edit to a last line without a newline")
    {
        repo.write("e.txt", "a\nb\nc");
        repo.commit_all("base");
        repo.write("e.txt", "A\nb\nC");
        const diff_file file { repo.diff("e.txt", diff_source::unstaged) };
        REQUIRE(file.hunks.size() == 1);
        // 줄바꿈 없는 마지막 줄의 수정만 되돌린다. 앞 줄의 수정은 남는다.
        const std::vector<std::size_t> changes { changed_line_indices(file.hunks[0]) };
        REQUIRE(changes.size() == 4);
        const std::optional<std::string> patch { build_line_patch(file, 0, { changes[2], changes[3] }, patch_direction::discard) };
        REQUIRE(patch.has_value());
        repo.apply(*patch, patch_direction::discard);
        CHECK(repo.read("e.txt") == "A\nb\nc");
    }
}

TEST_CASE("integration: CRLF files keep their carriage returns", "[git][integration][patch]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    const repository repo {};
    repo.write("w.txt", "one\r\ntwo\r\nthree\r\nfour\r\nfive\r\n");
    repo.commit_all("base");
    repo.write("w.txt", "one\r\nTWO\r\nthree\r\nFOUR\r\nfive\r\n");

    const diff_file file { repo.diff("w.txt", diff_source::unstaged) };
    REQUIRE(file.hunks.size() == 1);
    const std::vector<std::size_t> changes { changed_line_indices(file.hunks[0]) };
    REQUIRE(changes.size() == 4);
    // TWO만 올린다.
    const std::optional<std::string> patch { build_line_patch(file, 0, { changes[0], changes[1] }, patch_direction::stage) };
    REQUIRE(patch.has_value());
    repo.apply(*patch, patch_direction::stage);
    CHECK(repo.index_content("w.txt") == "one\r\nTWO\r\nthree\r\nfour\r\nfive\r\n");
}

TEST_CASE("integration: paths with spaces and Hangul are patched", "[git][integration][patch]")
{
    if (cyella::test::git_is_available() == false)
        SKIP("git is not installed");

    const std::string name { "dir with space/\xED\x95\x9C\xEA\xB8\x80 file.txt" };
    const repository repo {};
    repo.write(name, "x1\nx2\nx3\n");
    repo.commit_all("base");
    repo.write(name, "x1\nX2\nx3\nx4\n");

    const diff_file file { repo.diff(name, diff_source::unstaged) };
    REQUIRE(file.hunks.size() == 1);
    const std::vector<std::size_t> changes { changed_line_indices(file.hunks[0]) };
    REQUIRE(changes.size() == 3);

    // X2의 교체만 올리고 x4 추가는 남긴다.
    const std::optional<std::string> patch { build_line_patch(file, 0, { changes[0], changes[1] }, patch_direction::stage) };
    REQUIRE(patch.has_value());
    repo.apply(*patch, patch_direction::stage);
    CHECK(repo.index_content(name) == "x1\nX2\nx3\n");
}
