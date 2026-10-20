// 커밋 서명을 실제 git·ssh-keygen·gpg·askpass 서버와 도우미로 끝까지 돌린다. 모두 `[integration]` 태그를 가지며 필요한 도구가 없으면 건너뛴다.
//
// 격리: 사용자의 전역 git 설정, ~/.gnupg, ~/.ssh, ssh-agent를 건드리지 않는다. 전역 설정은 임시 파일(`GIT_CONFIG_GLOBAL`),
// GNUPGHOME은 임시 폴더, 키는 임시 폴더에 새로 만든 것이다 (`signing_test_support.h`). test마다 새 프로세스로 돈다.
#include "application/git_workspace.h"
#include "application/task_scheduler.h"
#include "infrastructure/git_operation_runner.h"
#include "infrastructure/git_signing_parser.h"
#include "infrastructure/git_tool_discovery.h"
#include "infrastructure/signing_info.h"
#include "platform/win32/askpass_bridge.h"
#include "platform/win32/win32_file_probe.h"
#include "platform/win32/win32_process_runner.h"
#include "platform/win32/win32_ssh_environment.h"
#include "platform/win32/win32_workspace_files.h"

#include "askpass_test_support.h"
#include "fake_git_runner.h"
#include "git_test_support.h"
#include "signing_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
    using namespace cyella;
    using namespace std::chrono_literals;
    using cyella::test::run_git;

    constexpr std::chrono::seconds step_timeout { 90 };
    namespace fs = std::filesystem;

    std::string trimmed(std::string text)
    {
        while (text.empty() == false && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        return text;
    }

    void git_ok(const fs::path& directory, const std::vector<std::string>& arguments)
    {
        const test::git_run_result result { run_git(directory, arguments) };
        INFO("git " << (arguments.empty() ? std::string {} : arguments.front()));
        INFO("stderr: " << result.standard_error);
        REQUIRE(result.exit_code == 0);
    }

    std::string git_text(const fs::path& directory, const std::vector<std::string>& arguments)
    {
        const test::git_run_result result { run_git(directory, arguments) };
        INFO("stderr: " << result.standard_error);
        REQUIRE(result.exit_code == 0);
        return trimmed(result.standard_output);
    }

    std::string text_of(const std::u8string& text)
    {
        return std::string { text.begin(), text.end() };
    }

    // 실제 구성이다. job은 scheduler의 worker에서 돌고 사건은 test 스레드가 `pump_until`에서 반영한다.
    class live_signing_workspace
    {
    public:
        live_signing_workspace()
            : process_ { win32::make_process_runner() }
            , files_ { win32::make_workspace_files() }
            , bridge_ { events_.sink() }
        {
            const std::unique_ptr<file_probe> probe { win32::make_file_probe() };
            tool_ = resolve_git_tool({}, win32::current_git_tool_environment(), *process_, *probe, {});
            REQUIRE(tool_.usable());
            ssh_ = locate_ssh_programs(tool_.executable, win32::windows_directory(), *probe);
            bridge_.start(test::askpass_helper_path());
            REQUIRE(bridge_.available());

            git_operation_runner_options runner_options {};
            runner_options.askpass_helper = bridge_.helper_path();
            runner_options.askpass = bridge_.endpoint();
            executor_ = std::make_unique<git_operation_runner>(*process_, tool_.executable, *files_, runner_options);
            scheduler_ = std::make_unique<task_scheduler>();
            git_workspace_options workspace_options {};
            workspace_options.refresh_on_open = false;
            workspace_options.askpass = bridge_.service();
            task_scheduler* const scheduler { scheduler_.get() };
            workspace_
                = std::make_unique<git_workspace>(*executor_, [scheduler](operation_request request) { return scheduler->submit(std::move(request)); }, events_.sink(), nullptr, workspace_options);
        }

        live_signing_workspace(const live_signing_workspace&) = delete;
        live_signing_workspace& operator=(const live_signing_workspace&) = delete;

        // 실패한 test가 프롬프트에서 멈춘 프로세스를 남기지 않게 모두 정리한 뒤 닫는다.
        ~live_signing_workspace()
        {
            for (const operation_id id : workspace_->operation_order())
            {
                const operation_status* const status { workspace_->find_operation(id) };
                if (status != nullptr && status->finished() == false)
                    static_cast<void>(workspace_->cancel(id));
            }
            bridge_.shutdown();
            scheduler_->shutdown();
        }

        [[nodiscard]] git_workspace& workspace() noexcept
        {
            return *workspace_;
        }

        [[nodiscard]] const git_tool_info& tool() const noexcept
        {
            return tool_;
        }

        [[nodiscard]] const ssh_programs& ssh() const noexcept
        {
            return ssh_;
        }

        std::string open(const fs::path& path)
        {
            const std::string id { workspace_->open_repository(path.generic_u8string()) };
            wait_for_finish(submit(id, refresh_request {}));
            return id;
        }

        operation_id submit(const std::string& repository, git_operation operation)
        {
            const operation_id id { workspace_->submit(repository, std::move(operation)) };
            REQUIRE(id != 0);
            return id;
        }

        // 요청을 내고 끝날 때까지 기다린다.
        const operation_status& run(const std::string& repository, git_operation operation)
        {
            const operation_id id { submit(repository, std::move(operation)) };
            wait_for_finish(id);
            return status(id);
        }

        void pump_until(const std::function<bool()>& condition)
        {
            const auto deadline { std::chrono::steady_clock::now() + step_timeout };
            while (std::chrono::steady_clock::now() < deadline)
            {
                for (const operation_event& event : events_.take())
                    workspace_->apply(event);
                if (condition())
                    return;
                std::this_thread::sleep_for(5ms);
            }
            FAIL("timed out waiting for the operation");
        }

        void wait_for_prompt(const operation_id id, const credential_prompt_kind kind)
        {
            pump_until([this, id, kind] {
                const operation_status* const found { workspace_->find_operation(id) };
                return found != nullptr && found->state == operation_state::needs_input && found->needs_input.has_value() && found->needs_input->prompt.kind == kind;
            });
        }

        void wait_for_finish(const operation_id id)
        {
            pump_until([this, id] {
                const operation_status* const found { workspace_->find_operation(id) };
                return found != nullptr && found->finished();
            });
        }

        [[nodiscard]] const operation_status& status(const operation_id id) const
        {
            const operation_status* const found { workspace_->find_operation(id) };
            REQUIRE(found != nullptr);
            return *found;
        }

        [[nodiscard]] const repository_session& session(const std::string& repository) const
        {
            const repository_session* const found { workspace_->find_session(repository) };
            REQUIRE(found != nullptr);
            return *found;
        }

        // 커밋 하나의 서명을 검증하고 세션에 남은 결과를 돌려준다.
        const commit_signature* verify(const std::string& repository, const std::string& sha)
        {
            const operation_status& done { run(repository, verify_signatures_request { { sha } }) };
            REQUIRE(done.state == operation_state::succeeded);
            return session(repository).signature_of(sha);
        }

    private:
        std::unique_ptr<process_runner> process_ {};
        std::unique_ptr<workspace_files> files_ {};
        test::event_queue events_ {};
        win32::askpass_bridge bridge_;
        git_tool_info tool_ {};
        ssh_programs ssh_ {};
        std::unique_ptr<git_operation_runner> executor_ {};
        std::unique_ptr<task_scheduler> scheduler_ {};
        std::unique_ptr<git_workspace> workspace_ {};
    };

    std::string failure_text(const operation_status& status)
    {
        if (status.failure.has_value() == false)
            return {};
        return text_of(status.failure->summary) + " | " + text_of(status.failure->detail);
    }

    // ssh 서명을 쓰는 저장소다. 전역 설정이 아니라 이 저장소의 지역 설정이다.
    struct ssh_repository
    {
        fs::path path {};
        fs::path key {};
        fs::path allowed {};
    };

    std::string pub_of(const fs::path& key)
    {
        return key.generic_string() + ".pub";
    }

    ssh_repository make_ssh_repository(const test::signing_sandbox& sandbox, const live_signing_workspace& live, const std::string& name)
    {
        REQUIRE(live.ssh().found());
        ssh_repository result {};
        result.key = sandbox.ssh_directory() / "id_ann";
        if (fs::exists(result.key) == false)
            REQUIRE(test::make_ssh_key(fs::path { live.ssh().ssh_keygen }, result.key, "ann@example.com", ""));
        result.allowed = sandbox.root() / "allowed_signers";
        test::write_allowed_signers(result.allowed, "ann@example.com", pub_of(result.key));
        result.path = sandbox.root() / name;
        fs::create_directories(result.path);
        git_ok(result.path, { "init", "-q", "-b", "main" });
        git_ok(result.path, { "config", "--local", "gpg.format", "ssh" });
        git_ok(result.path, { "config", "--local", "user.signingkey", pub_of(result.key) });
        git_ok(result.path, { "config", "--local", "gpg.ssh.allowedSignersFile", result.allowed.generic_string() });
        git_ok(result.path, { "config", "--local", "gpg.ssh.program", fs::path { live.ssh().ssh_keygen }.generic_string() });
        return result;
    }

    void stage_file(const fs::path& repo, const std::string& name, const std::string& content)
    {
        test::write_file(repo / name, content);
        git_ok(repo, { "add", "--", name });
    }

    bool tools_available()
    {
        // 환경(GIT_CONFIG_GLOBAL 등)을 바꾸기 전에 test 지원 코드가 기본 환경을 한 번 잡게 한다.
        return test::git_is_available();
    }
} // namespace

