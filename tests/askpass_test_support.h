#pragma once

#include "application/askpass_service.h"
#include "application/process_request.h"
#include "application/process_runner.h"
#include "infrastructure/askpass_environment.h"
#include "process_test_support.h"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace cyella::test {
    // CMake가 `cyella-askpass.exe`의 절대 경로를 넣어 준다.
    inline std::filesystem::path askpass_helper_path()
    {
        return std::filesystem::absolute(std::filesystem::path { CYELLA_ASKPASS_PATH });
    }

    inline askpass_session session_for(const askpass_endpoint& endpoint, const std::u8string_view operation_id)
    {
        askpass_session session {};
        session.pipe_name = endpoint.pipe_name;
        session.token = endpoint.token;
        session.operation_id = std::u8string { operation_id };
        return session;
    }

    // 도우미를 직접 실행하는 요청이다. 표준 출력을 가공 없이 받도록 raw 모드로 둔다.
    inline process_request make_helper_request(const std::u8string_view prompt, const askpass_session& session)
    {
        const std::filesystem::path helper { askpass_helper_path() };

        process_request request {};
        request.executable = to_u8string(helper.generic_string());
        request.working_directory = to_u8string(helper.parent_path().generic_string());
        request.arguments.push_back(std::u8string { prompt });
        request.environment_overrides = askpass_session_overrides(true, session);
        request.standard_output_mode = process_output_mode::raw;
        request.timeout = std::chrono::seconds { 30 };
        request.maximum_captured_bytes_per_stream = 1024u * 1024u;
        return request;
    }

    struct helper_run
    {
        process_result result {};
        std::string standard_output {};
    };

    inline helper_run run_helper(process_runner& runner, const process_request& request)
    {
        collecting_sink sink {};
        helper_run run {};
        run.result = runner.run(request, &sink, {});
        run.standard_output = sink.standard_output_bytes;
        return run;
    }
} // namespace cyella::test
