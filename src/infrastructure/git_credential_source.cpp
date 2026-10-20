#include "infrastructure/git_credential_source.h"

#include "infrastructure/git_execution_policy.h"
#include "infrastructure/secret_masking.h"

#include <algorithm>
#include <filesystem>
#include <utility>

namespace cyella {
    namespace {
        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        bool safe_value(const std::string_view value)
        {
            return std::ranges::none_of(value, [](const char character) { return character == '\n' || character == '\r' || character == '\0'; });
        }

        // 출력을 모으는 sink다. 표준 출력은 비밀을 담으므로 레코드로 만들지 않고 조각을 이어 붙이며, 표준 오류는 줄을 모은다.
        class credential_sink final : public process_output_sink
        {
        public:
            void on_record(const process_output_record& record) override
            {
                if (record.stream == process_stream::standard_error && error_lines_ < 4)
                {
                    if (error_text.empty() == false)
                        error_text.push_back(' ');
                    error_text += std::string { reinterpret_cast<const char*>(record.text.data()), record.text.size() };
                    ++error_lines_;
                }
            }

            void on_chunk(const process_output_chunk& chunk) override
            {
                if (chunk.stream == process_stream::standard_output)
                    output.append(chunk.bytes);
            }

            std::string output {};
            std::string error_text {};

        private:
            int error_lines_ { 0 };
        };

        std::string masked(const std::string& text)
        {
            const std::u8string result { mask_secrets(to_u8(text)) };
            return std::string { reinterpret_cast<const char*>(result.data()), result.size() };
        }
    } // namespace

    std::optional<std::string> encode_credential_input(const credential_query& query, const std::string_view username, const std::string_view password)
    {
        if (query.protocol.empty() || query.host.empty() || safe_value(query.protocol) == false || safe_value(query.host) == false || safe_value(username) == false || safe_value(password) == false)
            return std::nullopt;
        std::string input { "protocol=" + query.protocol + "\nhost=" + query.host + "\n" };
        if (username.empty() == false)
            input += "username=" + std::string { username } + "\n";
        if (password.empty() == false)
            input += "password=" + std::string { password } + "\n";
        input += "\n";
        return input;
    }

    parsed_credential_output parse_credential_output(const std::string_view output)
    {
        parsed_credential_output result {};
        std::size_t start { 0 };
        while (start < output.size())
        {
            std::size_t end { output.find('\n', start) };
            if (end == std::string_view::npos)
                end = output.size();
            std::string_view line { output.substr(start, end - start) };
            start = end + 1;
            if (line.empty() == false && line.back() == '\r')
                line.remove_suffix(1);
            const std::size_t equals { line.find('=') };
            if (equals == std::string_view::npos)
                continue;
            const std::string_view key { line.substr(0, equals) };
            const std::string_view value { line.substr(equals + 1) };
            if (key == "username")
                result.username = std::string { value };
            else if (key == "password")
            {
                result.password = secret_text { std::string { value } };
                result.has_password = value.empty() == false;
            }
        }
        return result;
    }

    git_credential_source::git_credential_source(process_runner& runner, git_credential_source_options options)
        : runner_ { &runner }
        , options_ { std::move(options) }
    {}

    process_request git_credential_source::make_request(const credential_query& query, std::string verb, std::string standard_input) const
    {
        git_request_options request_options {};
        request_options.raw_standard_output = true;
        request_options.standard_input = std::move(standard_input);
        request_options.environment = options_.environment;
        std::u8string directory { query.working_directory.empty() ? options_.default_working_directory : to_u8(query.working_directory) };
        if (directory.empty())
            directory = std::filesystem::temp_directory_path().u8string();
        process_request request { make_git_process_request(options_.git_executable, directory, { u8"credential", to_u8(verb) }, git_command_class::network, request_options) };
        request.timeout = options_.timeout;
        request.maximum_captured_bytes_per_stream = 64u * 1024u;
        return request;
    }

    credential_fill_result git_credential_source::fill(const credential_query& query, const process_cancellation_token& cancellation)
    {
        credential_fill_result result {};
        std::optional<std::string> input { encode_credential_input(query) };
        if (input.has_value() == false)
        {
            result.error = "자격 증명을 물을 호스트가 올바르지 않습니다.";
            return result;
        }
        process_request request { make_request(query, "fill", std::move(*input)) };
        credential_sink sink {};
        const process_result run { runner_->run(request, &sink, cancellation) };
        wipe_secret(request.standard_input);

        if (run.completion == process_completion::cancelled)
        {
            result.cancelled = true;
            result.error = "취소되었습니다.";
        }
        else if (run.completion == process_completion::timed_out)
            result.error = "자격 증명 도우미가 제때 답하지 않았습니다.";
        else if (run.completion != process_completion::exited)
            result.error = "git을 실행하지 못했습니다.";
        else if (run.exit_code.value_or(-1) != 0)
        {
            result.error = "자격 증명 도우미가 토큰을 주지 않았습니다";
            if (sink.error_text.empty() == false)
                result.error += " (" + masked(sink.error_text) + ")";
            result.error += ".";
        }
        else
        {
            parsed_credential_output parsed { parse_credential_output(sink.output) };
            if (parsed.has_password)
            {
                result.ok = true;
                result.username = std::move(parsed.username);
                result.token = std::move(parsed.password);
            }
            else
                result.error = "자격 증명 도우미가 비밀번호(토큰)를 주지 않았습니다.";
        }
        wipe_secret(sink.output);
        return result;
    }

    void git_credential_source::reject(const credential_query& query, const std::string_view username, const secret_text& token, const process_cancellation_token& cancellation)
    {
        std::optional<std::string> input { encode_credential_input(query, username, token.value()) };
        if (input.has_value() == false)
            return;
        process_request request { make_request(query, "reject", std::move(*input)) };
        credential_sink sink {};
        static_cast<void>(runner_->run(request, &sink, cancellation));
        wipe_secret(request.standard_input);
        wipe_secret(sink.output);
    }
} // namespace cyella
