// 환경 설정의 SSH 도구(ssh-keygen·ssh -T·ssh-add -l)다. operation으로 도는 두 도구는 가짜 runner로 명령줄·환경·분류를,
// 읽기 도구는 가짜 프로세스와 임시 폴더로 결과를 검증한다. 실제 ssh와 askpass는 `[integration]` test가 맡는다.
#include "application/git_workspace.h"
#include "infrastructure/git_operation_runner.h"
#include "infrastructure/ssh_info.h"

#include "fake_git_runner.h"
#include "temporary_directory_support.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {
    using namespace cyella;

    // 탐색에 쓰는 가짜 probe다.
    class fake_probe final : public file_probe
    {
    public:
        [[nodiscard]] path_kind probe(const std::u8string_view absolute_path) const noexcept override
        {
            for (const std::u8string& file : files)
                if (file == absolute_path)
                    return path_kind::file;
            return path_kind::missing;
        }

        std::vector<std::u8string> files {};
    };

    struct ssh_harness
    {
        explicit ssh_harness(const bool with_tools = true)
            : runner { process, u8"C:/Git/cmd/git.exe", files, make_options(with_tools) }
            , workspace { runner, dispatcher.dispatcher(), events.sink(), nullptr, git_workspace_options { .refresh_on_open = false } }
        {}

        static git_operation_runner_options make_options(const bool with_tools)
        {
            git_operation_runner_options options {};
            if (with_tools)
            {
                options.ssh_executable = u8"C:/Git/usr/bin/ssh.exe";
                options.ssh_keygen_executable = u8"C:/Git/usr/bin/ssh-keygen.exe";
            }
            options.askpass_helper = u8"C:/Git/cyella-askpass.exe";
            options.askpass = askpass_endpoint { u8"\\\\.\\pipe\\cyella-test", u8"secret" };
            return options;
        }

        // 한 operation을 내고 끝까지 돌려 반영한다.
        operation_id run(git_operation operation)
        {
            const operation_id id { workspace.submit_setup(std::move(operation)) };
            REQUIRE(id != 0);
            while (dispatcher.run())
            {
            }
            for (const operation_event& event : events.take())
                workspace.apply(event);
            return id;
        }

        [[nodiscard]] const operation_status& status(const operation_id id) const
        {
            const operation_status* const found { workspace.find_operation(id) };
            REQUIRE(found != nullptr);
            return *found;
        }

        test::fake_git_runner process {};
        test::fake_workspace_files files {};
        test::manual_dispatcher dispatcher {};
        test::event_queue events {};
        git_operation_runner runner;
        git_workspace workspace;
    };
} // namespace

TEST_CASE("an authenticated ssh -T is a successful test even though GitHub exits with 1", "[ssh][operation]")
{
    ssh_harness test {};
    test.process.when({ "-T" }, test::scripted_response { .exit_code = 1, .standard_error_lines = { "Hi alice! You've successfully authenticated, but GitHub does not provide shell access." } });
    const operation_id id { test.run(ssh_connection_test_request { .host = u8"github.com" }) };

    const operation_status& status { test.status(id) };
    CHECK(status.kind == operation_kind::ssh_connection_test);
    CHECK(status.state == operation_state::succeeded);
    CHECK(status.target_path == u8"github.com");
    const std::optional<ssh_connection_classification> result { test.workspace.ssh_connection_result_of(id) };
    REQUIRE(result.has_value());
    CHECK(result->result == ssh_connection_result::authenticated);
    CHECK(result->authenticated_user == u8"alice");

    // 질문은 askpass로 간다. 이 operation의 id가 환경에 실린다.
    const std::vector<test::recorded_call> calls { test.process.calls() };
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].arguments == test::text_list { "-T", "-o", "StrictHostKeyChecking=ask", "git@github.com" });
    CHECK(calls[0].variable(u8"SSH_ASKPASS") == std::optional<std::string> { "C:/Git/cyella-askpass.exe" });
    CHECK(calls[0].variable(u8"SSH_ASKPASS_REQUIRE") == std::optional<std::string> { "force" });
    CHECK(calls[0].variable(u8"CYELLA_ASKPASS_OPERATION") == std::optional<std::string> { std::to_string(id) });
    CHECK(calls[0].variable(u8"CYELLA_ASKPASS_TOKEN") == std::optional<std::string> { "secret" });
    // 작업 로그에 명령이 한 줄 남는다.
    CHECK(status.commands.size() == 1);
}