TEST_CASE("a commit made with sign=true is ssh signed and verifies as good", "[signing][integration]")
{
    if (tools_available() == false)
        SKIP("git is not installed");
    test::signing_sandbox sandbox {};
    live_signing_workspace live {};
    if (live.ssh().found() == false)
        SKIP("OpenSSH is not installed");
    const ssh_repository repo { make_ssh_repository(sandbox, live, "signed") };
    const std::string id { live.open(repo.path) };

    stage_file(repo.path, "a.txt", "one\n");
    const operation_status& commit { live.run(id, commit_request { .message = "signed commit", .sign = true }) };
    INFO(failure_text(commit));
    REQUIRE(commit.state == operation_state::succeeded);

    // 명령줄에 -S가 있고 커밋 객체에 서명 머리가 있다.
    bool has_flag { false };
    for (const operation_command& command : commit.commands)
        has_flag = has_flag || command.command_line.find(u8" -S ") != std::u8string::npos;
    CHECK(has_flag);
    const std::string sha { git_text(repo.path, { "rev-parse", "HEAD" }) };
    CHECK(git_text(repo.path, { "cat-file", "commit", sha }).find("gpgsig") != std::string::npos);

    const commit_signature* signature { live.verify(id, sha) };
    REQUIRE(signature != nullptr);
    CHECK(signature->status == signature_status::good);
    CHECK(signature->signer == u8"ann@example.com");
    CHECK(signature->key.starts_with(u8"SHA256:"));
    CHECK(signature->note == signature_note::none);

    // 같은 커밋을 다시 요청해도 읽은 값을 쓴다 (operation을 내지 않는다).
    CHECK(live.workspace().submit(id, verify_signatures_request { { sha } }) == 0);
}

