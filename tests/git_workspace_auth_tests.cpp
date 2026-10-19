// 인증 질문이 operation 상태에 어떻게 반영되는지 가짜 runner와 가짜 askpass 서비스로 검증한다.
// 질문 사건(`operation_input_requested`)은 test가 직접 넣는다. 실제 서버와 git은 쓰지 않는다.
#include "application/git_workspace.h"
#include "domain/credential_prompt.h"
#include "infrastructure/git_operation_runner.h"

#include "fake_git_runner.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace {
    using namespace cyella;
    using cyella::test::scripted_response;

    constexpr std::u8string_view secret_answer { u8"hunter2-never-log-this" };

    // askpass 도우미가 환경 변수로 받는 operation id 문자열이다.
    std::u8string operation_text(const operation_id id)
    {
        const std::string digits { std::to_string(id) };
        return std::u8string { digits.begin(), digits.end() };
    }

    // `cancel_operation`과 `shutdown` 호출을 기록하는 서비스다.
    class fake_askpass_service final : public askpass_service
    {
    public:
        [[nodiscard]] askpass_endpoint endpoint() const override
        {
            return { u8"\\\\.\\pipe\\cyella-askpass-test", u8"test-token" };
        }

        void cancel_operation(const std::u8string_view operation_id) override
        {
            cancelled_operations.emplace_back(operation_id);
        }

        void shutdown() override
        {
            ++shutdown_calls;
        }

        std::vector<std::u8string> cancelled_operations {};
        int shutdown_calls { 0 };
    };

    // 답 callback이다. 서버와 달리 첫 호출만 남기지 않고 모든 호출을 센다. 일회용 규칙이 서버 몫이어서,
    // workspace가 두 번 부르는 실수를 여기서 잡으려는 것이다.
    struct reply_recorder
    {
        [[nodiscard]] askpass_reply_callback callback()
        {
            return [this](askpass_reply reply) { replies.push_back(std::move(reply)); };
        }

        std::vector<askpass_reply> replies {};
    };

    struct auth_harness
    {
        auth_harness()
            : runner { process, u8"C:/Git/cmd/git.exe", files, make_runner_options() }
            , workspace { runner, dispatcher.dispatcher(), events.sink(), nullptr, make_workspace_options(&service) }
        {
            process.when({ "rev-parse", "--absolute-git-dir" }, scripted_response { 0, "C:/work/repo/.git\nC:/work/repo\n" });
            repository = workspace.open_repository(u8"C:/work/repo");
        }

        static git_operation_runner_options make_runner_options()
        {
            git_operation_runner_options options {};
            options.askpass_helper = u8"C:/app/bin/cyella-askpass.exe";
            options.askpass = askpass_endpoint { u8"\\\\.\\pipe\\cyella-askpass-test", u8"test-token" };
            return options;
        }

        static git_workspace_options make_workspace_options(askpass_service* const service)
        {
            git_workspace_options options {};
            options.refresh_on_open = false;
            options.askpass = service;
            return options;
        }

        // fetch를 내고 worker가 git을 실행 중인 상태로 만든다. job은 실행하지 않는다.
        operation_id start_fetch()
        {
            const operation_id id { workspace.submit(repository, fetch_request {}) };
            REQUIRE(id != 0);
            workspace.apply(operation_event { id, repository, operation_started {} });
            return id;
        }

        // 다음 fetch의 가짜 응답이다. `during_fetch`는 git이 도는 동안(worker 쪽) 일어나는 일을 test가 끼워 넣는 자리다.
        void script_fetch(const std::int32_t exit_code, std::vector<std::string> standard_error_lines, std::function<void()> during_fetch)
        {
            scripted_response response {};
            response.exit_code = exit_code;
            response.standard_error_lines = std::move(standard_error_lines);
            response.before_reply = std::move(during_fetch);
            process.when({ "fetch" }, std::move(response));
        }

        // worker가 보낸 사건을 반영한다. 실제로는 relay가 `operation_started`를 질문보다 먼저 넘긴다.
        void apply_pending_events()
        {
            for (const operation_event& event : events.take())
                workspace.apply(event);
        }

        void ask(const operation_id id, const std::uint64_t prompt_id, const std::u8string_view text, askpass_reply_callback reply)
        {
            workspace.apply(operation_event { id, {}, operation_input_requested { prompt_id, classify_credential_prompt(text), std::move(reply) } });
        }

        void finish(const operation_id id, const operation_state state = operation_state::succeeded)
        {
            workspace.apply(operation_event { id, repository, operation_finished { state, std::nullopt, std::nullopt, {} } });
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
        fake_askpass_service service {};
        git_operation_runner runner;
        git_workspace workspace;
        std::string repository {};
    };

    // 상태에 담긴 모든 문자열을 이어 붙인다. 답이 어디에도 새지 않았는지 찾는 용도다.
    std::string everything_in(const operation_status& status)
    {
        std::string text {};
        const auto add = [&text](const std::u8string_view part) { text.append(part.begin(), part.end()); };
        add(status.target_path);
        add(status.progress.phase_text);
        add(status.result_summary);
        for (const operation_command& command : status.commands)
            add(command.command_line);
        if (status.failure.has_value())
        {
            add(status.failure->summary);
            add(status.failure->detail);
        }
        if (status.needs_input.has_value())
        {
            const credential_prompt& prompt { status.needs_input->prompt };
            add(prompt.raw_text);
            add(prompt.key_path);
            add(prompt.url);
            add(prompt.user_name);
            add(prompt.host);
            add(prompt.address);
            add(prompt.key_type);
            add(prompt.fingerprint);
        }
        return text;
    }
} // namespace