TEST_CASE("failed connections are classified and still end as a finished test", "[ssh][operation]")
{
    ssh_harness test {};
    SECTION("permission denied")
    {
        test.process.when({ "-T" }, test::scripted_response { .exit_code = 255, .standard_error_lines = { "git@github.com: Permission denied (publickey)." } });
        const operation_id id { test.run(ssh_connection_test_request { .host = u8"github.com" }) };
        CHECK(test.status(id).state == operation_state::succeeded);
        CHECK(test.workspace.ssh_connection_result_of(id)->result == ssh_connection_result::permission_denied);
    }

    SECTION("unresolvable host")
    {
        test.process.when({ "-T" }, test::scripted_response { .exit_code = 255, .standard_error_lines = { "ssh: Could not resolve hostname nowhere.invalid: Name or service not known" } });
        const operation_id id { test.run(ssh_connection_test_request { .host = u8"nowhere.invalid", .user_name = u8"me", .port = 2200 }) };
        CHECK(test.workspace.ssh_connection_result_of(id)->result == ssh_connection_result::connection_failed);
        const std::vector<test::recorded_call> calls { test.process.calls() };
        REQUIRE(calls.size() == 1);
        CHECK(calls[0].arguments == test::text_list { "-T", "-o", "StrictHostKeyChecking=ask", "-p", "2200", "me@nowhere.invalid" });
    }

    SECTION("changed host key")
    {
        test.process.when({ "-T" }, test::scripted_response { .exit_code = 255, .standard_error_lines = { "@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @" } });
        const operation_id id { test.run(ssh_connection_test_request { .host = u8"github.com" }) };
        CHECK(test.workspace.ssh_connection_result_of(id)->result == ssh_connection_result::host_key_changed);
    }

    SECTION("an unrecognised answer is reported as unknown")
    {
        test.process.when({ "-T" }, test::scripted_response { .exit_code = 255, .standard_error_lines = { "something odd" } });
        const operation_id id { test.run(ssh_connection_test_request { .host = u8"github.com" }) };
        CHECK(test.workspace.ssh_connection_result_of(id)->result == ssh_connection_result::unknown);
    }
}

TEST_CASE("an unusable target or missing ssh fails without running anything", "[ssh][operation]")
{
    SECTION("option injection through the host")
    {
        ssh_harness test {};
        const operation_id id { test.run(ssh_connection_test_request { .host = u8"-oProxyCommand=calc" }) };
        CHECK(test.status(id).state == operation_state::failed);
        CHECK(test.status(id).failure->kind == failure_kind::invalid_request);
        CHECK(test.process.calls().empty());
        CHECK_FALSE(test.workspace.ssh_connection_result_of(id).has_value());
    }

    SECTION("ssh is not installed")
    {
        ssh_harness test { false };
        const operation_id id { test.run(ssh_connection_test_request { .host = u8"github.com" }) };
        CHECK(test.status(id).state == operation_state::failed);
        CHECK(test.status(id).failure->summary.find(u8"ssh를 찾지 못했습니다") != std::u8string::npos);
        CHECK(test.process.calls().empty());
    }

    SECTION("ssh-keygen is not installed")
    {
        ssh_harness test { false };
        const operation_id id { test.run(ssh_keygen_request { .key_path = u8"C:\\Users\\me\\.ssh\\id_new", .comment = u8"me@pc" }) };
        CHECK(test.status(id).state == operation_state::failed);
        CHECK(test.process.calls().empty());
    }
}

