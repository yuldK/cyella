// 실제 git과 임시 저장소로 작성자 신원을 읽고 `git config --local`로 쓰는 경로를 끝까지 돌린다.
// 사용자의 git 설정을 읽지도 쓰지도 않도록 모든 요청에 임시 전역 설정 파일(GIT_CONFIG_GLOBAL)과 GIT_CONFIG_NOSYSTEM=1을 건다.
#include "git_test_support.h"
#include "infrastructure/git_identity_gateway.h"
#include "infrastructure/git_tool_discovery.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_process_runner.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>

namespace {
    using namespace cyella;

    std::u8string u8_of(const std::string& text)
    {
        return std::u8string { text.begin(), text.end() };
    }

    std::u8string find_git(process_runner& runner)
    {
        const std::unique_ptr<file_probe> probe { win32::make_file_probe() };
        const git_tool_info info { resolve_git_tool(u8"", win32::current_git_tool_environment(), runner, *probe, {}) };
        return info.usable() ? info.executable : std::u8string {};
    }

    std::string slashed(const std::filesystem::path& path)
    {
        return path.generic_string();
    }

    // 임시 전역 설정과 includeIf 파일, 그 아래 저장소다. 사용자의 설정은 어디에도 닿지 않는다.
    struct identity_fixture
    {
        identity_fixture()
            : root { "identity" }
            , global_file { root.path() / "global.gitconfig" }
            , include_file { root.path() / "work.gitconfig" }
            , repository { root.path() / "work" / "repo" }
            , other_repository { root.path() / "play" / "repo" }
        {
            test::write_file(global_file,
                "[user]\n\tname = Global Gal\n\temail = global@example.com\n[includeIf \"gitdir:" + slashed(std::filesystem::canonical(root.path()) / "work")
                    + "/\"]\n\tpath = " + slashed(include_file) + "\n");
            test::write_file(include_file, "[user]\n\temail = work@corp.example\n\tsigningkey = ABCD1234\n");
            std::filesystem::create_directories(repository);
            std::filesystem::create_directories(other_repository);
            for (const std::filesystem::path& path : { repository, other_repository })
                REQUIRE(test::run_git(path, std::vector<std::string> { "init", "-q", "-b", "main" }).exit_code == 0);
        }

        [[nodiscard]] git_identity_options options() const
        {
            git_identity_options value {};
            value.hints.global_files = { u8_of(slashed(global_file)) };
            value.environment.push_back(process_environment_override { u8"GIT_CONFIG_GLOBAL", u8_of(global_file.string()) });
            value.environment.push_back(process_environment_override { u8"GIT_CONFIG_NOSYSTEM", std::u8string { u8"1" } });
            return value;
        }

        test::temporary_directory root;
        std::filesystem::path global_file;
        std::filesystem::path include_file;
        std::filesystem::path repository;
        std::filesystem::path other_repository;
    };

    std::string local_value(const identity_fixture& fixture, const std::filesystem::path& repository, const std::string& key)
    {
        // 확인도 같은 격리 환경에서 한다 (run_git은 GIT_CONFIG_GLOBAL=NUL로 사용자 설정을 끊는다).
        static_cast<void>(fixture);
        const test::git_run_result result { test::run_git(repository, std::vector<std::string> { "config", "--local", "--get", key }) };
        std::string value { result.standard_output };
        while (value.empty() == false && (value.back() == '\n' || value.back() == '\r'))
            value.pop_back();
        return value;
    }
} // namespace

TEST_CASE("the identity gateway reads global, includeIf and local values with their origins", "[profiles][identity][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not available");
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    const std::u8string git { find_git(*runner) };
    REQUIRE(git.empty() == false);
    identity_fixture fixture {};
    const git_identity_gateway gateway { *runner, git, fixture.options() };

    // includeIf 밖의 저장소는 전역 값 그대로다.
    const git_identity_read outside { gateway.read(u8_of(fixture.other_repository.string()), process_cancellation_token {}) };
    REQUIRE(outside.ok);
    REQUIRE(outside.identity.name.has_value());
    REQUIRE(outside.identity.name->value == u8"Global Gal");
    REQUIRE(outside.identity.name->origin == identity_origin::global);
    REQUIRE(outside.identity.email->value == u8"global@example.com");

    // includeIf 폴더 안의 저장소는 이메일이 포함 파일의 값이고 출처가 includeIf다.
    const git_identity_read inside { gateway.read(u8_of(fixture.repository.string()), process_cancellation_token {}) };
    REQUIRE(inside.ok);
    REQUIRE(inside.identity.name->origin == identity_origin::global);
    REQUIRE(inside.identity.email->value == u8"work@corp.example");
    REQUIRE(inside.identity.email->origin == identity_origin::include);
    REQUIRE(inside.identity.signing_key.has_value());
    REQUIRE(inside.identity.signing_key->value == u8"ABCD1234");

    // 로컬 설정이 둘 다 이긴다.
    REQUIRE(test::run_git(fixture.repository, std::vector<std::string> { "config", "--local", "user.name", "Local Lou" }).exit_code == 0);
    const git_identity_read local { gateway.read(u8_of(fixture.repository.string()), process_cancellation_token {}) };
    REQUIRE(local.identity.name->value == u8"Local Lou");
    REQUIRE(local.identity.name->origin == identity_origin::local);
}

