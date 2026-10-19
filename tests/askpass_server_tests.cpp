#include "askpass_test_support.h"
#include "platform/win32/askpass_helper_path.h"
#include "platform/win32/askpass_protocol.h"
#include "platform/win32/win32_askpass_server.h"
#include "platform/win32/win32_process_runner.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <aclapi.h>
#include <sddl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
    using namespace cyella;
    using namespace cyella::test;
    using namespace std::chrono_literals;

    // 질문을 받은 순서대로 보관하고 test가 원하는 때 답한다.
    class recording_handler
    {
    public:
        struct entry
        {
            askpass_prompt_request request {};
            askpass_reply_callback reply {};
        };

        [[nodiscard]] askpass_prompt_handler handler()
        {
            return [this](askpass_prompt_request request, askpass_reply_callback reply) {
                {
                    const std::lock_guard<std::mutex> lock { mutex_ };
                    entries_.push_back(entry { std::move(request), std::move(reply) });
                }
                condition_.notify_all();
            };
        }

        [[nodiscard]] bool wait_for_count(const std::size_t count, const std::chrono::milliseconds limit)
        {
            std::unique_lock<std::mutex> lock { mutex_ };
            return condition_.wait_for(lock, limit, [this, count]() { return entries_.size() >= count; });
        }

        [[nodiscard]] std::vector<entry> snapshot()
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            return entries_;
        }

        [[nodiscard]] std::size_t count()
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            return entries_.size();
        }

    private:
        std::mutex mutex_ {};
        std::condition_variable condition_ {};
        std::vector<entry> entries_ {};
    };

    askpass_prompt_handler answering(std::u8string answer)
    {
        return [answer = std::move(answer)](askpass_prompt_request, askpass_reply_callback reply) { reply(askpass_reply::with_answer(answer)); };
    }

    askpass_prompt_handler cancelling()
    {
        return [](askpass_prompt_request, askpass_reply_callback reply) { reply(askpass_reply::cancelled()); };
    }

    std::unique_ptr<askpass_service> start(askpass_prompt_handler handler)
    {
        win32::askpass_server_start_result started { win32::start_askpass_server(std::move(handler)) };
        REQUIRE(started.server != nullptr);
        return std::move(started.server);
    }

    // 도우미를 실행해 결과를 돌려준다. 종료 코드를 꺼내는 수고를 줄인다.
    helper_run run_prompt(process_runner& runner, const askpass_service& server, const std::u8string_view prompt, const std::u8string_view operation = u8"op-1")
    {
        return run_helper(runner, make_helper_request(prompt, session_for(server.endpoint(), operation)));
    }

    int exit_code_of(const helper_run& run)
    {
        REQUIRE(run.result.completion == process_completion::exited);
        REQUIRE(run.result.exit_code.has_value());
        return *run.result.exit_code;
    }
} // namespace

TEST_CASE("askpass protocol round trips a request and a reply", "[askpass][protocol]")
{
    const std::optional<std::string> frame { win32::encode_askpass_request({ "tok", "op", "Enter passphrase: " }) };
    REQUIRE(frame.has_value());
    REQUIRE(frame->size() > win32::askpass_request_header_bytes);

    const std::optional<win32::askpass_request_lengths> lengths { win32::decode_askpass_request_header(frame->substr(0, win32::askpass_request_header_bytes)) };
    REQUIRE(lengths.has_value());
    const std::optional<win32::askpass_request_message> message { win32::decode_askpass_request_body(*lengths, frame->substr(win32::askpass_request_header_bytes)) };
    REQUIRE(message.has_value());
    CHECK(message->token == "tok");
    CHECK(message->operation_id == "op");
    CHECK(message->prompt == "Enter passphrase: ");

    const std::string answered { win32::encode_askpass_reply(true, "pw") };
    const std::optional<win32::askpass_reply_header> header { win32::decode_askpass_reply_header(answered.substr(0, win32::askpass_reply_header_bytes)) };
    REQUIRE(header.has_value());
    CHECK(header->answered);
    CHECK(header->answer_bytes == 2);

    // 빈 답과 취소는 길이가 같아도 상태 byte로 구분된다.
    const std::optional<win32::askpass_reply_header> empty { win32::decode_askpass_reply_header(win32::encode_askpass_reply(true, "")) };
    REQUIRE(empty.has_value());
    CHECK(empty->answered);
    const std::optional<win32::askpass_reply_header> cancelled { win32::decode_askpass_reply_header(win32::encode_askpass_reply(false, "ignored")) };
    REQUIRE(cancelled.has_value());
    CHECK_FALSE(cancelled->answered);
    CHECK(cancelled->answer_bytes == 0);
}