TEST_CASE("sign=false overrides commit.gpgsign and sign unset follows it", "[signing][integration]")
{
    if (tools_available() == false)
        SKIP("git is not installed");
    test::signing_sandbox sandbox {};
    live_signing_workspace live {};
    if (live.ssh().found() == false)
        SKIP("OpenSSH is not installed");
    const ssh_repository repo { make_ssh_repository(sandbox, live, "configured") };
    git_ok(repo.path, { "config", "--local", "commit.gpgsign", "true" });
    const std::string id { live.open(repo.path) };
    // refresh가 실효 설정을 snapshot에 담는다 (커밋 상자의 서명 기본값).
    CHECK(live.session(id).snapshot().signing.commit_signing());
    CHECK(live.session(id).snapshot().signing.format() == signing_format::ssh);
    CHECK(live.session(id).snapshot().signing.at(signing_key_id::commit_gpgsign).origin == config_origin::local);

    stage_file(repo.path, "a.txt", "one\n");
    const operation_status& unsigned_commit { live.run(id, commit_request { .message = "unsigned", .sign = false }) };
    REQUIRE(unsigned_commit.state == operation_state::succeeded);
    bool has_flag { false };
    for (const operation_command& command : unsigned_commit.commands)
        has_flag = has_flag || command.command_line.find(u8"--no-gpg-sign") != std::u8string::npos;
    CHECK(has_flag);
    const std::string first { git_text(repo.path, { "rev-parse", "HEAD" }) };
    const commit_signature* none { live.verify(id, first) };
    REQUIRE(none != nullptr);
    CHECK(none->status == signature_status::none);

    stage_file(repo.path, "b.txt", "two\n");
    const operation_status& followed { live.run(id, commit_request { .message = "follows config" }) };
    INFO(failure_text(followed));
    REQUIRE(followed.state == operation_state::succeeded);
    const std::string second { git_text(repo.path, { "rev-parse", "HEAD" }) };
    const commit_signature* good { live.verify(id, second) };
    REQUIRE(good != nullptr);
    CHECK(good->status == signature_status::good);
}

