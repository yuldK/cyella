#pragma once

#include "application/process_request.h"
#include "application/process_runner.h"
#include "domain/process_execution.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cyella::test {
    inline std::u8string to_u8string(const std::string_view text)
    {
        return std::u8string { reinterpret_cast<const char8_t*>(text.data()), text.size() };
    }

    inline std::string to_string(const std::u8string_view text)
    {
        return std::string { reinterpret_cast<const char*>(text.data()), text.size() };
    }

    // CMake가 `cyella_test_child` 실행 파일의 절대 경로를 넣어 준다.
    inline std::filesystem::path test_child_path()
    {
        return std::filesystem::path { CYELLA_TEST_CHILD_PATH };
    }

    // 도우미 실행 파일에 인자를 넘기는 기본 요청이다. 어떤 test도 무기한 대기하지 않도록
    // 안전망 timeout을 둔다. timeout을 시험하는 test만 값을 바꾼다.
    inline process_request make_child_request(const std::vector<std::string>& arguments)
    {
        const std::filesystem::path child { std::filesystem::absolute(test_child_path()) };

        process_request request {};
        request.executable = to_u8string(child.generic_string());
        request.working_directory = to_u8string(child.parent_path().generic_string());
        for (const std::string& argument : arguments)
            request.arguments.push_back(to_u8string(argument));
        request.timeout = std::chrono::seconds { 30 };
        request.maximum_captured_bytes_per_stream = 64u * 1024u * 1024u;
        return request;
    }

    // 두 스트림의 레코드와 조각을 순서대로 보관한다. runner가 호출을 직렬화하므로 잠금이 없다.
    class collecting_sink final : public process_output_sink
    {
    public:
        void on_record(const process_output_record& record) override
        {
            records.push_back(record);
        }

        void on_chunk(const process_output_chunk& chunk) override
        {
            chunks.push_back(chunk_copy { chunk.sequence, chunk.stream, std::string { chunk.bytes } });
            raw_bytes(chunk.stream).append(chunk.bytes);
        }

        [[nodiscard]] std::vector<std::u8string> record_texts(const process_stream stream) const
        {
            std::vector<std::u8string> texts {};
            for (const process_output_record& record : records)
                if (record.stream == stream)
                    texts.push_back(record.text);
            return texts;
        }

        struct chunk_copy
        {
            std::uint64_t sequence {};
            process_stream stream { process_stream::standard_output };
            std::string bytes {};
        };

        std::vector<process_output_record> records {};
        std::vector<chunk_copy> chunks {};
        std::string standard_output_bytes {};
        std::string standard_error_bytes {};

    private:
        std::string& raw_bytes(const process_stream stream)
        {
            return stream == process_stream::standard_error ? standard_error_bytes : standard_output_bytes;
        }
    };
} // namespace cyella::test