TEST_CASE("an https username prompt moves a running operation to needs_input credentials", "[workspace][auth]")
{
    auth_harness test {};
    const operation_id id { test.start_fetch() };
    reply_recorder reply {};

    test.ask(id, 7, u8"Username for 'https://example.invalid': ", reply.callback());

    const operation_status& status { test.status(id) };
    CHECK(status.state == operation_state::needs_input);
    // 도우미를 기다리는 동안 operation은 끝난 것이 아니고 취소할 수 있다.
    CHECK_FALSE(status.finished());
    CHECK(status.cancellable);
    REQUIRE(status.needs_input.has_value());
    CHECK(status.needs_input->kind == input_kind::credentials);
    CHECK(status.needs_input->prompt_id == 7);
    CHECK(status.needs_input->prompt.kind == credential_prompt_kind::https_username);
    CHECK(status.needs_input->prompt.url == u8"https://example.invalid");
    CHECK(status.needs_input->prompt.raw_text == u8"Username for 'https://example.invalid': ");
    CHECK_FALSE(status.needs_input->secret);
    CHECK_FALSE(status.needs_input->yes_no);
    CHECK(reply.replies.empty());
}

TEST_CASE("prompt kinds map to the dialog kind with the answer style", "[workspace][auth]")
{
    struct expectation
    {
        std::u8string_view text;
        input_kind kind;
        bool secret;
        bool yes_no;
    };
    const std::u8string_view host_key_text = u8"The authenticity of host 'github.com (20.200.245.247)' can't be established.\n"
                                             u8"ED25519 key fingerprint is SHA256:abc.\n"
                                             u8"Are you sure you want to continue connecting (yes/no/[fingerprint])? ";
    const std::vector<expectation> expectations {
        { u8"Enter passphrase for key 'C:/Users/x/.ssh/id_ed25519': ", input_kind::passphrase, true, false },
        { u8"Enter passphrase for \"g\" (empty for no passphrase): ", input_kind::passphrase, true, false },
        { u8"Enter same passphrase again: ", input_kind::passphrase, true, false },
        { host_key_text, input_kind::host_key, false, true },
        { u8"Password for 'https://alice@example.invalid': ", input_kind::credentials, true, false },
        { u8"git@example.invalid's password: ", input_kind::credentials, true, false },
        { u8"Something we do not recognize: ", input_kind::credentials, false, false },
    };

    for (const expectation& expected : expectations)
    {
        auth_harness test {};
        const operation_id id { test.start_fetch() };
        reply_recorder reply {};
        test.ask(id, 1, expected.text, reply.callback());

        const operation_status& status { test.status(id) };
        INFO(std::string(expected.text.begin(), expected.text.end()));
        REQUIRE(status.needs_input.has_value());
        CHECK(status.needs_input->kind == expected.kind);
        CHECK(status.needs_input->secret == expected.secret);
        CHECK(status.needs_input->yes_no == expected.yes_no);
    }
}