TEST_CASE("verify_signatures reports tampered, unknown signer and unverifiable ssh signatures", "[signing][integration]")
{
    if (tools_available() == false)
        SKIP("git is not installed");
    test::signing_sandbox sandbox {};
    live_signing_workspace live {};
    if (live.ssh().found() == false)
        SKIP("OpenSSH is not installed");
    ssh_repository repo { make_ssh_repository(sandbox, live, "matrix") };
    const std::string id { live.open(repo.path) };

    stage_file(repo.path, "a.txt", "one\n");
    git_ok(repo.path, { "commit", "-q", "-m", "plain" });
    const std::string plain { git_text(repo.path, { "rev-parse", "HEAD" }) };
    stage_file(repo.path, "a.txt", "two\n");
    REQUIRE(live.run(id, commit_request { .message = "signed by ann", .sign = true }).state == operation_state::succeeded);
    const std::string good { git_text(repo.path, { "rev-parse", "HEAD" }) };

    // 서명된 커밋의 제목만 바꾼 객체: 서명이 맞지 않는다.
    const std::string original { git_text(repo.path, { "cat-file", "commit", good }) };
    std::string forged { original };
    forged.replace(forged.find("signed by ann"), std::string { "signed by ann" }.size(), "tampered by eve");
    const fs::path forged_file { sandbox.root() / "forged.txt" };
    test::write_file(forged_file, forged + "\n");
    const std::string tampered { git_text(repo.path, { "hash-object", "-t", "commit", "-w", forged_file.generic_string() }) };

    // 다른 키로 서명한 커밋: 서명은 올바르지만 allowed signers에 없다.
    fs::path other_key { sandbox.ssh_directory() / "id_other" };
    REQUIRE(test::make_ssh_key(fs::path { live.ssh().ssh_keygen }, other_key, "other@example.com", ""));
    git_ok(repo.path, { "config", "--local", "user.signingkey", pub_of(other_key) });
    stage_file(repo.path, "a.txt", "three\n");
    REQUIRE(live.run(id, commit_request { .message = "signed by someone else", .sign = true }).state == operation_state::succeeded);
    const std::string other { git_text(repo.path, { "rev-parse", "HEAD" }) };

    const operation_status& done { live.run(id, verify_signatures_request { { plain, good, tampered, other } }) };
    INFO(failure_text(done));
    REQUIRE(done.state == operation_state::succeeded);
    const repository_session& session { live.session(id) };
    REQUIRE(session.signature_of(plain) != nullptr);
    CHECK(session.signature_of(plain)->status == signature_status::none);
    REQUIRE(session.signature_of(good) != nullptr);
    CHECK(session.signature_of(good)->status == signature_status::good);
    REQUIRE(session.signature_of(tampered) != nullptr);
    CHECK(session.signature_of(tampered)->status == signature_status::bad);
    REQUIRE(session.signature_of(other) != nullptr);
    CHECK(session.signature_of(other)->status == signature_status::unknown_key);
    CHECK(session.signature_of(other)->signer.empty());
    CHECK(session.signature_of(other)->note == signature_note::none);

    // allowed signers 설정이 없다: git은 서명 없는 커밋(N)과 구별하지 못하지만 앱은 확인할 수 없음으로 알린다.
    git_ok(repo.path, { "config", "--local", "--unset", "gpg.ssh.allowedSignersFile" });
    live.workspace().find_session(id)->forget_signatures();
    const operation_status& unset { live.run(id, verify_signatures_request { { plain, good } }) };
    REQUIRE(unset.state == operation_state::succeeded);
    CHECK(live.session(id).signature_of(plain)->status == signature_status::none);
    REQUIRE(live.session(id).signature_of(good) != nullptr);
    CHECK(live.session(id).signature_of(good)->status == signature_status::cannot_check);
    CHECK(live.session(id).signature_of(good)->note == signature_note::allowed_signers_missing);
    CHECK(signature_badge_label(*live.session(id).signature_of(good)) == u8"확인할 수 없음 (allowed signers 없음)");

    // 설정은 있는데 파일이 없다: 같은 결과다.
    git_ok(repo.path, { "config", "--local", "gpg.ssh.allowedSignersFile", (sandbox.root() / "nowhere").generic_string() });
    live.workspace().find_session(id)->forget_signatures();
    REQUIRE(live.run(id, verify_signatures_request { { good } }).state == operation_state::succeeded);
    REQUIRE(live.session(id).signature_of(good) != nullptr);
    CHECK(live.session(id).signature_of(good)->note == signature_note::allowed_signers_missing);
}

TEST_CASE("create_tag with sign=true makes a verifiable signed tag and sign=false overrides tag.gpgsign", "[signing][integration]")
{
    if (tools_available() == false)
        SKIP("git is not installed");
    test::signing_sandbox sandbox {};
    live_signing_workspace live {};
    if (live.ssh().found() == false)
        SKIP("OpenSSH is not installed");
    const ssh_repository repo { make_ssh_repository(sandbox, live, "tags") };
    stage_file(repo.path, "a.txt", "one\n");
    git_ok(repo.path, { "commit", "-q", "-m", "base" });
    const std::string id { live.open(repo.path) };

    const operation_status& signed_tag { live.run(id, create_tag_request { .name = "v1", .sha = {}, .annotated = false, .message = "release one", .sign = true }) };
    INFO(failure_text(signed_tag));
    REQUIRE(signed_tag.state == operation_state::succeeded);
    CHECK(git_text(repo.path, { "cat-file", "-t", "v1" }) == "tag");
    CHECK(run_git(repo.path, std::vector<std::string> { "tag", "-v", "v1" }).exit_code == 0);

    // tag.gpgsign가 켜져 있어도 서명을 끄면 가벼운 태그가 만들어진다 (git은 그대로면 서명한 주석 태그를 만들려다 메시지가 없어 실패한다).
    git_ok(repo.path, { "config", "--local", "tag.gpgsign", "true" });
    const operation_status& light { live.run(id, create_tag_request { .name = "v2", .sign = false }) };
    INFO(failure_text(light));
    REQUIRE(light.state == operation_state::succeeded);
    CHECK(git_text(repo.path, { "cat-file", "-t", "v2" }) == "commit");
}

