// 인터랙티브 rebase의 순수 규칙(할 일 검사·git todo 형식·자동 squash·메시지 짝)과 실행기 쪽 명령줄·편집기 명령을
// 검증한다. git과 디스크가 필요 없다.
#include "domain/interactive_rebase.h"
#include "infrastructure/git_interactive_rebase.h"
#include "infrastructure/git_progress_parser.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

namespace {
    using namespace cyella;

    // 40자 SHA를 만든다. 앞 글자가 달라 줄인 SHA끼리 겹치지 않는다.
    std::string sha_of(const char letter)
    {
        return std::string(40, letter);
    }

    rebase_commit commit_of(const char letter, std::string subject, std::string body = {})
    {
        rebase_commit commit {};
        commit.sha = sha_of(letter);
        commit.subject = std::move(subject);
        commit.body = std::move(body);
        return commit;
    }

    std::vector<std::string> shas(const std::vector<rebase_commit>& commits)
    {
        std::vector<std::string> result {};
        for (const rebase_commit& commit : commits)
            result.push_back(commit.sha);
        return result;
    }

    std::vector<std::string> todo_shas(const std::vector<rebase_todo_entry>& todo)
    {
        std::vector<std::string> result {};
        for (const rebase_todo_entry& entry : todo)
            result.push_back(entry.sha);
        return result;
    }

    std::vector<rebase_action> todo_actions(const std::vector<rebase_todo_entry>& todo)
    {
        std::vector<rebase_action> result {};
        for (const rebase_todo_entry& entry : todo)
            result.push_back(entry.action);
        return result;
    }

    std::vector<std::string> text_of(const git_arguments& arguments)
    {
        std::vector<std::string> result {};
        for (const std::u8string& argument : arguments)
            result.emplace_back(argument.begin(), argument.end());
        return result;
    }

    const std::vector<std::string> range { sha_of('a'), sha_of('b'), sha_of('c') };
} // namespace

TEST_CASE("rebase todo: serialization is one `<command> <full sha>` line per entry with LF endings", "[rebase][todo]")
{
    const std::vector<rebase_todo_entry> todo {
        { rebase_action::pick, sha_of('a'), std::nullopt },
        { rebase_action::reword, sha_of('b'), std::string { "new" } },
        { rebase_action::squash, sha_of('c'), std::nullopt },
        { rebase_action::fixup, sha_of('d'), std::nullopt },
        { rebase_action::fixup_keep_message, sha_of('e'), std::nullopt },
        { rebase_action::drop, sha_of('f'), std::nullopt },
    };
    std::string expected { "pick " + sha_of('a') + "\nreword " + sha_of('b') + "\nsquash " + sha_of('c') + "\n" };
    expected += "fixup " + sha_of('d') + "\nfixup -C " + sha_of('e') + "\ndrop " + sha_of('f') + "\n";
    CHECK(serialize_rebase_todo(todo) == expected);
    // git이 다시 읽는 형식과 같다: 우리가 쓴 줄을 todo 해석기가 그대로 되읽는다.
    CHECK(rebase_todo_commits(serialize_rebase_todo(todo)) == todo_shas(todo));
    CHECK(serialize_rebase_todo({}).empty());
}