TEST_CASE("a host key prompt keeps the details the dialog shows", "[workspace][auth]")
{
    auth_harness test {};
    const operation_id id { test.start_fetch() };
    reply_recorder reply {};

    test.ask(id, 3,
        u8"The authenticity of host 'github.com (20.200.245.247)' can't be established.\n"
        u8"ED25519 key fingerprint is SHA256:+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU.\n"
        u8"Are you sure you want to continue connecting (yes/no/[fingerprint])? ",
        reply.callback());

    const operation_status& status { test.status(id) };
    REQUIRE(status.needs_input.has_value());
    CHECK(status.needs_input->kind == input_kind::host_key);
    CHECK(status.needs_input->prompt.host == u8"github.com");
    CHECK(status.needs_input->prompt.address == u8"20.200.245.247");
    CHECK(status.needs_input->prompt.key_type == u8"ED25519");
    CHECK(status.needs_input->prompt.fingerprint == u8"SHA256:+DiY3wvvV6TuJJhbpZisF/zLDA0zPMSvHdkr4UvCOqU");
    CHECK(status.needs_input->yes_no);
}

TEST_CASE("an answer resumes the operation and reaches the reply once", "[workspace][auth]")
{
    auth_harness test {};
    const operation_id id { test.start_fetch() };
    reply_recorder reply {};
    test.ask(id, 7, u8"Username for 'https://example.invalid': ", reply.callback());

    REQUIRE(test.workspace.answer_input(id, 7, u8"alice"));

    const operation_status& status { test.status(id) };
    CHECK(status.state == operation_state::running);
    CHECK_FALSE(status.needs_input.has_value());
    REQUIRE(reply.replies.size() == 1);
    CHECK(reply.replies[0].answered);
    CHECK(reply.replies[0].answer == u8"alice");
}

TEST_CASE("an empty answer is an answer, not a cancellation", "[workspace][auth]")
{
    auth_harness test {};
    const operation_id id { test.start_fetch() };
    reply_recorder reply {};
    test.ask(id, 1, u8"Enter passphrase for \"g\" (empty for no passphrase): ", reply.callback());

    REQUIRE(test.workspace.answer_input(id, 1, {}));

    REQUIRE(reply.replies.size() == 1);
    CHECK(reply.replies[0].answered);
    CHECK(reply.replies[0].answer.empty());
}

TEST_CASE("a duplicate, late or mismatched answer does nothing", "[workspace][auth]")
{
    auth_harness test {};
    const operation_id id { test.start_fetch() };
    reply_recorder reply {};
    test.ask(id, 7, u8"Username for 'https://example.invalid': ", reply.callback());

    CHECK_FALSE(test.workspace.answer_input(id, 8, u8"wrong-prompt"));
    CHECK(test.status(id).state == operation_state::needs_input);
    CHECK(reply.replies.empty());

    REQUIRE(test.workspace.answer_input(id, 7, u8"alice"));
    CHECK_FALSE(test.workspace.answer_input(id, 7, u8"alice-again"));
    CHECK_FALSE(test.workspace.cancel_input(id, 7));
    CHECK(reply.replies.size() == 1);
    CHECK(test.status(id).state == operation_state::running);

    test.finish(id);
    CHECK_FALSE(test.workspace.answer_input(id, 7, u8"too-late"));
    CHECK_FALSE(test.workspace.answer_input(9999, 7, u8"unknown-operation"));
    CHECK(reply.replies.size() == 1);
}