TEST_CASE("an ssh key passphrase is asked through the app and the answer completes the signature", "[signing][askpass][integration]")
{
    if (tools_available() == false)
        SKIP("git is not installed");
    test::signing_sandbox sandbox {};
    live_signing_workspace live {};
    if (live.ssh().found() == false)
        SKIP("OpenSSH is not installed");
    ssh_repository repo { make_ssh_repository(sandbox, live, "protected") };
    const fs::path protected_key { sandbox.ssh_directory() / "id_protected" };
    REQUIRE(test::make_ssh_key(fs::path { live.ssh().ssh_keygen }, protected_key, "ann@example.com", "correct horse"));
    test::write_allowed_signers(repo.allowed, "ann@example.com", pub_of(protected_key));
    git_ok(repo.path, { "config", "--local", "user.signingkey", pub_of(protected_key) });
    const std::string id { live.open(repo.path) };

    stage_file(repo.path, "a.txt", "one\n");
    const operation_id commit { live.submit(id, commit_request { .message = "protected", .sign = true }) };
    live.wait_for_prompt(commit, credential_prompt_kind::ssh_key_passphrase);
    {
        const operation_status& waiting { live.status(commit) };
        REQUIRE(waiting.needs_input.has_value());
        CHECK(waiting.needs_input->kind == input_kind::passphrase);
        CHECK(waiting.needs_input->secret);
        CHECK(waiting.needs_input->prompt.key_path.find(u8"id_protected") != std::u8string::npos);
        CHECK(waiting.cancellable);
        CHECK_FALSE(waiting.finished());
        REQUIRE(live.workspace().answer_input(commit, waiting.needs_input->prompt_id, u8"correct horse"));
    }
    live.wait_for_finish(commit);
    INFO(failure_text(live.status(commit)));
    REQUIRE(live.status(commit).state == operation_state::succeeded);
    // 답은 작업 기록 어디에도 남지 않는다.
    for (const operation_command& command : live.status(commit).commands)
        CHECK(command.command_line.find(u8"correct horse") == std::u8string::npos);

    const commit_signature* signature { live.verify(id, git_text(repo.path, { "rev-parse", "HEAD" })) };
    REQUIRE(signature != nullptr);
    CHECK(signature->status == signature_status::good);
}

TEST_CASE("closing the ssh signing passphrase prompt cancels the commit and wrong passphrases end as signing_passphrase_failed", "[signing][askpass][integration]")
{
    if (tools_available() == false)
        SKIP("git is not installed");
    test::signing_sandbox sandbox {};
    live_signing_workspace live {};
    if (live.ssh().found() == false)
        SKIP("OpenSSH is not installed");
    ssh_repository repo { make_ssh_repository(sandbox, live, "refused") };
    const fs::path protected_key { sandbox.ssh_directory() / "id_protected" };
    REQUIRE(test::make_ssh_key(fs::path { live.ssh().ssh_keygen }, protected_key, "ann@example.com", "correct horse"));
    git_ok(repo.path, { "config", "--local", "user.signingkey", pub_of(protected_key) });
    const std::string id { live.open(repo.path) };
    stage_file(repo.path, "a.txt", "one\n");

    // 대화 상자를 답 없이 닫는다.
    const operation_id closed { live.submit(id, commit_request { .message = "closed", .sign = true }) };
    live.wait_for_prompt(closed, credential_prompt_kind::ssh_key_passphrase);
    REQUIRE(live.workspace().cancel_input(closed, live.status(closed).needs_input->prompt_id));
    live.wait_for_finish(closed);
    INFO(failure_text(live.status(closed)));
    CHECK(live.status(closed).state == operation_state::failed);
    REQUIRE(live.status(closed).failure.has_value());
    CHECK(live.status(closed).failure->kind == failure_kind::auth_cancelled);
    CHECK(git_text(repo.path, { "rev-list", "--all", "--count" }) == "0");

    // 틀린 passphrase를 계속 답한다. ssh-keygen이 몇 번 다시 묻다가 포기한다.
    const operation_id wrong { live.submit(id, commit_request { .message = "wrong", .sign = true }) };
    for (int attempt { 0 }; attempt < 6; ++attempt)
    {
        live.pump_until([&live, wrong] {
            const operation_status& current { live.status(wrong) };
            return current.finished() || (current.state == operation_state::needs_input && current.needs_input.has_value());
        });
        if (live.status(wrong).finished())
            break;
        REQUIRE(live.workspace().answer_input(wrong, live.status(wrong).needs_input->prompt_id, u8"not it"));
    }
    live.wait_for_finish(wrong);
    INFO(failure_text(live.status(wrong)));
    CHECK(live.status(wrong).state == operation_state::failed);
    REQUIRE(live.status(wrong).failure.has_value());
    CHECK(live.status(wrong).failure->kind == failure_kind::signing_passphrase_failed);
    CHECK(git_text(repo.path, { "rev-list", "--all", "--count" }) == "0");
}

TEST_CASE("ssh signing failures are classified with a Korean summary", "[signing][integration]")
{
    if (tools_available() == false)
        SKIP("git is not installed");
    test::signing_sandbox sandbox {};
    live_signing_workspace live {};
    if (live.ssh().found() == false)
        SKIP("OpenSSH is not installed");
    const ssh_repository repo { make_ssh_repository(sandbox, live, "failing") };
    const std::string id { live.open(repo.path) };
    stage_file(repo.path, "a.txt", "one\n");

    git_ok(repo.path, { "config", "--local", "user.signingkey", (sandbox.root() / "nowhere" / "id.pub").generic_string() });
    const operation_status& missing { live.run(id, commit_request { .message = "x", .sign = true }) };
    REQUIRE(missing.failure.has_value());
    CHECK(missing.failure->kind == failure_kind::signing_key_missing);
    CHECK(missing.failure->summary.find(u8".pub") != std::u8string::npos);

    git_ok(repo.path, { "config", "--local", "user.signingkey", pub_of(repo.key) });
    git_ok(repo.path, { "config", "--local", "gpg.ssh.program", (sandbox.root() / "nowhere" / "ssh-keygen.exe").generic_string() });
    const operation_status& spawn { live.run(id, commit_request { .message = "x", .sign = true }) };
    REQUIRE(spawn.failure.has_value());
    CHECK(spawn.failure->kind == failure_kind::signing_failed);
    CHECK(spawn.failure->summary.find(u8"gpg.ssh.program") != std::u8string::npos);
    CHECK(git_text(repo.path, { "rev-list", "--all", "--count" }) == "0");
}

