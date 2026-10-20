#include "domain/app_settings.h"
#include "domain/git_identity.h"
#include "domain/user_profile.h"
#include "infrastructure/json_app_settings_store.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace cyella;

namespace {
    user_profile make_profile(std::u8string name, std::u8string user_name, std::u8string email)
    {
        return user_profile { .name = std::move(name), .user_name = std::move(user_name), .user_email = std::move(email) };
    }

    std::vector<user_profile> two_profiles()
    {
        return { make_profile(u8"회사", u8"홍길동", u8"hong@corp.example"), make_profile(u8"개인", u8"길동", u8"me@example.com") };
    }
} // namespace

TEST_CASE("a rule matches paths under its folder at a folder boundary", "[profiles]")
{
    REQUIRE(path_is_under_prefix(u8"E:\\work\\repo", u8"E:\\work"));
    REQUIRE(path_is_under_prefix(u8"E:\\work", u8"E:\\work"));
    // 대소문자·구분자·끝 구분자는 무시한다.
    REQUIRE(path_is_under_prefix(u8"e:/Work/Repo", u8"E:\\WORK\\"));
    // 폴더 이름의 앞부분만 같은 다른 폴더에는 맞지 않는다.
    REQUIRE(path_is_under_prefix(u8"E:\\workshop\\repo", u8"E:\\work") == false);
    REQUIRE(path_is_under_prefix(u8"E:\\other\\repo", u8"E:\\work") == false);
    REQUIRE(path_is_under_prefix(u8"E:\\work\\repo", u8"") == false);
    // 뿌리 규칙은 그 드라이브 전체다.
    REQUIRE(path_is_under_prefix(u8"E:\\anything\\repo", u8"E:\\"));
}

TEST_CASE("the rule with the longest folder wins and unknown profiles are skipped", "[profiles]")
{
    const std::vector<user_profile> profiles { two_profiles() };
    const std::vector<profile_rule> rules { profile_rule { u8"E:\\work", u8"개인" }, profile_rule { u8"E:\\work\\corp", u8"회사" }, profile_rule { u8"E:\\ghost", u8"없는 프로필" } };
    REQUIRE(match_profile_rule(rules, profiles, u8"E:\\work\\corp\\repo") == std::size_t { 1 });
    REQUIRE(match_profile_rule(rules, profiles, u8"E:\\work\\hobby") == std::size_t { 0 });
    REQUIRE(match_profile_rule(rules, profiles, u8"E:\\elsewhere").has_value() == false);
    REQUIRE(match_profile_rule(rules, profiles, u8"E:\\ghost\\repo").has_value() == false);
    // 같은 길이면 앞의 규칙이 이긴다.
    const std::vector<profile_rule> tie { profile_rule { u8"E:\\work", u8"회사" }, profile_rule { u8"e:/WORK", u8"개인" } };
    REQUIRE(match_profile_rule(tie, profiles, u8"E:\\work\\repo") == std::size_t { 0 });
}

TEST_CASE("a profile needs a unique name, a user name and an email", "[profiles]")
{
    const std::vector<user_profile> existing { two_profiles() };
    REQUIRE(check_user_profile(make_profile(u8"새 프로필", u8"홍", u8"a@b.c"), existing, std::nullopt).empty());
    REQUIRE(check_user_profile(make_profile(u8"", u8"홍", u8"a@b.c"), existing, std::nullopt).empty() == false);
    REQUIRE(check_user_profile(make_profile(u8" 앞공백", u8"홍", u8"a@b.c"), existing, std::nullopt).empty() == false);
    REQUIRE(check_user_profile(make_profile(u8"회사", u8"홍", u8"a@b.c"), existing, std::nullopt).empty() == false);
    // 고치는 줄 자신의 이름은 겹침이 아니다.
    REQUIRE(check_user_profile(make_profile(u8"회사", u8"홍", u8"a@b.c"), existing, 0).empty());
    REQUIRE(check_user_profile(make_profile(u8"Work", u8"홍", u8"a@b.c"), { make_profile(u8"work", u8"x", u8"x@y.z") }, std::nullopt).empty() == false);
    REQUIRE(check_user_profile(make_profile(u8"새", u8"", u8"a@b.c"), existing, std::nullopt).empty() == false);
    REQUIRE(check_user_profile(make_profile(u8"새", u8"홍", u8""), existing, std::nullopt).empty() == false);
    REQUIRE(check_user_profile(make_profile(u8"새", u8"홍", u8"no-at-sign"), existing, std::nullopt).empty() == false);
    REQUIRE(check_user_profile(make_profile(u8"새", u8"홍", u8"a b@c.d"), existing, std::nullopt).empty() == false);
    REQUIRE(check_user_profile(make_profile(u8"새", u8"홍\n길동", u8"a@b.c"), existing, std::nullopt).empty() == false);
    // 서명 키는 뜻을 몰라도 한 줄이면 받는다 (해석은 커밋 서명 쪽의 몫이다).
    user_profile signed_profile { make_profile(u8"서명", u8"홍", u8"a@b.c") };
    signed_profile.signing_key = u8"ABCD1234";
    REQUIRE(check_user_profile(signed_profile, existing, std::nullopt).empty());
    signed_profile.signing_key = u8"a\nb";
    REQUIRE(check_user_profile(signed_profile, existing, std::nullopt).empty() == false);
}