TEST_CASE("ssh-keygen runs without the passphrase on the command line and creates the key folder", "[ssh][operation]")
{
    const test::temporary_directory scratch { "keygen" };
    const std::filesystem::path key { scratch.path() / "home" / ".ssh" / "id_new" };
    ssh_harness test {};
    const operation_id id { test.run(ssh_keygen_request { .key_path = key.u8string(), .comment = u8"me@pc" }) };

    const operation_status& status { test.status(id) };
    CHECK(status.kind == operation_kind::ssh_keygen);
    CHECK(status.state == operation_state::succeeded);
    CHECK(status.target_path == key.u8string());
    CHECK(status.result_summary.find(u8"키를 만들었습니다") != std::u8string::npos);
    // 처음 키를 만드는 PC에는 ~/.ssh가 없다. ssh-keygen은 폴더를 만들지 않으므로 앱이 만든다.
    CHECK(std::filesystem::is_directory(key.parent_path()));

    const std::vector<test::recorded_call> calls { test.process.calls() };
    REQUIRE(calls.size() == 1);
    // passphrase를 주는 -N이 없다. 질문은 askpass가 받는다.
    CHECK(calls[0].arguments == test::text_list { "-t", "ed25519", "-C", "me@pc", "-f", key.generic_string() });
    CHECK(calls[0].variable(u8"SSH_ASKPASS_REQUIRE") == std::optional<std::string> { "force" });
    CHECK(calls[0].variable(u8"CYELLA_ASKPASS_OPERATION") == std::optional<std::string> { std::to_string(id) });
}

TEST_CASE("ssh-keygen failures say why and an existing key is refused before running", "[ssh][operation]")
{
    const test::temporary_directory scratch { "keygen-fail" };
    ssh_harness test {};

    SECTION("a failing run reports ssh-keygen's own message")
    {
        test.process.when({ "-t" }, test::scripted_response { .exit_code = 1, .standard_error_lines = { "Saving key failed: Permission denied" } });
        const operation_id id { test.run(ssh_keygen_request { .key_path = (scratch.path() / "k").u8string(), .comment = u8"c" }) };
        const operation_status& status { test.status(id) };
        CHECK(status.state == operation_state::failed);
        REQUIRE(status.failure.has_value());
        CHECK(status.failure->detail.find(u8"Permission denied") != std::u8string::npos);
    }

    SECTION("a key that already exists is not overwritten")
    {
        test::write_file(scratch.path() / "taken", "private");
        test::write_file(scratch.path() / "taken.pub", "public");
        const operation_id id { test.run(ssh_keygen_request { .key_path = (scratch.path() / "taken").u8string(), .comment = u8"c" }) };
        CHECK(test.status(id).state == operation_state::failed);
        CHECK(test.status(id).failure->summary.find(u8"이미 있습니다") != std::u8string::npos);
        CHECK(test.process.calls().empty());
        CHECK(test::read_file(scratch.path() / "taken") == "private");
    }

    SECTION("a relative key path is refused")
    {
        const operation_id id { test.run(ssh_keygen_request { .key_path = u8"relative\\key", .comment = u8"c" }) };
        CHECK(test.status(id).state == operation_state::failed);
        CHECK(test.process.calls().empty());
    }
}

// ---------------------------------------------------------------------------------------------- 읽기 도구

TEST_CASE("ssh tools are taken from Git for Windows and the agent from the system OpenSSH", "[ssh][info]")
{
    fake_probe probe {};
    const auto add_set = [&probe](const std::u8string& directory, const bool with_add) {
        probe.files.push_back(directory + u8"\\ssh.exe");
        probe.files.push_back(directory + u8"\\ssh-keygen.exe");
        if (with_add)
            probe.files.push_back(directory + u8"\\ssh-add.exe");
    };

    SECTION("both exist")
    {
        add_set(u8"C:\\Program Files\\Git\\usr\\bin", true);
        add_set(u8"C:\\Windows\\System32\\OpenSSH", true);
        const ssh_programs programs { locate_ssh_programs(u8"C:\\Program Files\\Git\\cmd\\git.exe", u8"C:\\Windows", probe) };
        CHECK(programs.found());
        CHECK(programs.origin == u8"Git for Windows");
        CHECK(programs.ssh == u8"C:\\Program Files\\Git\\usr\\bin\\ssh.exe");
        CHECK(programs.ssh_keygen == u8"C:\\Program Files\\Git\\usr\\bin\\ssh-keygen.exe");
        // Windows의 ssh-agent 서비스는 시스템 OpenSSH의 ssh-add만 닿는다.
        CHECK(programs.ssh_add == u8"C:\\Windows\\System32\\OpenSSH\\ssh-add.exe");
    }

    SECTION("git for windows only")
    {
        add_set(u8"C:\\Program Files\\Git\\usr\\bin", true);
        const ssh_programs programs { locate_ssh_programs(u8"C:\\Program Files\\Git\\mingw64\\bin\\git.exe", u8"C:\\Windows", probe) };
        CHECK(programs.found());
        CHECK(programs.ssh_add == u8"C:\\Program Files\\Git\\usr\\bin\\ssh-add.exe");
    }

    SECTION("system openssh only")
    {
        add_set(u8"C:\\Windows\\System32\\OpenSSH", true);
        const ssh_programs programs { locate_ssh_programs(u8"D:\\tools\\git\\git.exe", u8"C:\\Windows", probe) };
        CHECK(programs.found());
        CHECK(programs.origin == u8"Windows OpenSSH");
    }

    SECTION("nothing")
    {
        const ssh_programs programs { locate_ssh_programs(u8"D:\\tools\\git\\git.exe", u8"C:\\Windows", probe) };
        CHECK_FALSE(programs.found());
        CHECK(programs.ssh_add.empty());
    }
}