TEST_CASE("a gpg signed commit made through the app verifies as good and failures are classified", "[signing][gpg][integration]")
{
    if (tools_available() == false)
        SKIP("git is not installed");
    test::signing_sandbox sandbox {};
    live_signing_workspace live {};
    const std::optional<fs::path> gpg { test::find_gpg(live.tool().executable) };
    if (gpg.has_value() == false)
        SKIP("gpg is not installed");
    sandbox.set_gpgconf(gpg->parent_path() / "gpgconf.exe");
    sandbox.append_global_config("[gpg]\n\tprogram = " + gpg->generic_string() + "\n");

    // 임시 GNUPGHOME에 passphrase 없는 키를 만든다.
    const test::program_result generated { test::run_program(*gpg, { "--batch", "--passphrase", "", "--quick-gen-key", "Ann Author <ann@example.com>", "ed25519", "sign", "never" }, sandbox.root()) };
    INFO(generated.standard_error);
    REQUIRE(generated.exit_code == 0);
    const test::program_result listing { test::run_program(*gpg, { "--list-secret-keys", "--with-colons", "--batch", "--fixed-list-mode", "--with-fingerprint" }, sandbox.root()) };
    const std::vector<gpg_secret_key> keys { parse_gpg_secret_keys(listing.standard_output, 0) };
    REQUIRE(keys.size() == 1);
    CHECK(keys[0].can_sign);
    CHECK(keys[0].fingerprint.size() == 40);

    const fs::path repo { sandbox.root() / "gpg" };
    fs::create_directories(repo);
    git_ok(repo, { "init", "-q", "-b", "main" });
    git_ok(repo, { "config", "--local", "user.signingkey", keys[0].fingerprint });
    const std::string id { live.open(repo) };

    stage_file(repo, "a.txt", "one\n");
    const operation_status& commit { live.run(id, commit_request { .message = "gpg signed", .sign = true }) };
    INFO(failure_text(commit));
    REQUIRE(commit.state == operation_state::succeeded);
    const std::string sha { git_text(repo, { "rev-parse", "HEAD" }) };
    const commit_signature* signature { live.verify(id, sha) };
    REQUIRE(signature != nullptr);
    CHECK(signature->status == signature_status::good);
    CHECK(signature->signer.find(u8"ann@example.com") != std::u8string::npos);
    CHECK(signature->key.size() == 16);
    CHECK(signature->note == signature_note::none);

    // 서명 태그도 gpg로 서명되고 git이 확인한다.
    REQUIRE(live.run(id, create_tag_request { .name = "g1", .message = "gpg tag", .sign = true }).state == operation_state::succeeded);
    CHECK(run_git(repo, std::vector<std::string> { "tag", "-v", "g1" }).exit_code == 0);

    // 서명 키가 없다: gpg의 `No secret key`는 signing_key_missing이고 gpg 쪽 문구로 알린다.
    git_ok(repo, { "config", "--local", "user.signingkey", "nobody@example.com" });
    stage_file(repo, "b.txt", "two\n");
    const operation_status& missing { live.run(id, commit_request { .message = "no key", .sign = true }) };
    REQUIRE(missing.failure.has_value());
    CHECK(missing.failure->kind == failure_kind::signing_key_missing);
    CHECK(missing.failure->summary.find(u8"gpg") != std::u8string::npos);

    // gpg 프로그램을 실행하지 못한다.
    git_ok(repo, { "config", "--local", "user.signingkey", keys[0].fingerprint });
    git_ok(repo, { "config", "--local", "gpg.program", (sandbox.root() / "nowhere" / "gpg.exe").generic_string() });
    const operation_status& spawn { live.run(id, commit_request { .message = "no gpg", .sign = true }) };
    REQUIRE(spawn.failure.has_value());
    CHECK(spawn.failure->kind == failure_kind::signing_failed);
    CHECK(spawn.failure->summary.find(u8"gpg.program") != std::u8string::npos);
    CHECK(git_text(repo, { "rev-list", "--all", "--count" }) == "1");
}