TEST_CASE("a rule needs a folder path and a known profile", "[profiles]")
{
    const std::vector<user_profile> profiles { two_profiles() };
    REQUIRE(check_profile_rule(profile_rule { u8"E:\\work", u8"회사" }, profiles).empty());
    REQUIRE(check_profile_rule(profile_rule { u8"", u8"회사" }, profiles).empty() == false);
    REQUIRE(check_profile_rule(profile_rule { u8"work", u8"회사" }, profiles).empty() == false);
    REQUIRE(check_profile_rule(profile_rule { u8"E:\\work", u8"없음" }, profiles).empty() == false);
}

TEST_CASE("renaming and removing a profile keeps rules and assignments consistent", "[profiles]")
{
    app_settings settings {};
    upsert_user_profile(settings, std::nullopt, make_profile(u8"회사", u8"홍길동", u8"hong@corp.example"));
    upsert_user_profile(settings, std::nullopt, make_profile(u8"개인", u8"길동", u8"me@example.com"));
    upsert_profile_rule(settings, profile_rule { u8"E:/work/", u8"회사" });
    assign_profile(settings, u8"E:\\work\\repo", u8"회사");
    assign_profile(settings, u8"E:\\play\\game", u8"개인");
    REQUIRE(settings.profile_rules.front().path_prefix == u8"E:\\work");

    // 같은 접두의 규칙은 프로필만 바뀐다.
    upsert_profile_rule(settings, profile_rule { u8"e:\\WORK", u8"개인" });
    REQUIRE(settings.profile_rules.size() == 1);
    REQUIRE(settings.profile_rules.front().profile == u8"개인");
    upsert_profile_rule(settings, profile_rule { u8"E:\\work", u8"회사" });

    upsert_user_profile(settings, 0, make_profile(u8"직장", u8"홍길동", u8"hong@corp.example"));
    REQUIRE(settings.profiles.front().name == u8"직장");
    REQUIRE(settings.profile_rules.front().profile == u8"직장");
    REQUIRE(assigned_profile(settings, u8"e:/work/REPO") == u8"직장");

    // 같은 저장소에 다시 적용하면 기록을 바꾼다.
    assign_profile(settings, u8"E:\\work\\repo\\", u8"개인");
    REQUIRE(settings.profile_assignments.size() == 2);
    REQUIRE(assigned_profile(settings, u8"E:\\work\\repo") == u8"개인");

    REQUIRE(remove_user_profile(settings, 1));
    REQUIRE(settings.profiles.size() == 1);
    REQUIRE(assigned_profile(settings, u8"E:\\work\\repo").has_value() == false);
    REQUIRE(assigned_profile(settings, u8"E:\\play\\game").has_value() == false);
    REQUIRE(settings.profile_rules.size() == 1);
    REQUIRE(remove_user_profile(settings, 0));
    REQUIRE(settings.profile_rules.empty());
    REQUIRE(remove_user_profile(settings, 0) == false);
}

