#include "infrastructure/ssh_tools.h"
#include "platform/win32/win32_process_runner.h"

#include "process_test_support.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
    using namespace cyella;
    using namespace cyella::test;
    using namespace std::chrono_literals;

    std::string pattern_bytes(const std::size_t count)
    {
        std::string bytes(count, '\0');
        for (std::size_t index = 0; index < count; ++index)
            bytes[index] = static_cast<char>(index % 256u);
        return bytes;
    }

    // 첫 출력이 도착했음을 다른 스레드에 알린다. 취소 test가 "실행 중"을 확인하는 용도다.
    class signalling_sink final : public process_output_sink
    {
    public:
        void on_record(const process_output_record&) override
        {
            signal();
        }

        void on_chunk(const process_output_chunk&) override
        {
            signal();
        }

        [[nodiscard]] bool wait_for_output(const std::chrono::milliseconds limit)
        {
            std::unique_lock<std::mutex> lock { mutex_ };
            return condition_.wait_for(lock, limit, [this]() { return seen_; });
        }

    private:
        void signal()
        {
            {
                const std::lock_guard<std::mutex> lock { mutex_ };
                seen_ = true;
            }
            condition_.notify_all();
        }

        std::mutex mutex_ {};
        std::condition_variable condition_ {};
        bool seen_ { false };
    };

    bool has_diagnostic(const process_result& result, const diagnostic_code code, const diagnostic_severity severity)
    {
        for (const diagnostic& value : result.diagnostics)
            if (value.code == code && value.severity == severity)
                return true;
        return false;
    }
} // namespace

TEST_CASE("runner reports the exit code of a finished child", "[process]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };

    const process_result result { runner->run(make_child_request({ "exit", "7" }), nullptr, {}) };

    CHECK(result.completion == process_completion::exited);
    REQUIRE(result.exit_code.has_value());
    CHECK(*result.exit_code == 7);
    CHECK_FALSE(result.succeeded());
}

TEST_CASE("runner rejects a relative executable and a missing one", "[process]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };

    process_request relative { make_child_request({}) };
    relative.executable = u8"cyella_test_child.exe";
    CHECK(runner->run(relative, nullptr, {}).completion == process_completion::invalid_request);

    process_request missing { make_child_request({}) };
    missing.executable = u8"C:/cyella-does-not-exist/none.exe";
    CHECK(runner->run(missing, nullptr, {}).completion == process_completion::start_failed);
}

TEST_CASE("runner feeds standard input and closes it", "[process][stdin]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    process_request request { make_child_request({ "echo_stdin" }) };
    request.standard_input = "hello\nworld\n";
    collecting_sink sink {};

    const process_result result { runner->run(request, &sink, {}) };

    REQUIRE(result.succeeded());
    const std::vector<std::u8string> lines { sink.record_texts(process_stream::standard_output) };
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == u8"hello");
    CHECK(lines[1] == u8"world");
}

TEST_CASE("runner streams a large standard input without deadlock", "[process][stdin]")
{
    // 입력과 출력이 모두 pipe buffer보다 크다. 쓰기와 읽기가 서로 다른 스레드가 아니면
    // 자식은 출력에서, 부모는 입력에서 막혀 영영 끝나지 않는다.
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    process_request request { make_child_request({ "echo_stdin" }) };
    request.standard_input = pattern_bytes(2u * 1024u * 1024u);
    request.standard_output_mode = process_output_mode::raw;
    collecting_sink sink {};

    const process_result result { runner->run(request, &sink, {}) };

    REQUIRE(result.succeeded());
    CHECK(sink.standard_output_bytes == request.standard_input);
}

TEST_CASE("runner gives the child an empty input when no payload is set", "[process][stdin]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    collecting_sink sink {};

    const process_result result { runner->run(make_child_request({ "echo_stdin", "write_text", "done" }), &sink, {}) };

    REQUIRE(result.succeeded());
    const std::vector<std::u8string> lines { sink.record_texts(process_stream::standard_output) };
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == u8"done");
}