TEST_CASE("askpass protocol rejects bad magic and oversized lengths", "[askpass][protocol]")
{
    std::string header { *win32::encode_askpass_request({ "t", "o", "p" }) };
    header.resize(win32::askpass_request_header_bytes);

    std::string bad_magic { header };
    bad_magic[0] = 'X';
    CHECK_FALSE(win32::decode_askpass_request_header(bad_magic).has_value());

    // 프롬프트 길이 필드를 상한 위로 올린다.
    std::string oversized { header };
    oversized[12] = '\xff';
    oversized[13] = '\xff';
    oversized[14] = '\xff';
    oversized[15] = '\x7f';
    CHECK_FALSE(win32::decode_askpass_request_header(oversized).has_value());
    CHECK_FALSE(win32::decode_askpass_request_header(header.substr(0, 8)).has_value());

    CHECK_FALSE(win32::encode_askpass_request({ std::string(win32::askpass_maximum_token_bytes + 1u, 'a'), "o", "p" }).has_value());
    CHECK_FALSE(win32::decode_askpass_reply_header("CYR1\x02\0\0\0\0").has_value());
    CHECK_FALSE(win32::decode_askpass_reply_header(std::string { "CYR1\0\x05\0\0\0", 9 }).has_value());
}

TEST_CASE("helper path is resolved beside the executable", "[askpass][path]")
{
    // 여기서는 경로 조립만 본다. 설치 규칙이 둘을 같은 디렉터리에 둔다.
    CHECK(win32::askpass_helper_path_beside("C:/cyella/bin").filename() == "cyella-askpass.exe");
    CHECK(win32::askpass_helper_path_beside("C:/cyella/bin").parent_path() == "C:/cyella/bin");
}

TEST_CASE("helper returns the answer on stdout and exits zero", "[askpass][server]")
{
    recording_handler recorder {};
    askpass_prompt_handler record_and_answer = [record = recorder.handler()](askpass_prompt_request request, askpass_reply_callback reply) {
        record(std::move(request), [](askpass_reply) {});
        reply(askpass_reply::with_answer(u8"s3cret"));
    };
    const std::unique_ptr<askpass_service> server { start(std::move(record_and_answer)) };
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };

    const helper_run run { run_prompt(*runner, *server, u8"Enter passphrase for key 'C:/Users/x/.ssh/id_ed25519': ", u8"op-42") };

    CHECK(exit_code_of(run) == 0);
    CHECK(run.standard_output == "s3cret\n");
    REQUIRE(recorder.wait_for_count(1, 1s));
    const auto entries { recorder.snapshot() };
    CHECK(entries[0].request.operation_id == u8"op-42");
    CHECK(entries[0].request.prompt.kind == credential_prompt_kind::ssh_key_passphrase);
    CHECK(entries[0].request.prompt.key_path == u8"C:/Users/x/.ssh/id_ed25519");
    CHECK(entries[0].request.prompt.raw_text == u8"Enter passphrase for key 'C:/Users/x/.ssh/id_ed25519': ");
}

TEST_CASE("helper carries non ascii prompts and answers unchanged", "[askpass][server]")
{
    std::u8string seen_prompt {};
    askpass_prompt_handler remember = [&seen_prompt](askpass_prompt_request request, askpass_reply_callback reply) {
        seen_prompt = request.prompt.raw_text;
        reply(askpass_reply::with_answer(u8"비밀번호 \u00e9\U0001F511"));
    };
    const std::unique_ptr<askpass_service> server { start(std::move(remember)) };
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };

    const helper_run run { run_prompt(*runner, *server, u8"Enter passphrase for key 'C:/Users/홍길동/.ssh/id': ") };

    CHECK(exit_code_of(run) == 0);
    CHECK(run.standard_output == to_string(u8"비밀번호 \u00e9\U0001F511\n"));
    CHECK(seen_prompt == u8"Enter passphrase for key 'C:/Users/홍길동/.ssh/id': ");
}