TEST_CASE("profiles, rules and assignments survive a settings round trip and keep unknown keys", "[profiles]")
{
    app_settings settings {};
    user_profile company { make_profile(u8"회사", u8"홍길동", u8"hong@corp.example") };
    company.signing_key = u8"ABCD1234";
    upsert_user_profile(settings, std::nullopt, company);
    upsert_user_profile(settings, std::nullopt, make_profile(u8"개인", u8"길동", u8"me@example.com"));
    upsert_profile_rule(settings, profile_rule { u8"E:\\work", u8"회사" });
    assign_profile(settings, u8"E:\\work\\repo", u8"회사");

    const std::u8string json { serialize_app_settings_json(settings, u8"{\"future_key\": [1, 2]}") };
    REQUIRE(json.find(u8"future_key") != std::u8string::npos);
    const app_settings_load_result loaded { parse_app_settings_json(json) };
    REQUIRE(loaded.diagnostics.empty());
    REQUIRE(loaded.settings.profiles == settings.profiles);
    REQUIRE(loaded.settings.profile_rules == settings.profile_rules);
    REQUIRE(loaded.settings.profile_assignments == settings.profile_assignments);
    REQUIRE(loaded.settings.profiles.front().signing_key == u8"ABCD1234");

    // 쓸 것이 없으면 키를 쓰지 않는다.
    const std::u8string plain { serialize_app_settings_json(app_settings {}, u8"") };
    REQUIRE(plain.find(u8"profiles") == std::u8string::npos);
    REQUIRE(plain.find(u8"shortcuts") == std::u8string::npos);
}

TEST_CASE("broken profile entries are dropped with a warning", "[profiles]")
{
    const std::u8string json = u8R"({
        "profiles": [
            { "name": "회사", "user_name": "홍", "user_email": "a@b.c" },
            { "name": "회사", "user_name": "겹침", "user_email": "x@y.z" },
            { "user_name": "이름 없음" },
            "문자열"
        ],
        "profile_rules": [
            { "path_prefix": "E:/work", "profile": "회사" },
            { "path_prefix": "E:/ghost", "profile": "없음" },
            { "profile": "회사" }
        ],
        "profile_assignments": [
            { "path": "E:/work/repo", "profile": "회사" },
            { "path": "E:/gone", "profile": "없음" }
        ],
        "shortcuts": { "git.fetch": ["F7"], "bad": "F8", "worse": [1] },
        "command_history": ["git.pull", 3, ""]
    })";
    const app_settings_load_result loaded { parse_app_settings_json(json) };
    REQUIRE(loaded.settings.profiles.size() == 1);
    REQUIRE(loaded.settings.profiles.front().user_name == u8"홍");
    REQUIRE(loaded.settings.profile_rules.size() == 1);
    REQUIRE(loaded.settings.profile_rules.front().path_prefix == u8"E:\\work");
    REQUIRE(loaded.settings.profile_assignments.size() == 1);
    REQUIRE(loaded.settings.shortcuts.at("git.fetch") == std::vector<std::u8string> { u8"F7" });
    REQUIRE(loaded.settings.shortcuts.contains("bad") == false);
    REQUIRE(loaded.settings.command_history == std::vector<std::string> { "git.pull" });
    REQUIRE(loaded.diagnostics.size() >= 3);
    REQUIRE(loaded.corrupt == false);
}

TEST_CASE("git identity is read from --show-scope --show-origin output", "[profiles][identity]")
{
    // 전역 설정과 그 설정이 includeIf로 끌어온 파일, 저장소 설정이 섞인 실제 git 출력이다 (경로만 줄였다).
    const std::u8string output = u8"global\tfile:C:/Users/me/.gitconfig\tuser.name Global Gal\n"
                                 u8"global\tfile:C:/Users/me/.gitconfig\tuser.email g@example.com\n"
                                 u8"global\tfile:C:/Users/me/.gitconfig-work\tuser.email work@corp.example\n"
                                 u8"global\tfile:C:/Users/me/.gitconfig-work\tuser.signingkey ABCD\n"
                                 u8"local\tfile:.git/config\tuser.name Local Lou\n";
    identity_origin_hints hints {};
    hints.global_files = { u8"C:\\Users\\me\\.gitconfig" };
    const git_identity identity { parse_git_identity(output, hints) };
    REQUIRE(identity.name.has_value());
    REQUIRE(identity.name->value == u8"Local Lou");
    REQUIRE(identity.name->origin == identity_origin::local);
    REQUIRE(identity.name->file == u8".git/config");
    // 마지막 값이 이긴다: 이메일은 includeIf 파일의 값이다.
    REQUIRE(identity.email.has_value());
    REQUIRE(identity.email->value == u8"work@corp.example");
    REQUIRE(identity.email->origin == identity_origin::include);
    REQUIRE(identity.signing_key.has_value());
    REQUIRE(identity.signing_key->value == u8"ABCD");
    REQUIRE(identity.complete());
    REQUIRE(describe_identity(identity) == u8"Local Lou <work@corp.example>");
    REQUIRE(identity_origin_label(identity.email->origin) == u8"includeIf");

    // 전역 파일 경로를 모르면 global 범위는 모두 전역이다.
    const git_identity unknown_hints { parse_git_identity(output) };
    REQUIRE(unknown_hints.email->origin == identity_origin::global);
}