TEST_CASE("rebase todo: validation rejects empty, foreign, duplicate, missing and leading meld entries", "[rebase][todo]")
{
    const auto problem_of = [](const std::vector<rebase_todo_entry>& todo) {
        const auto issue { find_rebase_todo_problem(todo, range) };
        return issue.has_value() ? std::optional<rebase_todo_problem> { issue->problem } : std::nullopt;
    };
    const std::vector<rebase_todo_entry> valid {
        { rebase_action::pick, sha_of('a'), std::nullopt },
        { rebase_action::fixup, sha_of('b'), std::nullopt },
        { rebase_action::drop, sha_of('c'), std::nullopt },
    };
    CHECK(problem_of(valid) == std::nullopt);
    CHECK(problem_of({}) == rebase_todo_problem::empty);
    CHECK(problem_of({ { rebase_action::pick, sha_of('a') }, { rebase_action::pick, sha_of('b') }, { rebase_action::pick, sha_of('z') } }) == rebase_todo_problem::unknown_commit);
    CHECK(problem_of({ { rebase_action::pick, sha_of('a') }, { rebase_action::pick, sha_of('a') }, { rebase_action::pick, sha_of('c') } }) == rebase_todo_problem::duplicate_commit);
    CHECK(problem_of({ { rebase_action::pick, sha_of('a') }, { rebase_action::pick, sha_of('b') } }) == rebase_todo_problem::missing_commit);
    CHECK(problem_of({ { rebase_action::squash, sha_of('a') }, { rebase_action::pick, sha_of('b') }, { rebase_action::pick, sha_of('c') } }) == rebase_todo_problem::starts_with_meld);
    // drop은 합칠 앞 커밋이 되지 못한다.
    CHECK(problem_of({ { rebase_action::drop, sha_of('a') }, { rebase_action::fixup, sha_of('b') }, { rebase_action::pick, sha_of('c') } }) == rebase_todo_problem::starts_with_meld);
    // 모두 지우는 것은 할 수 있다 (브랜치를 onto로 옮긴다).
    CHECK(problem_of({ { rebase_action::drop, sha_of('a') }, { rebase_action::drop, sha_of('b') }, { rebase_action::drop, sha_of('c') } }) == std::nullopt);

    const auto issue { find_rebase_todo_problem(std::vector<rebase_todo_entry> { { rebase_action::pick, sha_of('a') }, { rebase_action::pick, sha_of('b') } }, range) };
    REQUIRE(issue.has_value());
    CHECK(issue->sha == sha_of('c'));
    CHECK(rebase_todo_problem_text(*issue).find(u8"cccccc") != std::u8string::npos);
}

TEST_CASE("rebase todo: messages are only accepted on reword and on the last line of a squash group", "[rebase][todo][messages]")
{
    const auto problem_of = [](const std::vector<rebase_todo_entry>& todo) {
        const auto issue { find_rebase_todo_problem(todo, range) };
        return issue.has_value() ? std::optional<rebase_todo_problem> { issue->problem } : std::nullopt;
    };
    CHECK(problem_of({ { rebase_action::reword, sha_of('a') }, { rebase_action::pick, sha_of('b') }, { rebase_action::pick, sha_of('c') } }) == rebase_todo_problem::reword_without_message);
    CHECK(problem_of({ { rebase_action::reword, sha_of('a'), std::string { "# only a comment\n\n" } }, { rebase_action::pick, sha_of('b') }, { rebase_action::pick, sha_of('c') } })
        == rebase_todo_problem::empty_message);
    CHECK(problem_of({ { rebase_action::pick, sha_of('a'), std::string { "x" } }, { rebase_action::pick, sha_of('b') }, { rebase_action::pick, sha_of('c') } })
        == rebase_todo_problem::message_not_allowed);
    // squash 묶음의 가운데 줄에는 둘 수 없고 마지막 줄에는 둘 수 있다. 끝이 fixup이어도 묶음에 squash가 있으면 된다.
    CHECK(problem_of({ { rebase_action::pick, sha_of('a') }, { rebase_action::squash, sha_of('b'), std::string { "x" } }, { rebase_action::fixup, sha_of('c') } })
        == rebase_todo_problem::message_not_allowed);
    CHECK(problem_of({ { rebase_action::pick, sha_of('a') }, { rebase_action::squash, sha_of('b') }, { rebase_action::fixup, sha_of('c'), std::string { "combined" } } }) == std::nullopt);
    // fixup만 있는 묶음은 git이 편집기를 열지 않으므로 메시지를 받을 수 없다.
    CHECK(problem_of({ { rebase_action::pick, sha_of('a') }, { rebase_action::fixup, sha_of('b'), std::string { "x" } }, { rebase_action::pick, sha_of('c') } })
        == rebase_todo_problem::message_not_allowed);
}

TEST_CASE("rebase todo: message files map reword to its commit and a squash group to its last line", "[rebase][todo][messages]")
{
    const std::vector<rebase_todo_entry> todo {
        { rebase_action::reword, sha_of('a'), std::string { "한글 제목\r\n\r\n본문 첫 줄\r\n둘째 줄\r\n\r\n" } },
        { rebase_action::pick, sha_of('b'), std::nullopt },
        { rebase_action::squash, sha_of('c'), std::string { "합친 메시지" } },
        { rebase_action::drop, sha_of('d'), std::nullopt },
    };
    const std::vector<rebase_message_file> files { rebase_message_files(todo) };
    REQUIRE(files.size() == 2);
    CHECK(files[0].sha == sha_of('a'));
    // CRLF는 LF가 되고 끝의 빈 줄은 줄바꿈 하나가 된다.
    CHECK(files[0].message == "한글 제목\n\n본문 첫 줄\n둘째 줄\n");
    CHECK(files[0].message.find('\r') == std::string::npos);
    CHECK(files[1].sha == sha_of('c'));
    CHECK(files[1].message == "합친 메시지\n");
    CHECK(normalize_commit_message("\n\n  ").empty());
    CHECK(normalize_commit_message("a\rb") == "a\nb\n");
}