TEST_CASE("cancelling the prompt makes the authentication failure auth_cancelled", "[workspace][auth]")
{
    auth_harness test {};
    reply_recorder reply {};
    operation_id id { 0 };
    // git이 도는 동안(worker 쪽) 질문이 오고 사용자가 닫는 순서를 fetch 응답 직전에 재현한다.
    const auto during_fetch = [&test, &reply, &id]() {
        test.apply_pending_events();
        test.ask(id, 5, u8"Username for 'https://example.invalid': ", reply.callback());
        REQUIRE(test.workspace.cancel_input(id, 5));
    };
    test.script_fetch(128, { "fatal: could not read Username for 'https://example.invalid': terminal prompts disabled" }, during_fetch);
    id = test.workspace.submit(test.repository, fetch_request {});
    REQUIRE(id != 0);

    REQUIRE(test.dispatcher.run());
    for (const operation_event& event : test.events.take())
        test.workspace.apply(event);

    const operation_status& status { test.status(id) };
    CHECK(status.state == operation_state::failed);
    REQUIRE(status.failure.has_value());
    CHECK(status.failure->kind == failure_kind::auth_cancelled);
    REQUIRE(reply.replies.size() == 1);
    CHECK_FALSE(reply.replies[0].answered);
    // 같은 키로 다시 묻는 ssh가 대화 상자를 또 띄우지 않게 서버에도 알린다.
    REQUIRE_FALSE(test.service.cancelled_operations.empty());
    CHECK(test.service.cancelled_operations.front() == operation_text(id));
}

TEST_CASE("an authentication failure without a cancelled prompt stays auth_failed", "[workspace][auth]")
{
    auth_harness test {};
    reply_recorder reply {};
    operation_id id { 0 };
    const auto during_fetch = [&test, &reply, &id]() {
        test.apply_pending_events();
        test.ask(id, 5, u8"Username for 'https://example.invalid': ", reply.callback());
        REQUIRE(test.workspace.answer_input(id, 5, u8"alice"));
    };
    test.script_fetch(128, { "fatal: Authentication failed for 'https://example.invalid/repo.git/'" }, during_fetch);
    id = test.workspace.submit(test.repository, fetch_request {});

    REQUIRE(test.dispatcher.run());
    for (const operation_event& event : test.events.take())
        test.workspace.apply(event);

    const operation_status& status { test.status(id) };
    REQUIRE(status.failure.has_value());
    CHECK(status.failure->kind == failure_kind::auth_failed);
}

TEST_CASE("closing a host key prompt is auth_cancelled but answering no is host_key_refused", "[workspace][auth]")
{
    const std::u8string_view host_key_text = u8"The authenticity of host 'github.com (20.200.245.247)' can't be established.\n"
                                             u8"ED25519 key fingerprint is SHA256:abc.\n"
                                             u8"Are you sure you want to continue connecting (yes/no/[fingerprint])? ";
    for (const bool closed : { true, false })
    {
        auth_harness test {};
        reply_recorder reply {};
        operation_id id { 0 };
        const auto during_fetch = [&test, &reply, &id, closed, host_key_text]() {
            test.apply_pending_events();
            test.ask(id, 3, host_key_text, reply.callback());
            if (closed)
                REQUIRE(test.workspace.cancel_input(id, 3));
            else
                REQUIRE(test.workspace.answer_input(id, 3, u8"no"));
        };
        test.script_fetch(128, { "Host key verification failed.", "fatal: Could not read from remote repository." }, during_fetch);
        id = test.workspace.submit(test.repository, fetch_request {});

        REQUIRE(test.dispatcher.run());
        for (const operation_event& event : test.events.take())
            test.workspace.apply(event);

        const operation_status& status { test.status(id) };
        REQUIRE(status.failure.has_value());
        CHECK(status.failure->kind == (closed ? failure_kind::auth_cancelled : failure_kind::host_key_refused));
    }
}

TEST_CASE("cancelling the operation releases a pending prompt", "[workspace][auth]")
{
    auth_harness test {};
    const operation_id id { test.start_fetch() };
    reply_recorder reply {};
    test.ask(id, 7, u8"Enter passphrase for key 'C:/k': ", reply.callback());
    REQUIRE(test.status(id).state == operation_state::needs_input);

    REQUIRE(test.workspace.cancel(id));

    REQUIRE(reply.replies.size() == 1);
    CHECK_FALSE(reply.replies[0].answered);
    const operation_status& status { test.status(id) };
    CHECK(status.cancel_requested);
    // 대화 상자는 닫혔고, git이 끝나 `cancelled`가 올 때까지 실행 중으로 보인다.
    CHECK(status.state == operation_state::running);
    CHECK_FALSE(status.needs_input.has_value());
    const std::u8string digits { operation_text(id) };
    REQUIRE_FALSE(test.service.cancelled_operations.empty());
    CHECK(test.service.cancelled_operations.front() == digits);
    // 취소가 걸린 뒤 도착한 질문은 대화 상자 없이 바로 취소된다.
    reply_recorder late {};
    test.ask(id, 8, u8"Enter passphrase for key 'C:/k': ", late.callback());
    REQUIRE(late.replies.size() == 1);
    CHECK_FALSE(late.replies[0].answered);
    CHECK(test.status(id).state == operation_state::running);
}