TEST_CASE("git identity classifies the scopes", "[profiles][identity]")
{
    const std::u8string output = u8"system\tfile:C:/Git/etc/gitconfig\tuser.name Sys\n"
                                 u8"command\tcommand line:\tuser.email cmd@example.com\n";
    const git_identity identity { parse_git_identity(output) };
    REQUIRE(identity.name->origin == identity_origin::system);
    REQUIRE(identity.email->origin == identity_origin::command_line);
    REQUIRE(identity.complete());

    const git_identity worktree { parse_git_identity(u8"worktree\tfile:C:/r/.git/config.worktree\tuser.name Tree\n") };
    REQUIRE(worktree.name->origin == identity_origin::worktree);

    // --show-scope 없이 받은 출력도 읽는다.
    const git_identity plain { parse_git_identity(u8"file:C:/Users/me/.gitconfig\tuser.name Plain\nfile:.git/config\tuser.email l@example.com\n") };
    REQUIRE(plain.name->origin == identity_origin::global);
    REQUIRE(plain.email->origin == identity_origin::local);
}

TEST_CASE("an empty or foreign output is no identity", "[profiles][identity]")
{
    REQUIRE(parse_git_identity(u8"") == git_identity {});
    REQUIRE(parse_git_identity(u8"global\tfile:x\tcore.editor vim\n") == git_identity {});
    REQUIRE(describe_identity(git_identity {}) == u8"설정되지 않음");
    REQUIRE(git_identity {}.complete() == false);
    // 이름만 있으면 커밋할 수 없다.
    const git_identity only_name { parse_git_identity(u8"local\tfile:.git/config\tuser.name Solo\n") };
    REQUIRE(only_name.complete() == false);
    REQUIRE(describe_identity(only_name).find(u8"Solo") != std::u8string::npos);
    // 값에 공백과 탭이 들어 있어도 첫 공백 뒤가 전부 값이다.
    const git_identity spaced { parse_git_identity(u8"local\tfile:.git/config\tuser.name  Spaced   Name \r\n") };
    REQUIRE(spaced.name->value == u8" Spaced   Name ");
}

TEST_CASE("an identity matches a profile by name and email", "[profiles][identity]")
{
    const git_identity identity { parse_git_identity(u8"local\tfile:.git/config\tuser.name 홍길동\nlocal\tfile:.git/config\tuser.email hong@corp.example\n") };
    REQUIRE(identity_matches_profile(identity, make_profile(u8"회사", u8"홍길동", u8"hong@corp.example")));
    REQUIRE(identity_matches_profile(identity, make_profile(u8"개인", u8"길동", u8"me@example.com")) == false);
    REQUIRE(identity_matches_profile(git_identity {}, make_profile(u8"회사", u8"홍길동", u8"hong@corp.example")) == false);
}

TEST_CASE("git's quoted origin paths are unquoted before they are compared", "[profiles][identity]")
{
    identity_origin_hints hints {};
    hints.global_files = { u8R"(C:\Users\me\.gitconfig)" };
    // 역슬래시가 든 경로는 따옴표와 두 겹 역슬래시로 온다. 비ASCII 글자는 8진수 바이트로 온다 (홍 = 355 231 215).
    std::u8string output { u8"global\t" };
    output += u8R"(file:"C:\\Users\\me\\.gitconfig")";
    output += u8"\tuser.name Quoted\nglobal\t";
    output += u8R"(file:"C:/Users/\355\231\215/.gitconfig-work")";
    output += u8"\tuser.email q@example.com\n";
    const git_identity quoted { parse_git_identity(output, hints) };
    REQUIRE(quoted.name->origin == identity_origin::global);
    REQUIRE(quoted.name->file == u8R"(C:\Users\me\.gitconfig)");
    REQUIRE(quoted.email->origin == identity_origin::include);
    REQUIRE(quoted.email->file == u8"C:/Users/홍/.gitconfig-work");
}