TEST_CASE("an empty git config reads as no identity, not as an error", "[profiles][identity][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not available");
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    const std::u8string git { find_git(*runner) };
    REQUIRE(git.empty() == false);
    identity_fixture fixture {};
    test::write_file(fixture.global_file, "");
    const git_identity_gateway gateway { *runner, git, fixture.options() };
    const git_identity_read read { gateway.read(u8_of(fixture.other_repository.string()), process_cancellation_token {}) };
    REQUIRE(read.ok);
    REQUIRE(read.identity.complete() == false);
    REQUIRE(describe_identity(read.identity) == u8"설정되지 않음");
}

TEST_CASE("applying a profile writes only the repository's local config", "[profiles][identity][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not available");
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    const std::u8string git { find_git(*runner) };
    REQUIRE(git.empty() == false);
    identity_fixture fixture {};
    const std::string global_before { test::read_file(fixture.global_file) };
    const git_identity_gateway gateway { *runner, git, fixture.options() };

    const git_identity_write written { gateway.apply(u8_of(fixture.other_repository.string()), local_identity_change { .user_name = u8"홍길동", .user_email = u8"hong@corp.example" }, process_cancellation_token {}) };
    REQUIRE(written.ok);
    REQUIRE(local_value(fixture, fixture.other_repository, "user.name") == "홍길동");
    REQUIRE(local_value(fixture, fixture.other_repository, "user.email") == "hong@corp.example");
    // 전역 설정 파일은 한 글자도 바뀌지 않았다.
    REQUIRE(test::read_file(fixture.global_file) == global_before);
    // 다른 저장소에도 닿지 않았다.
    REQUIRE(local_value(fixture, fixture.repository, "user.name").empty());

    const git_identity_read after { gateway.read(u8_of(fixture.other_repository.string()), process_cancellation_token {}) };
    REQUIRE(after.identity.name->value == u8"홍길동");
    REQUIRE(after.identity.name->origin == identity_origin::local);
    REQUIRE(after.identity.email->origin == identity_origin::local);

    // 다시 적용하면 값을 덮어쓴다 (--local은 줄을 늘리지 않는다).
    REQUIRE(gateway.apply(u8_of(fixture.other_repository.string()), local_identity_change { .user_name = u8"-길동 (대시로 시작)", .user_email = u8"me@example.com" }, process_cancellation_token {}).ok);
    REQUIRE(local_value(fixture, fixture.other_repository, "user.name") == "-길동 (대시로 시작)");
    REQUIRE(local_value(fixture, fixture.other_repository, "user.email") == "me@example.com");
}

TEST_CASE("applying needs both a name and an email and reports git failures", "[profiles][identity][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not available");
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    const std::u8string git { find_git(*runner) };
    REQUIRE(git.empty() == false);
    identity_fixture fixture {};
    const git_identity_gateway gateway { *runner, git, fixture.options() };
    REQUIRE(gateway.apply(u8_of(fixture.repository.string()), local_identity_change { .user_email = u8"a@b.c" }, process_cancellation_token {}).ok == false);
    REQUIRE(gateway.apply(u8_of(fixture.repository.string()), local_identity_change { .user_name = u8"x" }, process_cancellation_token {}).ok == false);
    // 저장소가 아닌 폴더에는 `git config --local`이 쓸 곳이 없다.
    const std::filesystem::path plain { fixture.root.path() / "plain" };
    std::filesystem::create_directories(plain);
    const git_identity_write failed { gateway.apply(u8_of(plain.string()), local_identity_change { .user_name = u8"x", .user_email = u8"a@b.c" }, process_cancellation_token {}) };
    REQUIRE(failed.ok == false);
    REQUIRE(failed.message.empty() == false);
}

TEST_CASE("identity gateway commands are built without a shell", "[profiles][identity]")
{
    const std::vector<std::u8string> read { make_identity_read_arguments() };
    REQUIRE(read.front() == u8"config");
    REQUIRE(read.back() == u8"^(user\\.(name|email|signingkey)|credential\\.username|core\\.sshcommand)$");
    const std::vector<std::u8string> write { make_identity_write_arguments(u8"user.name", u8"홍 길동") };
    REQUIRE(write == std::vector<std::u8string> { u8"config", u8"--local", u8"--", u8"user.name", u8"홍 길동" });
    REQUIRE(make_identity_unset_arguments(u8"credential.username") == std::vector<std::u8string> { u8"config", u8"--local", u8"--unset-all", u8"--", u8"credential.username" });
}