TEST_CASE("finishing releases every pending prompt and tells the server", "[workspace][auth]")
{
    auth_harness test {};
    const operation_id id { test.start_fetch() };
    reply_recorder reply {};
    test.ask(id, 7, u8"Enter passphrase for key 'C:/k': ", reply.callback());

    test.finish(id, operation_state::failed);

    REQUIRE(reply.replies.size() == 1);
    CHECK_FALSE(reply.replies[0].answered);
    CHECK(test.status(id).state == operation_state::failed);
    CHECK_FALSE(test.service.cancelled_operations.empty());
}

TEST_CASE("a prompt for an unknown or finished operation is cancelled so the helper is not left waiting", "[workspace][auth]")
{
    auth_harness test {};
    reply_recorder unknown {};
    test.ask(4242, 1, u8"Username for 'https://example.invalid': ", unknown.callback());
    REQUIRE(unknown.replies.size() == 1);
    CHECK_FALSE(unknown.replies[0].answered);

    const operation_id id { test.start_fetch() };
    test.finish(id);
    reply_recorder finished {};
    test.ask(id, 1, u8"Username for 'https://example.invalid': ", finished.callback());
    REQUIRE(finished.replies.size() == 1);
    CHECK_FALSE(finished.replies[0].answered);
    CHECK(test.status(id).state == operation_state::succeeded);

    // 아직 시작하지 않은 operation의 질문도 받지 않는다.
    const operation_id queued { test.workspace.submit(test.repository, fetch_request {}) };
    reply_recorder early {};
    test.ask(queued, 1, u8"Username for 'https://example.invalid': ", early.callback());
    REQUIRE(early.replies.size() == 1);
    CHECK(test.status(queued).state == operation_state::queued);
}

TEST_CASE("a second prompt on the same operation shows after the first is answered", "[workspace][auth]")
{
    // `ssh-keygen`의 확인 문구(`Enter same passphrase again:`)는 같은 operation의 둘째 질문이다.
    auth_harness test {};
    const operation_id id { test.start_fetch() };
    reply_recorder first {};
    reply_recorder second {};
    test.ask(id, 1, u8"Enter passphrase (empty for no passphrase): ", first.callback());
    test.ask(id, 2, u8"Enter same passphrase again: ", second.callback());

    REQUIRE(test.status(id).needs_input.has_value());
    CHECK(test.status(id).needs_input->prompt_id == 1);
    // 보이지 않는 질문에는 답할 수 없다.
    CHECK_FALSE(test.workspace.answer_input(id, 2, u8"early"));

    REQUIRE(test.workspace.answer_input(id, 1, u8"pass"));
    CHECK(test.status(id).state == operation_state::needs_input);
    REQUIRE(test.status(id).needs_input.has_value());
    CHECK(test.status(id).needs_input->prompt_id == 2);
    CHECK(test.status(id).needs_input->prompt.kind == credential_prompt_kind::ssh_new_key_passphrase_confirm);

    REQUIRE(test.workspace.answer_input(id, 2, u8"pass"));
    CHECK(test.status(id).state == operation_state::running);
    CHECK(first.replies.size() == 1);
    CHECK(second.replies.size() == 1);
}