TEST_CASE("rebase todo: groups follow git's meld rules and skip dropped lines", "[rebase][todo]")
{
    const std::vector<rebase_todo_entry> todo {
        { rebase_action::pick, sha_of('a') },
        { rebase_action::drop, sha_of('b') },
        { rebase_action::squash, sha_of('c') },
        { rebase_action::reword, sha_of('d'), std::string { "x" } },
        { rebase_action::fixup, sha_of('e') },
    };
    const std::vector<rebase_group> groups { rebase_groups(todo) };
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].head == 0);
    CHECK(groups[0].last == 2);
    CHECK(groups[0].members == std::vector<std::size_t> { 0, 2 });
    CHECK(groups[0].has_squash);
    CHECK(groups[1].members == std::vector<std::size_t> { 3, 4 });
    CHECK_FALSE(groups[1].has_squash);

    const std::vector<std::string> messages { "A\n\nbody a", "B", "C", "D", "E" };
    CHECK(combined_squash_message(todo, groups[0], messages) == "A\n\nbody a\n\nC");
    // fixup의 메시지는 빠진다.
    CHECK(combined_squash_message(todo, groups[1], messages) == "D");
    const std::vector<rebase_todo_entry> amend { { rebase_action::pick, sha_of('a') }, { rebase_action::squash, sha_of('b') }, { rebase_action::fixup_keep_message, sha_of('c') } };
    // fixup -C는 그때까지의 메시지를 그 커밋의 것으로 바꾼다.
    CHECK(combined_squash_message(amend, rebase_groups(amend).front(), messages) == "C");
}

TEST_CASE("rebase todo: autosquash moves fixup!, squash! and amend! commits after their targets", "[rebase][autosquash]")
{
    const std::vector<rebase_commit> commits {
        commit_of('a', "add parser"),
        commit_of('b', "add view"),
        commit_of('c', "fixup! add parser"),
        commit_of('d', "squash! add view"),
        commit_of('e', "amend! add parser"),
        commit_of('f', "fixup! fixup! add parser"),
        // 대상을 SHA 앞부분으로 가리킨다.
        commit_of('1', "fixup! bbbbbbb"),
        // 제목의 앞부분으로 가리킨다.
        commit_of('2', "fixup! add vi"),
        // 대상이 없으면 그대로 pick이다.
        commit_of('3', "fixup! nothing like this"),
        commit_of('4', "plain"),
    };
    const std::vector<rebase_todo_entry> todo { autosquash_rebase_todo(commits) };
    CHECK(todo_shas(todo) == std::vector<std::string> { sha_of('a'), sha_of('c'), sha_of('e'), sha_of('f'), sha_of('b'), sha_of('d'), sha_of('1'), sha_of('2'), sha_of('3'), sha_of('4') });
    const std::vector<rebase_action> expected_actions {
        rebase_action::pick,
        rebase_action::fixup,
        rebase_action::fixup_keep_message,
        rebase_action::fixup,
        rebase_action::pick,
        rebase_action::squash,
        rebase_action::fixup,
        rebase_action::fixup,
        rebase_action::pick,
        rebase_action::pick,
    };
    CHECK(todo_actions(todo) == expected_actions);
    CHECK(find_rebase_todo_problem(todo, shas(commits)) == std::nullopt);

    CHECK(autosquash_marker_of("fixup! x") == autosquash_marker::fixup);
    CHECK(autosquash_marker_of("squash! x") == autosquash_marker::squash);
    CHECK(autosquash_marker_of("amend! x") == autosquash_marker::amend);
    CHECK(autosquash_marker_of("fixup!x") == autosquash_marker::none);
    // 표지가 없는 목록은 그대로다.
    const std::vector<rebase_commit> plain { commit_of('a', "one"), commit_of('b', "two") };
    CHECK(autosquash_rebase_todo(plain) == default_rebase_todo(plain));
    // 대상이 뒤에 있으면 옮기지 않는다 (git과 같다).
    const std::vector<rebase_commit> later { commit_of('a', "fixup! two"), commit_of('b', "two") };
    CHECK(todo_actions(autosquash_rebase_todo(later)) == std::vector<rebase_action> { rebase_action::pick, rebase_action::pick });
}