TEST_CASE("a gpg that cannot ask for the passphrase fails with the pinentry hint", "[signing][gpg][integration]")
{
    if (tools_available() == false)
        SKIP("git is not installed");
    test::signing_sandbox sandbox {};
    live_signing_workspace live {};
    const std::optional<fs::path> gpg { test::find_gpg(live.tool().executable) };
    if (gpg.has_value() == false)
        SKIP("gpg is not installed");
    sandbox.set_gpgconf(gpg->parent_path() / "gpgconf.exe");

    // passphrase가 걸린 키다. loopback으로 만들 때만 passphrase를 넘기고 서명할 때는 agent에 캐시가 없다.
    const test::program_result generated
        = test::run_program(*gpg, { "--batch", "--pinentry-mode", "loopback", "--passphrase", "pw123", "--quick-gen-key", "Locked <locked@example.com>", "ed25519", "sign", "never" }, sandbox.root());
    INFO(generated.standard_error);
    REQUIRE(generated.exit_code == 0);
    sandbox.kill_agent();
    std::this_thread::sleep_for(500ms);

    // pinentry를 띄울 수 없는 gpg(`--pinentry-mode error`)를 gpg.program으로 건다. 진짜 pinentry는 GUI라 사용자 화면에 창을 띄우므로 쓰지 않는다.
    const fs::path wrapper { sandbox.root() / "gpg-no-pinentry.sh" };
    test::write_file(wrapper, "#!/bin/sh\nexec gpg --pinentry-mode error \"$@\"\n");
    const fs::path repo { sandbox.root() / "locked" };
    fs::create_directories(repo);
    git_ok(repo, { "init", "-q", "-b", "main" });
    git_ok(repo, { "config", "--local", "user.signingkey", "locked@example.com" });
    git_ok(repo, { "config", "--local", "gpg.program", wrapper.generic_string() });
    const std::string id { live.open(repo) };
    stage_file(repo, "a.txt", "one\n");

    const operation_status& commit { live.run(id, commit_request { .message = "locked", .sign = true }) };
    INFO(failure_text(commit));
    REQUIRE(commit.failure.has_value());
    CHECK(commit.failure->kind == failure_kind::signing_passphrase_failed);
    CHECK(commit.failure->summary == u8"gpg가 passphrase를 받지 못했습니다 — gpg-agent 또는 pinentry 설정을 확인하세요.");
}

namespace {
    signing_info_sources make_sources(live_signing_workspace& live, process_runner& runner, const file_probe& probe, const fs::path& repository, const fs::path& fallback)
    {
        signing_info_sources sources {};
        sources.runner = &runner;
        sources.git_executable = live.tool().executable;
        sources.repository_path = repository.empty() ? std::u8string {} : repository.generic_u8string();
        sources.fallback_directory = fallback.generic_u8string();
        sources.probe = &probe;
        return sources;
    }
} // namespace

TEST_CASE("collect_signing_info reads the effective, local and global config and the gpg keys", "[signing][info][integration]")
{
    if (tools_available() == false)
        SKIP("git is not installed");
    test::signing_sandbox sandbox {};
    live_signing_workspace live {};
    const std::optional<fs::path> gpg { test::find_gpg(live.tool().executable) };
    if (gpg.has_value() == false || live.ssh().found() == false)
        SKIP("gpg or OpenSSH is not installed");
    sandbox.set_gpgconf(gpg->parent_path() / "gpgconf.exe");
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    const std::unique_ptr<file_probe> probe { win32::make_file_probe() };

    sandbox.append_global_config("[commit]\n\tgpgsign = true\n[tag]\n\tgpgsign\n[gpg]\n\tprogram = " + gpg->generic_string() + "\n");
    const ssh_repository repo { make_ssh_repository(sandbox, live, "info") };
    git_ok(repo.path, { "config", "--local", "commit.gpgsign", "false" });
    REQUIRE(test::run_program(*gpg, { "--batch", "--passphrase", "", "--quick-gen-key", "Ann Author <ann@example.com>", "ed25519", "sign", "never" }, sandbox.root()).exit_code == 0);

    const signing_info_snapshot info { collect_signing_info(make_sources(live, *runner, *probe, repo.path, sandbox.root())) };
    REQUIRE(info.config_read);
    CHECK(info.repository);
    // 전역은 commit.gpgsign=true, 지역이 false로 덮는다.
    CHECK(info.config.global.commit_signing());
    CHECK(info.config.local.commit_signing() == false);
    CHECK(info.config.effective.commit_signing() == false);
    CHECK(info.config.effective.at(signing_key_id::commit_gpgsign).origin == config_origin::local);
    CHECK(info.config.effective.tag_signing());
    CHECK(info.config.effective.at(signing_key_id::tag_gpgsign).valueless);
    CHECK(info.config.effective.format() == signing_format::ssh);
    CHECK(info.config.effective.at(signing_key_id::user_signingkey).value == pub_of(repo.key));
    CHECK(info.config.effective.at(signing_key_id::gpg_program).origin == config_origin::global);
    CHECK(info.config.effective.at(signing_key_id::gpg_ssh_program).origin == config_origin::local);
    // ~ 를 펼친 경로로 allowed signers 파일이 있는지 본다.
    CHECK(info.allowed_signers_exists);
    CHECK(fs::path { info.allowed_signers_path }.generic_string() == repo.allowed.generic_string());
    // 임시 GNUPGHOME의 키만 나온다.
    REQUIRE(info.gpg_keys_read);
    REQUIRE(info.gpg_keys.size() == 1);
    CHECK(info.gpg_problem.empty());
    CHECK(info.gpg_keys[0].can_sign);
    CHECK(info.gpg_keys[0].user_ids == std::vector<std::u8string> { u8"Ann Author <ann@example.com>" });
    CHECK(info.default_gpg.empty() == false);

    // 저장소 없이 읽으면 전역만 보인다.
    const signing_info_snapshot global_only { collect_signing_info(make_sources(live, *runner, *probe, {}, sandbox.root())) };
    REQUIRE(global_only.config_read);
    CHECK(global_only.repository == false);
    CHECK(global_only.config.effective.commit_signing());
    CHECK(global_only.config.local.at(signing_key_id::gpg_format).is_set() == false);
    CHECK(global_only.config.effective.format() == signing_format::openpgp);
}

