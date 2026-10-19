#include "application/cancellation_registry.h"
#include "application/process_cancellation.h"
#include "application/process_request.h"
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
} // namespace

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