namespace {
    // `ssh-add -l`처럼 stdout 줄을 레코드로 내는 가짜 프로세스다.
    class scripted_process final : public process_runner
    {
    public:
        [[nodiscard]] process_result run(const process_request& request, process_output_sink* const sink, const process_cancellation_token&) noexcept override
        {
            requests.push_back(request);
            process_result result {};
            result.completion = completion;
            if (completion == process_completion::exited)
                result.exit_code = exit_code;
            if (sink != nullptr)
            {
                std::uint64_t sequence { 0 };
                for (const std::u8string& line : lines)
                {
                    process_output_record record {};
                    record.sequence = sequence++;
                    record.stream = process_stream::standard_output;
                    record.text = line;
                    sink->on_record(record);
                }
            }
            return result;
        }

        std::vector<process_request> requests {};
        std::vector<std::u8string> lines {};
        std::int32_t exit_code { 0 };
        process_completion completion { process_completion::exited };
    };

    ssh_info_sources make_sources(scripted_process& runner, const std::filesystem::path& directory)
    {
        ssh_info_sources sources {};
        sources.service_status = [] { return ssh_agent_service_status::running; };
        sources.runner = &runner;
        sources.ssh_add_executable = u8"C:\\Windows\\System32\\OpenSSH\\ssh-add.exe";
        sources.ssh_directory = directory;
        return sources;
    }
} // namespace

TEST_CASE("collecting ssh info reads the service, the agent's keys and the key pairs", "[ssh][info]")
{
    const test::temporary_directory scratch { "ssh-info" };
    test::write_file(scratch.path() / "id_ed25519", "PRIVATE");
    test::write_file(scratch.path() / "id_ed25519.pub", "ssh-ed25519 AAAAC3Nza me@pc\r\n");
    test::write_file(scratch.path() / "known_hosts", "github.com ssh-ed25519 AAAA");
    scripted_process runner {};
    runner.lines = { u8"256 SHA256:Zk3lQ me@pc (ED25519)" };

    const ssh_info_snapshot info { collect_ssh_info(make_sources(runner, scratch.path())) };
    CHECK(info.service == ssh_agent_service_status::running);
    CHECK(info.agent_list_available);
    CHECK(info.agent.state == ssh_agent_list_state::identities);
    REQUIRE(info.agent.identities.size() == 1);
    CHECK(info.agent.identities[0].fingerprint == u8"SHA256:Zk3lQ");

    // 키 쌍만 나온다 (known_hosts 같은 파일은 빠진다). 공개 키 한 줄은 줄 끝 문자 없이 복사용으로 남는다.
    REQUIRE(info.keys.size() == 1);
    CHECK(info.keys[0].name == u8"id_ed25519");
    CHECK(info.keys[0].key_type == u8"ssh-ed25519");
    CHECK(info.keys[0].comment == u8"me@pc");
    CHECK(info.keys[0].public_key_line == u8"ssh-ed25519 AAAAC3Nza me@pc");
    CHECK(info.ssh_directory == scratch.path().u8string());

    // 프로세스 요청은 절대 경로의 작업 디렉터리를 요구한다.
    REQUIRE(runner.requests.size() == 1);
    CHECK(runner.requests[0].working_directory == u8"C:\\Windows\\System32\\OpenSSH");
    CHECK(runner.requests[0].arguments == std::vector<std::u8string> { u8"-l" });
}