TEST_CASE("the server command for a new ssh repository reaches the child as one unchanged argument", "[process][ssh]")
{
    // 서버에 저장소를 만드는 셸 명령은 따옴표·`$`·`;`·중괄호를 담는다. Windows 명령줄을 거쳐 ssh에 글자 그대로 닿아야 한다.
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    collecting_sink sink {};
    const std::u8string command { make_create_bare_repository_command("~/it's $(x) `y` \"z\" a.git") };
    // 시험 자식의 `write_text`는 `\`를 풀어 읽으므로 두 번 적어 보낸다. 명령줄 인용이 지키는지 보는 것은 그 앞 단계다.
    std::u8string escaped {};
    for (const char8_t character : command)
    {
        if (character == u8'\\')
            escaped.push_back(u8'\\');
        escaped.push_back(character);
    }
    process_request request { make_child_request({ "write_text" }) };
    request.arguments.push_back(escaped);

    const process_result result { runner->run(request, &sink, {}) };

    REQUIRE(result.succeeded());
    const std::vector<std::u8string> lines { sink.record_texts(process_stream::standard_output) };
    REQUIRE(lines.size() == 1);
    CHECK(std::string { lines[0].begin(), lines[0].end() } == std::string { command.begin(), command.end() });
}

TEST_CASE("runner tolerates a child that never reads its input", "[process][stdin]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    process_request request { make_child_request({ "exit", "0" }) };
    request.standard_input = pattern_bytes(4u * 1024u * 1024u);

    const process_result result { runner->run(request, nullptr, {}) };

    CHECK(result.completion == process_completion::exited);
    CHECK_FALSE(has_diagnostic(result, diagnostic_code::process_pipe_failed, diagnostic_severity::warning));
}

TEST_CASE("raw mode keeps NUL and carriage return bytes untouched", "[process][raw]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    process_request request { make_child_request({ "write_text", "a\\0b\\r\\nc\\rd\\0" }) };
    request.standard_output_mode = process_output_mode::raw;
    collecting_sink sink {};

    const process_result result { runner->run(request, &sink, {}) };

    REQUIRE(result.succeeded());
    CHECK(sink.records.empty());
    CHECK(sink.standard_output_bytes == std::string { "a\0b\r\nc\rd\0", 9 });
}

TEST_CASE("raw mode delivers large output in bounded chunks", "[process][raw]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    process_request request { make_child_request({ "write_bytes", "300000" }) };
    request.standard_output_mode = process_output_mode::raw;
    collecting_sink sink {};

    const process_result result { runner->run(request, &sink, {}) };

    REQUIRE(result.succeeded());
    CHECK(sink.standard_output_bytes == pattern_bytes(300000));
    // 한 번에 읽는 크기보다 크므로 반드시 여러 조각으로 온다. 조각 경계는 의미 단위와 무관하다.
    CHECK(sink.chunks.size() >= 5);
    std::uint64_t previous_sequence { 0 };
    for (const collecting_sink::chunk_copy& chunk : sink.chunks)
    {
        CHECK(chunk.bytes.size() <= process_read_block_byte_size);
        CHECK(chunk.sequence > previous_sequence);
        previous_sequence = chunk.sequence;
    }
    CHECK(result.record_count == sink.chunks.size());
}

TEST_CASE("raw standard output can be combined with line mode standard error", "[process][raw]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    process_request request { make_child_request({ "write_text", "payload", "error_lines", "2" }) };
    request.standard_output_mode = process_output_mode::raw;
    collecting_sink sink {};

    const process_result result { runner->run(request, &sink, {}) };

    REQUIRE(result.succeeded());
    CHECK(sink.standard_output_bytes == "payload");
    const std::vector<std::u8string> errors { sink.record_texts(process_stream::standard_error) };
    REQUIRE(errors.size() == 2);
    CHECK(errors[0] == u8"line 0");
    CHECK(errors[1] == u8"line 1");
}

