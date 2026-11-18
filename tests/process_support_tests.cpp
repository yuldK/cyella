#include "application/cancellation_registry.h"
#include "application/process_cancellation.h"
#include "application/process_request.h"
#include "infrastructure/process_output_pipeline.h"
#include "platform/win32/operation_result_relay.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <string>
#include <vector>

using namespace cyella;

namespace {
    process_request valid_request()
    {
        process_request request {};
        request.executable = u8"C:/Git/cmd/git.exe";
        request.working_directory = u8"C:/work";
        request.maximum_captured_bytes_per_stream = 1024;
        return request;
    }

    // 줄 단위 파이프라인에 byte를 한 번에 넣고 끝까지 비운 레코드다.
    std::vector<process_output_record> pipeline_records(const std::u8string_view bytes, const std::size_t maximum_record_bytes = default_process_record_byte_limit)
    {
        std::vector<process_output_record> records {};
        const process_output_pipeline::record_handler handler { [&records](process_output_record& record) { records.push_back(record); } };
        process_output_pipeline pipeline { process_stream::standard_error, maximum_record_bytes, 1024u * 1024u };
        pipeline.append(bytes, handler);
        pipeline.flush(handler);
        return records;
    }
} // namespace

TEST_CASE("a line ending in a doubled carriage return is a normal line, not progress", "[pipeline]")
{
    // Windows text 모드로 줄 끝을 한 번 더 바꾼 hook은 `\r\r\n`을 낸다. 진행 표시로 보면 거절 이유가 실패 설명에서 빠진다.
    const std::vector<process_output_record> records { pipeline_records(u8"remote: rejected by hook\r\r\nnext\r\n") };

    REQUIRE(records.size() == 2);
    CHECK(records[0].text == u8"remote: rejected by hook");
    CHECK_FALSE(records[0].progress);
    CHECK(records[1].text == u8"next");
    CHECK_FALSE(records[1].progress);

    // 단독 `\r`은 여전히 진행 표시다.
    const std::vector<process_output_record> progress { pipeline_records(u8"Receiving objects:  10%\rReceiving objects: 100%\n") };
    REQUIRE(progress.size() == 2);
    CHECK(progress[0].progress);
    CHECK_FALSE(progress[1].progress);
}

TEST_CASE("a forced split keeps a credential url in one piece so it is still masked", "[pipeline][masking]")
{
    // 한도에서 그대로 자르면 token 중간이 경계가 되어 어느 조각에서도 URL로 알아보지 못한다.
    const std::u8string line { u8"fatal: " + std::u8string(40, u8'x') + u8" https://user:secrettoken12@example.com/r.git\n" };
    const std::vector<process_output_record> records { pipeline_records(line, 64) };

    REQUIRE(records.size() == 2);
    CHECK(records[0].continued);
    CHECK(records[0].text == u8"fatal: " + std::u8string(40, u8'x') + u8" ");
    CHECK_FALSE(records[1].continued);
    CHECK(records[1].text == u8"https://user:***@example.com/r.git");
    for (const process_output_record& record : records)
        CHECK(record.text.find(u8"secret") == std::u8string::npos);
}

TEST_CASE("a forced split does not move an option value away from its option name", "[pipeline][masking]")
{
    // `--password` 뒤 공백으로 경계를 옮기면 값 전체가 이름 없이 다음 조각으로 가 가려지지 않는다.
    const std::u8string line { u8"fatal: " + std::u8string(30, u8'x') + u8" --password supersecretvalue12345 tail\n" };
    const std::vector<process_output_record> records { pipeline_records(line, 64) };

    REQUIRE(records.size() == 2);
    CHECK(records[0].continued);
    CHECK(records[0].text == u8"fatal: " + std::u8string(30, u8'x') + u8" --password ***");
    CHECK(records[1].text.find(u8"supersecret") == std::u8string::npos);
}

TEST_CASE("a valid process request has no diagnostics", "[request]")
{
    CHECK(validate_process_request(valid_request()).empty());
}

TEST_CASE("process request validation rejects unusable values", "[request]")
{
    process_request relative { valid_request() };
    relative.executable = u8"git.exe";
    CHECK_FALSE(validate_process_request(relative).empty());

    process_request no_directory { valid_request() };
    no_directory.working_directory = u8"";
    CHECK_FALSE(validate_process_request(no_directory).empty());

    process_request nul_argument { valid_request() };
    nul_argument.arguments.push_back(std::u8string { u8"a" } + std::u8string(1, u8'\0'));
    CHECK_FALSE(validate_process_request(nul_argument).empty());

    process_request duplicate_variable { valid_request() };
    duplicate_variable.environment_overrides.push_back({ u8"GIT_X", u8"1" });
    duplicate_variable.environment_overrides.push_back({ u8"git_x", u8"2" });
    CHECK_FALSE(validate_process_request(duplicate_variable).empty());

    process_request no_capture { valid_request() };
    no_capture.maximum_captured_bytes_per_stream = 0;
    CHECK_FALSE(validate_process_request(no_capture).empty());
}

TEST_CASE("a cancellation token reports a requested cancellation", "[cancel]")
{
    process_cancellation_source source {};
    const process_cancellation_token token { source.token() };
    int callbacks { 0 };
    const process_cancellation_registration registration { token.register_callback([&callbacks]() { ++callbacks; }) };

    CHECK_FALSE(token.cancelled());
    source.request_cancellation();
    source.request_cancellation();

    CHECK(token.cancelled());
    CHECK(callbacks == 1);
    CHECK_FALSE(process_cancellation_token {}.cancelled());
}

TEST_CASE("a released registration is not called", "[cancel]")
{
    process_cancellation_source source {};
    int callbacks { 0 };
    {
        const process_cancellation_registration registration { source.token().register_callback([&callbacks]() { ++callbacks; }) };
    }

    source.request_cancellation();

    CHECK(callbacks == 0);
}

TEST_CASE("the registry cancels every live source and forgets dead ones", "[cancel]")
{
    cancellation_registry registry {};
    process_cancellation_source first {};
    {
        process_cancellation_source dropped {};
        registry.add(dropped);
    }
    process_cancellation_source second {};
    registry.add(first);
    registry.add(second);

    CHECK(registry.size() == 2);
    registry.cancel_all();

    CHECK(first.cancellation_requested());
    CHECK(second.cancellation_requested());
}

TEST_CASE("the relay hands results over in arrival order", "[relay]")
{
    win32::operation_result_relay relay {};
    relay.post(luil::app_message { 1 });
    relay.post(luil::app_message { std::string { "two" } });
    relay.post(luil::app_message { 3 });
    CHECK(relay.pending() == 3);

    std::vector<std::string> seen {};
    relay.drain([&seen](luil::app_message message) {
        if (const int* const number { message.get<int>() }; number != nullptr)
            seen.push_back(std::to_string(*number));
        else if (const std::string* const text { message.get<std::string>() }; text != nullptr)
            seen.push_back(*text);
    });

    CHECK(seen == std::vector<std::string> { "1", "two", "3" });
    CHECK(relay.pending() == 0);
}