TEST_CASE("applying a profile's sign-in account writes credential.username and an ssh command, and clearing removes them", "[profiles][identity][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not available");
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    const std::u8string git { find_git(*runner) };
    REQUIRE(git.empty() == false);
    identity_fixture fixture {};
    const std::string global_before { test::read_file(fixture.global_file) };
    const git_identity_gateway gateway { *runner, git, fixture.options() };
    const std::u8string repository { u8_of(fixture.other_repository.string()) };
    const std::u8string command { make_profile_ssh_command(u8"C:\\Windows\\System32\\OpenSSH\\ssh.exe", u8"C:\\Users\\me\\.ssh\\id_work") };

    local_identity_change change { .user_name = u8"Bee", .user_email = u8"bee@corp.example", .credential_username = u8"bee-work", .ssh_command = command };
    REQUIRE(gateway.apply(repository, change, process_cancellation_token {}).ok);
    CHECK(local_value(fixture, fixture.other_repository, "credential.username") == "bee-work");
    CHECK(local_value(fixture, fixture.other_repository, "core.sshCommand") == "'C:/Windows/System32/OpenSSH/ssh.exe' -i 'C:/Users/me/.ssh/id_work' -o IdentitiesOnly=yes");

    // 읽기도 두 키를 출처와 함께 돌려준다 (git은 키를 소문자로 낸다).
    const git_identity_read read { gateway.read(repository, process_cancellation_token {}) };
    REQUIRE(read.ok);
    REQUIRE(read.identity.credential_username.has_value());
    CHECK(read.identity.credential_username->value == u8"bee-work");
    CHECK(read.identity.credential_username->origin == identity_origin::local);
    CHECK(profile_ssh_key_of(read.identity) == std::optional<std::u8string> { u8"C:/Users/me/.ssh/id_work" });
    CHECK(identity_matches_profile(read.identity, user_profile { .name = u8"w", .user_name = u8"Bee", .user_email = u8"bee@corp.example", .https_username = u8"bee-work", .ssh_key = u8"C:\\Users\\me\\.ssh\\id_work" }));

    // 접속 계정이 없는 프로필을 적용하면 앱이 쓴 두 키를 지운다.
    REQUIRE(gateway.apply(repository, local_identity_change { .user_name = u8"Bee", .user_email = u8"bee@corp.example" }, process_cancellation_token {}).ok);
    CHECK(local_value(fixture, fixture.other_repository, "credential.username").empty());
    CHECK(local_value(fixture, fixture.other_repository, "core.sshCommand").empty());

    // 사용자가 손으로 쓴 ssh 명령은 앱이 지우지 않는다. 로컬 설정 지우기도 그렇다.
    REQUIRE(test::run_git(fixture.other_repository, std::vector<std::string> { "config", "--local", "core.sshCommand", "ssh -F C:/proxy/config" }).exit_code == 0);
    REQUIRE(gateway.apply(repository, change, process_cancellation_token {}).ok);
    REQUIRE(test::run_git(fixture.other_repository, std::vector<std::string> { "config", "--local", "core.sshCommand", "ssh -F C:/proxy/config" }).exit_code == 0);
    const git_identity_write cleared { gateway.apply(repository, local_identity_change { .clear = true }, process_cancellation_token {}) };
    REQUIRE(cleared.ok);
    CHECK(local_value(fixture, fixture.other_repository, "user.name").empty());
    CHECK(local_value(fixture, fixture.other_repository, "user.email").empty());
    CHECK(local_value(fixture, fixture.other_repository, "credential.username").empty());
    CHECK(local_value(fixture, fixture.other_repository, "core.sshCommand") == "ssh -F C:/proxy/config");
    // 지울 것이 없어도 실패가 아니다. 전역 설정은 그대로다.
    CHECK(gateway.apply(repository, local_identity_change { .clear = true }, process_cancellation_token {}).ok);
    CHECK(test::read_file(fixture.global_file) == global_before);
    const git_identity_read after { gateway.read(repository, process_cancellation_token {}) };
    REQUIRE(after.identity.name.has_value());
    CHECK(after.identity.name->origin == identity_origin::global);
}

TEST_CASE("git hands the repository's credential.username to the credential helper", "[profiles][identity][integration]")
{
    if (test::git_is_available() == false)
        SKIP("git is not available");
    // 한 호스트에 계정이 둘 저장된 도우미다. 저장소의 `credential.username`이 어느 계정의 토큰을 받을지 정한다.
    // GCM도 같은 길로 계정 이름을 받는다. 여기서는 사용자의 GCM 대신 store 도우미로 git이 이름을 넘기는지만 본다.
    identity_fixture fixture {};
    const std::filesystem::path store { fixture.root.path() / "credentials" };
    test::write_file(store, "https://alpha:token-a@git.example\nhttps://bee-work:token-b@git.example\n");
    REQUIRE(test::run_git(fixture.other_repository, std::vector<std::string> { "config", "--local", "credential.username", "bee-work" }).exit_code == 0);
    // run_git은 사용자의 전역 설정을 끊으므로 도우미는 명령줄로 준다.
    const test::git_run_result filled {
        test::run_git(fixture.other_repository, std::vector<std::string> { "-c", "credential.helper=store --file " + slashed(store), "credential", "fill" }, "protocol=https\nhost=git.example\n\n")
    };
    REQUIRE(filled.exit_code == 0);
    CHECK(filled.standard_output.find("username=bee-work") != std::string::npos);
    CHECK(filled.standard_output.find("password=token-b") != std::string::npos);
}