TEST_CASE("raw mode honors the capture cap and keeps draining", "[process][raw][cap]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    process_request request { make_child_request({ "write_bytes", "1000000", "exit", "5" }) };
    request.standard_output_mode = process_output_mode::raw;
    request.maximum_captured_bytes_per_stream = 100000;
    collecting_sink sink {};

    const process_result result { runner->run(request, &sink, {}) };

    // 상한을 넘겨도 pipe를 계속 비우므로 자식은 막히지 않고 종료 코드까지 보고한다.
    CHECK(result.completion == process_completion::exited);
    REQUIRE(result.exit_code.has_value());
    CHECK(*result.exit_code == 5);
    CHECK(result.output_truncated);
    CHECK(result.captured_bytes == 100000);
    CHECK(sink.standard_output_bytes == pattern_bytes(100000));
    CHECK(has_diagnostic(result, diagnostic_code::process_output_truncated, diagnostic_severity::warning));
}

TEST_CASE("line mode honors the capture cap", "[process][cap]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    process_request request { make_child_request({ "write_bytes", "100000" }) };
    request.maximum_captured_bytes_per_stream = 10000;

    const process_result result { runner->run(request, nullptr, {}) };

    CHECK(result.completion == process_completion::exited);
    CHECK(result.output_truncated);
    CHECK(result.captured_bytes == 10000);
}

TEST_CASE("an unlimited capture cap never truncates", "[process][cap]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    process_request request { make_child_request({ "write_bytes", "200000" }) };
    request.standard_output_mode = process_output_mode::raw;
    request.maximum_captured_bytes_per_stream = unlimited_process_capture_bytes;
    collecting_sink sink {};

    const process_result result { runner->run(request, &sink, {}) };

    CHECK_FALSE(result.output_truncated);
    CHECK(sink.standard_output_bytes.size() == 200000);
}

TEST_CASE("line mode splits lines and flags progress records", "[process][lines]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    collecting_sink sink {};

    const process_result result { runner->run(make_child_request({ "write_text", "alpha\\r\\nbeta\\rgamma\\nlast" }), &sink, {}) };

    REQUIRE(result.succeeded());
    REQUIRE(sink.records.size() == 4);
    CHECK(sink.records[0].text == u8"alpha");
    CHECK_FALSE(sink.records[0].progress);
    CHECK(sink.records[1].text == u8"beta");
    CHECK(sink.records[1].progress);
    CHECK(sink.records[2].text == u8"gamma");
    CHECK_FALSE(sink.records[2].progress);
    CHECK(sink.records[3].text == u8"last");
    CHECK(sink.chunks.empty());
}

TEST_CASE("line mode splits long lines at the record limit", "[process][lines]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    process_request request { make_child_request({ "write_text", std::string(20000, 'x') }) };
    collecting_sink sink {};

    const process_result result { runner->run(request, &sink, {}) };

    REQUIRE(result.succeeded());
    REQUIRE(sink.records.size() >= 3);
    CHECK(sink.records.front().continued);
    CHECK(sink.records.front().text.size() <= default_process_record_byte_limit);
}

TEST_CASE("line mode masks credentials in output", "[process][lines][masking]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    collecting_sink sink {};

    const process_result result { runner->run(make_child_request({ "write_error_text", "fatal: https://user:token@host/repo.git\\n" }), &sink, {}) };

    REQUIRE(result.succeeded());
    const std::vector<std::u8string> errors { sink.record_texts(process_stream::standard_error) };
    REQUIRE(errors.size() == 1);
    CHECK(errors[0] == u8"fatal: https://user:***@host/repo.git");
}

TEST_CASE("the recorded command line masks credentials", "[process][masking]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };

    // 자식은 `exit` 앞에서 끝나므로 뒤의 인자는 해석되지 않는다.
    const process_result result { runner->run(make_child_request({ "exit", "0", "https://user:token@host/repo.git" }), nullptr, {}) };

    CHECK(result.completion == process_completion::exited);
    CHECK(result.masked_command_line.find(u8"user:***@host") != std::u8string::npos);
    CHECK(result.masked_command_line.find(u8"token") == std::u8string::npos);
}

TEST_CASE("an already cancelled token prevents the child from starting", "[process][cancel]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    process_cancellation_source source {};
    source.request_cancellation();
    collecting_sink sink {};

    const process_result result { runner->run(make_child_request({ "write_text", "never" }), &sink, source.token()) };

    CHECK(result.completion == process_completion::cancelled);
    CHECK(sink.records.empty());
}