TEST_CASE("an unreachable agent and a missing ssh-add are told apart", "[ssh][info]")
{
    const test::temporary_directory scratch { "ssh-info-agent" };
    scripted_process runner {};

    SECTION("exit code 2 means the agent is not reachable")
    {
        runner.exit_code = 2;
        const ssh_info_snapshot info { collect_ssh_info(make_sources(runner, scratch.path())) };
        CHECK(info.agent.state == ssh_agent_list_state::agent_unavailable);
        CHECK(info.keys.empty());
    }

    SECTION("exit code 1 means no keys are loaded")
    {
        runner.exit_code = 1;
        CHECK(collect_ssh_info(make_sources(runner, scratch.path())).agent.state == ssh_agent_list_state::no_identities);
    }

    SECTION("ssh-add could not start")
    {
        runner.completion = process_completion::start_failed;
        const ssh_info_snapshot info { collect_ssh_info(make_sources(runner, scratch.path())) };
        CHECK(info.agent_list_available);
        CHECK(info.agent.state == ssh_agent_list_state::failed);
    }

    SECTION("no ssh-add at all")
    {
        ssh_info_sources sources { make_sources(runner, scratch.path()) };
        sources.ssh_add_executable.clear();
        const ssh_info_snapshot info { collect_ssh_info(sources) };
        CHECK_FALSE(info.agent_list_available);
        CHECK(runner.requests.empty());
    }

    SECTION("a missing key folder is an empty list")
    {
        ssh_info_sources sources { make_sources(runner, scratch.path() / "nope") };
        CHECK(collect_ssh_info(sources).keys.empty());
    }
}

namespace {
    // 서버에 저장소를 만드는 일은 저장소의 쓰기 lane에서 돈다. workspace 없이 실행기에 job을 바로 넣는다.
    struct repository_ssh_harness
    {
        repository_ssh_harness()
            : runner { process, u8"C:/Git/cmd/git.exe", files, ssh_harness::make_options(true) }
        {}

        operation_finished execute(create_ssh_repository_request request, std::shared_ptr<std::atomic<bool>> prompt_cancelled = {})
        {
            git_operation_job job {};
            job.id = 9;
            job.repository_id = "repo";
            job.repository_path = u8"C:/work/repo";
            job.operation = std::move(request);
            job.generation = 1;
            job.prompt_cancelled = std::move(prompt_cancelled);
            std::vector<operation_event> events {};
            runner.execute(job, [&events](operation_event event) { events.push_back(std::move(event)); });
            REQUIRE(events.empty() == false);
            const auto* const finished { std::get_if<operation_finished>(&events.back().payload) };
            REQUIRE(finished != nullptr);
            return *finished;
        }

        test::fake_git_runner process {};
        test::fake_workspace_files files {};
        git_operation_runner runner;
    };
} // namespace