TEST_CASE("cancelling one prompt cancels the queued ones too", "[workspace][auth]")
{
    auth_harness test {};
    const operation_id id { test.start_fetch() };
    reply_recorder first {};
    reply_recorder second {};
    test.ask(id, 1, u8"Username for 'https://example.invalid': ", first.callback());
    test.ask(id, 2, u8"Username for 'https://other.invalid': ", second.callback());

    REQUIRE(test.workspace.cancel_input(id, 1));

    CHECK(first.replies.size() == 1);
    CHECK(second.replies.size() == 1);
    CHECK_FALSE(second.replies[0].answered);
    CHECK(test.status(id).state == operation_state::running);

    // ssh가 같은 키로 다시 물어도 대화 상자를 또 띄우지 않고 바로 취소로 답한다.
    reply_recorder retry {};
    test.ask(id, 3, u8"Username for 'https://example.invalid': ", retry.callback());
    REQUIRE(retry.replies.size() == 1);
    CHECK_FALSE(retry.replies[0].answered);
    CHECK(test.status(id).state == operation_state::running);
    CHECK_FALSE(test.status(id).needs_input.has_value());
}

TEST_CASE("the answer text never appears in the operation status, commands or git command lines", "[workspace][auth]")
{
    auth_harness test {};
    reply_recorder reply {};
    operation_id id { 0 };
    const auto during_fetch = [&test, &reply, &id]() {
        test.apply_pending_events();
        test.ask(id, 1, u8"Password for 'https://alice@example.invalid': ", reply.callback());
        // 대기 중 상태에도 답은 없다.
        REQUIRE(test.workspace.answer_input(id, 1, std::u8string { secret_answer }));
    };
    test.script_fetch(0, {}, during_fetch);
    id = test.workspace.submit(test.repository, fetch_request {});

    REQUIRE(test.dispatcher.run());
    for (const operation_event& event : test.events.take())
        test.workspace.apply(event);

    const operation_status& status { test.status(id) };
    CHECK(status.state == operation_state::succeeded);
    REQUIRE(reply.replies.size() == 1);
    CHECK(reply.replies[0].answer == secret_answer);

    const std::string needle { secret_answer.begin(), secret_answer.end() };
    CHECK(everything_in(status).find(needle) == std::string::npos);
    CHECK_FALSE(status.commands.empty());
    for (const operation_id other : test.workspace.operation_order())
        CHECK(everything_in(test.status(other)).find(needle) == std::string::npos);
    // git에 건넨 명령줄과 환경에도 답이 없다. 답은 pipe로만 간다.
    for (const test::recorded_call& call : test.process.calls())
    {
        for (const std::string& argument : call.arguments)
            CHECK(argument.find(needle) == std::string::npos);
        CHECK(call.standard_input.find(needle) == std::string::npos);
        for (const process_environment_override& entry : call.environment)
            if (entry.value.has_value())
                CHECK(std::string(entry.value->begin(), entry.value->end()).find(needle) == std::string::npos);
    }
}

TEST_CASE("wipe_secret_text zeroes the buffer before clearing it", "[workspace][auth]")
{
    std::u8string text { u8"short-secret" };
    const char8_t* const data { text.data() };
    const std::size_t size { text.size() };

    wipe_secret_text(text);

    CHECK(text.empty());
    // 작은 문자열은 객체 안에 있어 지운 뒤에도 같은 주소를 읽을 수 있다. 내용이 0이어야 한다.
    for (std::size_t index { 0 }; index < size; ++index)
        CHECK(data[index] == u8'\0');
}

TEST_CASE("every git command of an operation gets the askpass session with the operation id", "[workspace][auth]")
{
    auth_harness test {};
    const operation_id id { test.workspace.submit(test.repository, fetch_request {}) };
    REQUIRE(test.dispatcher.run());
    for (const operation_event& event : test.events.take())
        test.workspace.apply(event);

    const std::vector<test::recorded_call> calls { test.process.calls() };
    REQUIRE_FALSE(calls.empty());
    const std::string digits { std::to_string(id) };
    for (const test::recorded_call& call : calls)
    {
        CHECK(call.variable(u8"CYELLA_ASKPASS_OPERATION") == digits);
        CHECK(call.variable(u8"CYELLA_ASKPASS_PIPE") == std::string { "\\\\.\\pipe\\cyella-askpass-test" });
        CHECK(call.variable(u8"CYELLA_ASKPASS_TOKEN") == std::string { "test-token" });
        CHECK(call.variable(u8"GIT_ASKPASS") == std::string { "C:/app/bin/cyella-askpass.exe" });
        CHECK(call.variable(u8"SSH_ASKPASS_REQUIRE") == std::string { "force" });
        CHECK_FALSE(call.variable(u8"GIT_SSH_COMMAND").has_value());
    }
}