TEST_CASE("an empty answer is an answer and not a cancel", "[askpass][server]")
{
    const std::unique_ptr<askpass_service> server { start(answering(u8"")) };
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };

    const helper_run run { run_prompt(*runner, *server, u8"Enter passphrase (empty for no passphrase): ") };

    CHECK(exit_code_of(run) == 0);
    CHECK(run.standard_output == "\n");
}

TEST_CASE("a cancelled prompt makes the helper exit one and write nothing", "[askpass][server]")
{
    const std::unique_ptr<askpass_service> server { start(cancelling()) };
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };

    const helper_run run { run_prompt(*runner, *server, u8"Password for 'https://example.invalid': ") };

    CHECK(exit_code_of(run) == 1);
    CHECK(run.standard_output.empty());
}

TEST_CASE("a handler that throws cancels the prompt", "[askpass][server]")
{
    const std::unique_ptr<askpass_service> server { start([](askpass_prompt_request, askpass_reply_callback) { throw 42; }) };
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };

    const helper_run run { run_prompt(*runner, *server, u8"Username for 'https://example.invalid': ") };

    CHECK(exit_code_of(run) == 1);
    CHECK(run.standard_output.empty());
}

TEST_CASE("a wrong token is cancelled without reaching the handler", "[askpass][server]")
{
    std::atomic<int> calls { 0 };
    askpass_prompt_handler count_and_answer = [&calls](askpass_prompt_request, askpass_reply_callback reply) {
        ++calls;
        reply(askpass_reply::with_answer(u8"leaked"));
    };
    const std::unique_ptr<askpass_service> server { start(std::move(count_and_answer)) };
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };

    askpass_session session { session_for(server->endpoint(), u8"op-1") };
    session.token = u8"00000000000000000000000000000000";
    const helper_run run { run_helper(*runner, make_helper_request(u8"Enter passphrase: ", session)) };

    CHECK(exit_code_of(run) == 1);
    CHECK(run.standard_output.empty());
    CHECK(calls.load() == 0);
}

TEST_CASE("missing environment makes the helper exit one", "[askpass][server]")
{
    const std::unique_ptr<askpass_service> server { start(answering(u8"s3cret")) };
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    const askpass_session full { session_for(server->endpoint(), u8"op-1") };

    for (const std::u8string_view missing : { std::u8string_view { u8"CYELLA_ASKPASS_PIPE" }, std::u8string_view { u8"CYELLA_ASKPASS_TOKEN" }, std::u8string_view { u8"CYELLA_ASKPASS_OPERATION" } })
    {
        process_request request { make_helper_request(u8"Enter passphrase: ", full) };
        for (process_environment_override& entry : request.environment_overrides)
            if (entry.name == missing)
                entry.value.reset();

        const helper_run run { run_helper(*runner, request) };
        CHECK(exit_code_of(run) == 1);
        CHECK(run.standard_output.empty());
    }
}

TEST_CASE("helper exits one when no app is listening", "[askpass][server]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    askpass_session session {};
    session.pipe_name = u8"\\\\.\\pipe\\cyella-askpass-no-such-server";
    session.token = u8"abc";
    session.operation_id = u8"op";

    const helper_run run { run_helper(*runner, make_helper_request(u8"Enter passphrase: ", session)) };

    CHECK(exit_code_of(run) == 1);
    CHECK(run.standard_output.empty());
}

TEST_CASE("helper refuses a pipe name that is not a pipe path", "[askpass][server]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    askpass_session session {};
    session.pipe_name = u8"C:\\Windows\\win.ini";
    session.token = u8"abc";
    session.operation_id = u8"op";

    const helper_run run { run_helper(*runner, make_helper_request(u8"Enter passphrase: ", session)) };

    CHECK(exit_code_of(run) == 1);
    CHECK(run.standard_output.empty());
}