TEST_CASE("creating the repository on an ssh server runs one ssh command and reports what it found", "[ssh][operation][repository]")
{
    repository_ssh_harness test {};
    SECTION("created")
    {
        test.process.when({ "-T" }, test::scripted_response { .exit_code = 0, .standard_output = "cyella:created\n" });
        const operation_finished finished { test.execute(create_ssh_repository_request { .url = "ssh://me@git.example.org:2222/srv/git/app.git" }) };
        CHECK(finished.state == operation_state::succeeded);
        CHECK(finished.summary.find(u8"me@git.example.org:/srv/git/app.git") != std::u8string::npos);
        const std::vector<test::recorded_call> calls { test.process.calls() };
        REQUIRE(calls.size() == 1);
        REQUIRE(calls[0].arguments.size() == 5);
        CHECK(calls[0].arguments[1] == "-p");
        CHECK(calls[0].arguments[2] == "2222");
        CHECK(calls[0].arguments[3] == "me@git.example.org");
        CHECK(calls[0].arguments[4].find("p='/srv/git/app.git'") != std::string::npos);
        CHECK(calls[0].working_directory == "C:/work/repo");
        CHECK(calls[0].variable(u8"CYELLA_ASKPASS_OPERATION") == std::optional<std::string> { "9" });
    }

    SECTION("an existing repository is used as is")
    {
        test.process.when({ "-T" }, test::scripted_response { .exit_code = 0, .standard_output = "cyella:existing\n" });
        const operation_finished finished { test.execute(create_ssh_repository_request { .url = "me@host:app.git" }) };
        CHECK(finished.state == operation_state::succeeded);
        CHECK(finished.summary.find(u8"이미 있는 저장소") != std::u8string::npos);
    }

    SECTION("something else at the path is left alone")
    {
        test.process.when({ "-T" }, test::scripted_response { .exit_code = 3, .standard_output = "cyella:not-repository\n" });
        const operation_finished finished { test.execute(create_ssh_repository_request { .url = "me@host:app.git" }) };
        CHECK(finished.state == operation_state::failed);
        REQUIRE(finished.failure.has_value());
        CHECK(finished.failure->summary.find(u8"저장소가 아닌") != std::u8string::npos);
    }

    SECTION("git is missing on the server")
    {
        test.process.when({ "-T" }, test::scripted_response { .exit_code = 127, .standard_output = "cyella:no-git\n" });
        const operation_finished finished { test.execute(create_ssh_repository_request { .url = "me@host:app.git" }) };
        REQUIRE(finished.failure.has_value());
        CHECK(finished.failure->summary.find(u8"git을 찾지 못했습니다") != std::u8string::npos);
    }

    SECTION("ssh failures use the same kinds as a push")
    {
        test.process.when({ "-T" }, test::scripted_response { .exit_code = 255, .standard_error_lines = { "me@host: Permission denied (publickey)." } });
        const operation_finished finished { test.execute(create_ssh_repository_request { .url = "me@host:app.git" }) };
        REQUIRE(finished.failure.has_value());
        CHECK(finished.failure->kind == failure_kind::auth_failed);
    }

    SECTION("closing the host key question is a cancellation")
    {
        test.process.when({ "-T" }, test::scripted_response { .exit_code = 255, .standard_error_lines = { "Host key verification failed." } });
        const operation_finished finished { test.execute(create_ssh_repository_request { .url = "me@host:app.git" }, std::make_shared<std::atomic<bool>>(true)) };
        REQUIRE(finished.failure.has_value());
        CHECK(finished.failure->kind == failure_kind::auth_cancelled);
    }

    SECTION("a restricted shell that refuses the command says so")
    {
        test.process.when({ "-T" }, test::scripted_response { .exit_code = 128, .standard_error_lines = { "fatal: unrecognized command 'command -v git'" } });
        const operation_finished finished { test.execute(create_ssh_repository_request { .url = "me@host:app.git" }) };
        REQUIRE(finished.failure.has_value());
        CHECK(finished.failure->kind == failure_kind::other);
        CHECK(finished.failure->summary.find(u8"git-shell") != std::u8string::npos);
        CHECK(finished.failure->detail.find(u8"unrecognized command") != std::u8string::npos);
    }
}

TEST_CASE("repository creation trusts a host once through a temporary known_hosts and deletes it afterwards", "[ssh][operation][repository]")
{
    repository_ssh_harness test {};
    test.process.when({ "-T" }, test::scripted_response { .exit_code = 0, .standard_output = "cyella:created\n" });
    create_ssh_repository_request request { .url = "me@host:app.git" };
    request.ssh_command_override = temporary_ssh_trust { .command = u8"ssh -o UserKnownHostsFile=C:/Temp/kh.tmp", .known_hosts_path = u8"C:/Temp/kh.tmp" };
    test.files.put(u8"C:/Temp/kh.tmp", "host ssh-ed25519 AAAA\n");
    CHECK(test.execute(std::move(request)).state == operation_state::succeeded);
    const std::vector<test::recorded_call> calls { test.process.calls() };
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].arguments[1] == "-o");
    CHECK(calls[0].arguments[2] == "UserKnownHostsFile=C:/Temp/kh.tmp");
    CHECK_FALSE(test.files.content(u8"C:/Temp/kh.tmp").has_value());
}

TEST_CASE("an unreadable URL or missing ssh fails repository creation without running anything", "[ssh][operation][repository]")
{
    repository_ssh_harness test {};
    const operation_finished https { test.execute(create_ssh_repository_request { .url = "https://host/app.git" }) };
    REQUIRE(https.failure.has_value());
    CHECK(https.failure->kind == failure_kind::invalid_request);
    const operation_finished injected { test.execute(create_ssh_repository_request { .url = "-oProxyCommand=calc:app.git" }) };
    CHECK(injected.failure->kind == failure_kind::invalid_request);
    CHECK(test.process.calls().empty());
}