TEST_CASE("cancellation stops a running child", "[process][cancel]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    process_cancellation_source source {};
    signalling_sink sink {};

    const auto started { std::chrono::steady_clock::now() };
    std::future<process_result> pending { std::async(std::launch::async, [&]() { return runner->run(make_child_request({ "write_text", "started\\n", "sleep", "30000" }), &sink, source.token()); }) };

    REQUIRE(sink.wait_for_output(10s));
    source.request_cancellation();
    REQUIRE(pending.wait_for(10s) == std::future_status::ready);
    const process_result result { pending.get() };

    CHECK(result.completion == process_completion::cancelled);
    CHECK_FALSE(result.exit_code.has_value());
    CHECK(std::chrono::steady_clock::now() - started < 15s);
}

TEST_CASE("cancellation unblocks a child that stopped reading its input", "[process][cancel][stdin]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    process_cancellation_source source {};
    signalling_sink sink {};
    process_request request { make_child_request({ "write_text", "started\\n", "sleep", "30000" }) };
    // 자식이 입력을 읽지 않으므로 pipe가 차서 writer가 WriteFile에서 막힌다.
    request.standard_input = pattern_bytes(8u * 1024u * 1024u);

    std::future<process_result> pending { std::async(std::launch::async, [&]() { return runner->run(request, &sink, source.token()); }) };

    REQUIRE(sink.wait_for_output(10s));
    source.request_cancellation();
    REQUIRE(pending.wait_for(10s) == std::future_status::ready);
    CHECK(pending.get().completion == process_completion::cancelled);
}

TEST_CASE("a timeout terminates the child", "[process][timeout]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    process_request request { make_child_request({ "sleep", "30000" }) };
    request.timeout = 300ms;

    const process_result result { runner->run(request, nullptr, {}) };

    CHECK(result.completion == process_completion::timed_out);
    CHECK_FALSE(result.exit_code.has_value());
    CHECK(result.duration >= 250ms);
    CHECK(result.duration < 10s);
    CHECK(has_diagnostic(result, diagnostic_code::process_timed_out, diagnostic_severity::error));
}

TEST_CASE("a grandchild holding the pipe does not hang the runner", "[process][grandchild]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    collecting_sink sink {};

    const process_result result { runner->run(make_child_request({ "grandchild", "60000", "write_text", "parent-done", "exit", "4" }), &sink, {}) };

    // 자식은 바로 끝났지만 손자가 pipe를 잡고 있다. 유예 뒤 트리를 종료하고 수집을 마감한다.
    CHECK(result.completion == process_completion::exited);
    REQUIRE(result.exit_code.has_value());
    CHECK(*result.exit_code == 4);
    CHECK(result.duration < 10s);
    CHECK(has_diagnostic(result, diagnostic_code::operation_failed, diagnostic_severity::warning));
    const std::vector<std::u8string> lines { sink.record_texts(process_stream::standard_output) };
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == u8"parent-done");
}

TEST_CASE("a grandchild is terminated together with a cancelled child", "[process][grandchild][cancel]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };
    process_request request { make_child_request({ "grandchild", "60000", "sleep", "30000" }) };
    request.timeout = 500ms;

    const process_result result { runner->run(request, nullptr, {}) };

    CHECK(result.completion == process_completion::timed_out);
    CHECK(result.duration < 10s);
}

TEST_CASE("one runner instance serves concurrent runs", "[process]")
{
    const std::unique_ptr<process_runner> runner { win32::make_process_runner() };

    std::vector<std::future<process_result>> runs {};
    for (int index = 0; index < 4; ++index)
    {
        runs.push_back(std::async(std::launch::async, [&runner, index]() {
            process_request request { make_child_request({ "echo_stdin" }) };
            request.standard_input = "run " + std::to_string(index);
            return runner->run(request, nullptr, {});
        }));
    }

    for (std::future<process_result>& run : runs)
        CHECK(run.get().succeeded());
}