TEST_CASE("without a helper the askpass variables are cleared and no session is passed", "[workspace][auth]")
{
    test::fake_git_runner process {};
    test::fake_workspace_files files {};
    test::manual_dispatcher dispatcher {};
    test::event_queue events {};
    git_operation_runner runner { process, u8"C:/Git/cmd/git.exe", files };
    git_workspace_options options {};
    options.refresh_on_open = false;
    git_workspace workspace { runner, dispatcher.dispatcher(), events.sink(), nullptr, options };
    process.when({ "rev-parse", "--absolute-git-dir" }, scripted_response { 0, "C:/work/repo/.git\nC:/work/repo\n" });
    const std::string repository { workspace.open_repository(u8"C:/work/repo") };

    static_cast<void>(workspace.submit(repository, fetch_request {}));
    REQUIRE(dispatcher.run());

    for (const test::recorded_call& call : process.calls())
    {
        CHECK_FALSE(call.variable(u8"GIT_ASKPASS").has_value());
        CHECK_FALSE(call.variable(u8"CYELLA_ASKPASS_TOKEN").has_value());
        CHECK(call.variable(u8"GIT_TERMINAL_PROMPT") == std::string { "0" });
    }
}

TEST_CASE("connect this time only sets GIT_SSH_COMMAND for that operation and removes the temporary file", "[workspace][auth]")
{
    auth_harness test {};
    const std::u8string known_hosts { u8"C:/Temp/cyella-known-hosts-1" };
    test.files.put(known_hosts, "");
    temporary_ssh_trust trust {};
    trust.command = u8"'C:/Git/usr/bin/ssh.exe' -o 'UserKnownHostsFile=C:/Temp/cyella-known-hosts-1' -o 'StrictHostKeyChecking=accept-new'";
    trust.known_hosts_path = known_hosts;
    const operation_id id { test.workspace.submit(test.repository, fetch_request { "origin", false, true, trust }) };
    REQUIRE(id != 0);

    REQUIRE(test.dispatcher.run());
    for (const operation_event& event : test.events.take())
        test.workspace.apply(event);

    REQUIRE(test.status(id).state == operation_state::succeeded);
    const std::vector<test::recorded_call> calls { test.process.calls() };
    bool saw_fetch { false };
    for (const test::recorded_call& call : calls)
    {
        if (call.arguments.empty() || call.arguments.front() != "fetch")
            continue;
        saw_fetch = true;
        REQUIRE(call.variable(u8"GIT_SSH_COMMAND").has_value());
        CHECK(call.variable(u8"GIT_SSH_COMMAND")->find("StrictHostKeyChecking=accept-new") != std::string::npos);
    }
    CHECK(saw_fetch);
    CHECK_FALSE(test.files.content(known_hosts).has_value());

    // 다음 operation(자동 refresh 등)은 덮어쓰지 않는다.
    test.process.when({ "status" }, scripted_response { 0, test::status_header(test::sha_of(1), "main") });
    for (int round { 0 }; round < 8 && test.dispatcher.run(); ++round)
    {
    }
    for (const test::recorded_call& call : test.process.calls())
        if (call.arguments.empty() == false && call.arguments.front() != "fetch")
            CHECK_FALSE(call.variable(u8"GIT_SSH_COMMAND").has_value());
}

TEST_CASE("the temporary known_hosts file is removed even when the operation was cancelled before it ran", "[workspace][auth]")
{
    auth_harness test {};
    const std::u8string known_hosts { u8"C:/Temp/cyella-known-hosts-2" };
    test.files.put(known_hosts, "");
    temporary_ssh_trust trust {};
    trust.command = u8"ssh";
    trust.known_hosts_path = known_hosts;
    const operation_id id { test.workspace.submit(test.repository, pull_request { pull_strategy::ff_only, trust }) };
    REQUIRE(test.workspace.cancel(id));

    // 취소된 job도 실행기를 거쳐 파일을 지운다.
    REQUIRE(test.dispatcher.run());

    CHECK_FALSE(test.files.content(known_hosts).has_value());
    CHECK(test.status(id).state == operation_state::cancelled);
}