TEST_CASE("rebase todo: preview counts resulting commits and names their subjects", "[rebase][preview]")
{
    const std::vector<rebase_commit> commits { commit_of('a', "one"), commit_of('b', "two"), commit_of('c', "three"), commit_of('d', "four") };
    const std::vector<rebase_todo_entry> todo {
        { rebase_action::pick, sha_of('a') },
        { rebase_action::squash, sha_of('c'), std::string { "one and three\n\nbody" } },
        { rebase_action::reword, sha_of('b'), std::string { "second" } },
        { rebase_action::drop, sha_of('d') },
    };
    const rebase_preview preview { preview_rebase(todo, commits) };
    CHECK(preview.original_count == 4);
    CHECK(preview.dropped_count == 1);
    CHECK(preview.reordered);
    REQUIRE(preview.commits.size() == 2);
    CHECK(preview.commits[0].subject == "one and three");
    CHECK(preview.commits[0].combined == 2);
    CHECK(preview.commits[0].message_changed);
    CHECK(preview.commits[1].subject == "second");
    CHECK(preview.commits[1].rewritten);

    const rebase_preview untouched { preview_rebase(default_rebase_todo(commits), commits) };
    CHECK(untouched.commits.size() == 4);
    CHECK_FALSE(untouched.reordered);
    CHECK_FALSE(untouched.commits[0].rewritten);
}

TEST_CASE("rebase editor helper: git's todo and done lines are read the way git writes them", "[rebase][helper]")
{
    const auto line { parse_rebase_todo_line("pick ddd567c # c1") };
    REQUIRE(line.has_value());
    CHECK(line->command == "pick");
    CHECK(line->sha == "ddd567c");
    const auto fixup { parse_rebase_todo_line("fixup -C 0123abcd subject") };
    REQUIRE(fixup.has_value());
    CHECK(fixup->command == "fixup");
    CHECK(fixup->flag == "-C");
    CHECK(fixup->sha == "0123abcd");
    CHECK(parse_rebase_todo_line("p 0123abcd")->command == "pick");
    CHECK_FALSE(parse_rebase_todo_line("# Rebase dffb1c6..1b359c2 onto dffb1c6 (6 commands)").has_value());
    CHECK_FALSE(parse_rebase_todo_line("exec make test").has_value());
    CHECK_FALSE(parse_rebase_todo_line("label onto").has_value());
    CHECK_FALSE(parse_rebase_todo_line("pick ../../etc").has_value());

    // git 2.52가 만든 todo(주석과 도움말 포함)가 편집기에 보인 범위와 맞는지 본다.
    const std::string generated { "pick aaaaaaa # one\npick bbbbbbb # two\n\n# Rebase x..y onto x (2 commands)\n#\n# Commands:\n# p, pick <commit> = use commit\n" };
    CHECK(rebase_todo_matches(generated, std::vector<std::string> { sha_of('a'), sha_of('b') }));
    CHECK_FALSE(rebase_todo_matches(generated, std::vector<std::string> { sha_of('a') }));
    CHECK_FALSE(rebase_todo_matches(generated, std::vector<std::string> { sha_of('a'), sha_of('c') }));
    CHECK_FALSE(rebase_todo_matches("pick aaaaaaa\npick aaaaaaa\n", std::vector<std::string> { sha_of('a'), sha_of('b') }));

    // done의 마지막 줄이 지금 편집기를 연 커밋이다. pick(충돌 뒤 continue)은 메시지를 바꾸지 않는다.
    const std::string done { "pick " + sha_of('a') + "\nreword " + sha_of('b') + "\n" };
    CHECK(message_key_for_editor(done) == sha_of('b'));
    CHECK(message_key_for_editor("reword " + sha_of('b') + "\npick " + sha_of('c') + "\n") == std::nullopt);
    CHECK(message_key_for_editor("pick " + sha_of('a') + "\nsquash " + sha_of('c')) == sha_of('c'));
    CHECK(message_key_for_editor("fixup -C " + sha_of('d') + "\n") == sha_of('d'));
    CHECK(message_key_for_editor("") == std::nullopt);
    CHECK(is_hex_object_id(sha_of('a')));
    CHECK_FALSE(is_hex_object_id("abc"));
    CHECK_FALSE(is_hex_object_id("..\\x1234"));
}