TEST_CASE("three helpers can wait for answers at the same time", "[askpass][server][concurrency]")
{
    recording_handler recorder {};
    const std::unique_ptr<askpass_service> server { start(recorder.handler()) };

    // 세 질문이 모두 서버에 도착해 동시에 대기 중이어야 답한다. 직렬로 서비스하면 두 번째가 오지 않아 시간 초과로 실패한다.
    std::vector<helper_run> runs(3);
    std::vector<std::thread> threads {};
    for (std::size_t index { 0 }; index < runs.size(); ++index)
    {
        threads.emplace_back([&runs, &server, index]() {
            const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
            const std::u8string prompt { u8"Enter passphrase for key 'key" + to_u8string(std::to_string(index)) + u8"': " };
            runs[index] = run_prompt(*runner, *server, prompt, u8"op-" + to_u8string(std::to_string(index)));
        });
    }

    const bool all_arrived { recorder.wait_for_count(3, 20s) };
    if (all_arrived)
        for (const auto& item : recorder.snapshot())
            item.reply(askpass_reply::with_answer(u8"answer-for-" + item.request.prompt.key_path));
    for (std::thread& thread : threads)
        thread.join();

    REQUIRE(all_arrived);
    for (std::size_t index { 0 }; index < runs.size(); ++index)
    {
        CHECK(exit_code_of(runs[index]) == 0);
        CHECK(runs[index].standard_output == "answer-for-key" + std::to_string(index) + "\n");
    }
}

TEST_CASE("one operation can prompt twice in sequence", "[askpass][server]")
{
    // git은 https에서 사용자 이름과 비밀번호를 각각 한 번씩, 같은 operation에서 차례로 묻는다.
    askpass_prompt_handler by_kind = [](askpass_prompt_request request, askpass_reply_callback reply) {
        const bool user { request.prompt.kind == credential_prompt_kind::https_username };
        reply(askpass_reply::with_answer(user ? u8"alice" : u8"pw"));
    };
    const std::unique_ptr<askpass_service> server { start(std::move(by_kind)) };
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };

    const helper_run user { run_prompt(*runner, *server, u8"Username for 'https://example.invalid': ", u8"op-7") };
    const helper_run password { run_prompt(*runner, *server, u8"Password for 'https://alice@example.invalid': ", u8"op-7") };

    CHECK(user.standard_output == "alice\n");
    CHECK(password.standard_output == "pw\n");
}

TEST_CASE("shutdown answers pending prompts with a cancel and returns quickly", "[askpass][server][shutdown]")
{
    recording_handler recorder {};
    std::unique_ptr<askpass_service> server { start(recorder.handler()) };

    helper_run run {};
    const auto prompt_once = [&run, &server]() {
        const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
        run = run_prompt(*runner, *server, u8"Enter passphrase: ");
    };
    std::thread helper { prompt_once };
    REQUIRE(recorder.wait_for_count(1, 10s));

    const auto started { std::chrono::steady_clock::now() };
    server->shutdown();
    const auto elapsed { std::chrono::steady_clock::now() - started };
    helper.join();

    CHECK(elapsed < 2s);
    CHECK(exit_code_of(run) == 1);
    CHECK(run.standard_output.empty());

    // 종료 뒤에 늦게 온 답은 아무 일도 하지 않는다.
    recorder.snapshot()[0].reply(askpass_reply::with_answer(u8"late"));
    server->shutdown();
    server.reset();
}

TEST_CASE("destroying an idle server does not hang", "[askpass][server][shutdown]")
{
    const auto started { std::chrono::steady_clock::now() };
    {
        const std::unique_ptr<askpass_service> server { start(cancelling()) };
    }
    CHECK(std::chrono::steady_clock::now() - started < 2s);
}