TEST_CASE("collect_signing_info explains a missing gpg and an unreadable program", "[signing][info][integration]")
{
    if (tools_available() == false)
        SKIP("git is not installed");
    test::signing_sandbox sandbox {};
    live_signing_workspace live {};
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    const std::unique_ptr<file_probe> probe { win32::make_file_probe() };
    sandbox.append_global_config("[gpg]\n\tprogram = " + (sandbox.root() / "nowhere" / "gpg.exe").generic_string() + "\n");

    const signing_info_snapshot info { collect_signing_info(make_sources(live, *runner, *probe, {}, sandbox.root())) };
    REQUIRE(info.config_read);
    CHECK(info.gpg_keys_read == false);
    CHECK(info.gpg_keys.empty());
    CHECK(info.gpg_problem.find(u8"gpg.program") != std::u8string::npos);
}

TEST_CASE("write_signing_config changes local and global settings and unsets them", "[signing][info][integration]")
{
    if (tools_available() == false)
        SKIP("git is not installed");
    test::signing_sandbox sandbox {};
    live_signing_workspace live {};
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    const std::unique_ptr<file_probe> probe { win32::make_file_probe() };
    const fs::path repo { sandbox.root() / "writable" };
    fs::create_directories(repo);
    git_ok(repo, { "init", "-q", "-b", "main" });
    const signing_info_sources sources { make_sources(live, *runner, *probe, repo, sandbox.root()) };

    // 지역: 이 저장소의 .git/config에만 쓴다. 전역 파일은 그대로다.
    const std::string global_before { test::read_file(sandbox.global_config()) };
    const signing_write_result local { write_signing_config(sources, false, { { signing_key_id::commit_gpgsign, "true" }, { signing_key_id::gpg_format, "ssh" } }) };
    INFO(text_of(local.error));
    REQUIRE(local.ok);
    CHECK(git_text(repo, { "config", "--local", "commit.gpgsign" }) == "true");
    CHECK(git_text(repo, { "config", "--local", "gpg.format" }) == "ssh");
    CHECK(test::read_file(sandbox.global_config()) == global_before);
    signing_info_snapshot info { collect_signing_info(sources) };
    CHECK(info.config.effective.commit_signing());
    CHECK(info.config.effective.format() == signing_format::ssh);

    // 지우기: 있는 설정을 지우고, 이미 없는 설정을 지워도 실패가 아니다.
    const signing_write_result unset { write_signing_config(sources, false, { { signing_key_id::gpg_format, std::nullopt }, { signing_key_id::gpg_program, std::nullopt } }) };
    INFO(text_of(unset.error));
    REQUIRE(unset.ok);
    CHECK(run_git(repo, std::vector<std::string> { "config", "--local", "gpg.format" }).exit_code == 1);

    // 전역: 임시 전역 설정 파일(GIT_CONFIG_GLOBAL)에 쓴다.
    const signing_write_result global { write_signing_config(sources, true, { { signing_key_id::tag_gpgsign, "true" }, { signing_key_id::user_signingkey, "C:/keys/id_ed25519.pub" } }) };
    REQUIRE(global.ok);
    CHECK(test::read_file(sandbox.global_config()).find("signingkey = C:/keys/id_ed25519.pub") != std::string::npos);
    info = collect_signing_info(sources);
    CHECK(info.config.global.tag_signing());
    CHECK(info.config.global.at(signing_key_id::user_signingkey).value == "C:/keys/id_ed25519.pub");
    CHECK(info.config.local.at(signing_key_id::user_signingkey).is_set() == false);

    // 저장소가 없으면 지역 설정은 바꿀 수 없다.
    const signing_info_sources no_repository { make_sources(live, *runner, *probe, {}, sandbox.root()) };
    const signing_write_result refused { write_signing_config(no_repository, false, { { signing_key_id::commit_gpgsign, "true" } }) };
    CHECK(refused.ok == false);
    CHECK(refused.error.find(u8"저장소") != std::u8string::npos);
    // 줄바꿈이 든 값은 git을 부르기 전에 거절한다.
    const signing_write_result invalid { write_signing_config(sources, false, { { signing_key_id::user_signingkey, "a\nb" } }) };
    CHECK(invalid.ok == false);
}