TEST_CASE("rebase editor helper: the editor command is passed to git without shell surprises", "[rebase][helper]")
{
    // 특수 문자가 없으면 git이 셸 없이 바로 실행한다. 백슬래시는 셸 특수 문자라 `/`로 바꾼다.
    CHECK(make_editor_command(u8"E:\\build\\Debug\\cyella-rebase-editor.exe") == u8"E:/build/Debug/cyella-rebase-editor.exe");
    // 공백이 있으면 작은따옴표로 감싼다.
    CHECK(make_editor_command(u8"C:\\Program Files\\cyella\\cyella-rebase-editor.exe") == u8"'C:/Program Files/cyella/cyella-rebase-editor.exe'");
    // 작은따옴표는 `'\''`로 끊어 잇는다. `$`·`` ` ``는 작은따옴표 안에서 풀리지 않는다.
    CHECK(make_editor_command(u8"C:\\it's $HOME `x`\\a.exe") == u8"'C:/it'\\''s $HOME `x`/a.exe'");
    CHECK(make_editor_command(u8"D:\\사용자\\cyella\\a.exe") == u8"D:/사용자/cyella/a.exe");
}

TEST_CASE("interactive rebase commands: argument lists pin the todo-shaping options", "[rebase][commands]")
{
    CHECK(text_of(make_git_interactive_rebase_args("main", "feature", false))
        == std::vector<std::string> { "rebase", "-i", "--empty=drop", "--no-autosquash", "--no-update-refs", "--", "main", "feature" });
    CHECK(text_of(make_git_interactive_rebase_args("abc1234", "", true))
        == std::vector<std::string> { "rebase", "-i", "--empty=drop", "--no-autosquash", "--no-update-refs", "--autostash", "--", "abc1234" });
    const std::vector<std::string> range_log {
        "log",
        "--reverse",
        "--topo-order",
        "--max-parents=1",
        "--right-only",
        "--cherry-pick",
        "-z",
        "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%ct%x1f%s%x1f%b",
        "main...HEAD",
        "--",
    };
    CHECK(text_of(make_git_rebase_range_args("main", "")) == range_log);
    CHECK(text_of(make_git_rebase_merge_count_args("main", "feature")) == std::vector<std::string> { "rev-list", "--count", "--merges", "main..feature", "--" });
    CHECK(text_of(make_git_rebase_unpushed_args("main", "feature")) == std::vector<std::string> { "rev-list", "main..feature", "--not", "--remotes", "--" });
    CHECK(text_of(make_git_resolve_commit_args("main")) == std::vector<std::string> { "rev-parse", "--verify", "--quiet", "--end-of-options", "main^{commit}" });
    CHECK_THROWS(make_git_interactive_rebase_args("--exec=evil", "", false));
    CHECK_THROWS(make_git_rebase_range_args("main", "-x"));
}

TEST_CASE("interactive rebase commands: `Rebasing (k/n)` becomes progress", "[rebase][progress]")
{
    const auto progress { parse_git_progress(u8"Rebasing (2/6)\r") };
    REQUIRE(progress.has_value());
    CHECK(progress->phase_text == u8"Rebasing");
    REQUIRE(progress->fraction.has_value());
    CHECK(*progress->fraction > 0.33);
    CHECK(*progress->fraction < 0.34);
    CHECK_FALSE(parse_git_progress(u8"Rebasing (2/0)").has_value());
    CHECK_FALSE(parse_git_progress(u8"Rebasing (x/6)").has_value());
}

TEST_CASE("rebase todo: action keys follow git's one-letter commands", "[rebase][todo]")
{
    CHECK(rebase_action_for_key(U'p') == rebase_action::pick);
    CHECK(rebase_action_for_key(U'R') == rebase_action::reword);
    CHECK(rebase_action_for_key(U's') == rebase_action::squash);
    CHECK(rebase_action_for_key(U'f') == rebase_action::fixup);
    CHECK(rebase_action_for_key(U'd') == rebase_action::drop);
    CHECK(rebase_action_for_key(U'x') == std::nullopt);
    CHECK(rebase_action_word(rebase_action::fixup_keep_message) == "fixup -C");
    CHECK(is_meld_action(rebase_action::fixup_keep_message));
    CHECK_FALSE(is_meld_action(rebase_action::reword));
}