TEST_CASE("cancelling an operation only affects that operation", "[askpass][server][cancel]")
{
    recording_handler recorder {};
    const std::unique_ptr<askpass_service> server { start(recorder.handler()) };

    helper_run cancelled_run {};
    helper_run kept_run {};
    const auto prompt_to_cancel = [&]() {
        const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
        cancelled_run = run_prompt(*runner, *server, u8"Enter passphrase: ", u8"op-cancel");
    };
    const auto prompt_to_keep = [&]() {
        const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
        kept_run = run_prompt(*runner, *server, u8"Password for 'https://x@y': ", u8"op-keep");
    };
    std::thread cancelled { prompt_to_cancel };
    std::thread kept { prompt_to_keep };
    REQUIRE(recorder.wait_for_count(2, 20s));

    server->cancel_operation(u8"op-cancel");
    cancelled.join();
    CHECK(exit_code_of(cancelled_run) == 1);

    for (const auto& item : recorder.snapshot())
        if (item.request.operation_id == u8"op-keep")
            item.reply(askpass_reply::with_answer(u8"kept"));
    kept.join();
    CHECK(exit_code_of(kept_run) == 0);
    CHECK(kept_run.standard_output == "kept\n");

    // 취소된 operation의 이후 질문은 핸들러에 닿지 않고 바로 취소된다.
    const std::size_t before { recorder.count() };
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    const helper_run after { run_prompt(*runner, *server, u8"Enter passphrase: ", u8"op-cancel") };
    CHECK(exit_code_of(after) == 1);
    CHECK(recorder.count() == before);
}

TEST_CASE("a client that sends garbage does not disturb later prompts", "[askpass][server][robustness]")
{
    const std::unique_ptr<askpass_service> server { start(answering(u8"ok")) };
    const askpass_endpoint endpoint { server->endpoint() };
    std::wstring pipe_name {};
    for (const char8_t character : endpoint.pipe_name)
        pipe_name.push_back(static_cast<wchar_t>(character));

    {
        const HANDLE pipe { CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr) };
        REQUIRE(pipe != INVALID_HANDLE_VALUE);
        const std::string garbage(64, 'Z');
        DWORD written { 0 };
        WriteFile(pipe, garbage.data(), static_cast<DWORD>(garbage.size()), &written, nullptr);
        char reply[16] {};
        DWORD read { 0 };
        // 서버는 형식이 틀린 연결을 답 없이 닫는다.
        CHECK(ReadFile(pipe, reply, sizeof(reply), &read, nullptr) == FALSE);
        CloseHandle(pipe);
    }

    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    const helper_run run { run_prompt(*runner, *server, u8"Enter passphrase: ") };
    CHECK(exit_code_of(run) == 0);
    CHECK(run.standard_output == "ok\n");
}

TEST_CASE("the pipe name is unique per server and only the current user may open it", "[askpass][server][security]")
{
    const std::unique_ptr<askpass_service> first { start(cancelling()) };
    const std::unique_ptr<askpass_service> second { start(cancelling()) };

    const askpass_endpoint a { first->endpoint() };
    const askpass_endpoint b { second->endpoint() };
    CHECK(a.pipe_name != b.pipe_name);
    CHECK(a.token != b.token);
    CHECK(a.token.size() == 32);
    CHECK(a.pipe_name.starts_with(u8"\\\\.\\pipe\\cyella-askpass-"));

    // 파이프의 보안 기술자를 읽어 ACE가 현재 사용자 하나뿐인지 본다.
    std::wstring pipe_name {};
    for (const char8_t character : a.pipe_name)
        pipe_name.push_back(static_cast<wchar_t>(character));
    PSECURITY_DESCRIPTOR descriptor { nullptr };
    REQUIRE(GetNamedSecurityInfoW(pipe_name.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &descriptor) == ERROR_SUCCESS);
    LPWSTR text { nullptr };
    REQUIRE(ConvertSecurityDescriptorToStringSecurityDescriptorW(descriptor, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &text, nullptr) != FALSE);
    const std::wstring sddl { text };
    LocalFree(text);
    LocalFree(descriptor);

    CHECK(sddl.starts_with(L"D:P("));
    std::size_t ace_count { 0 };
    for (const wchar_t character : sddl)
        if (character == L'(')
            ++ace_count;
    CHECK(ace_count == 1);
}
