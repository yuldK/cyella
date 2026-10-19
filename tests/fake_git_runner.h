#pragma once

#include "application/git_operation_executor.h"
#include "application/operation_events.h"
#include "application/operation_executor.h"
#include "application/process_runner.h"
#include "application/workspace_files.h"
#include "domain/path_syntax.h"
#include "infrastructure/git_execution_policy.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 실행기·세션 단위 test가 쓰는 가짜들이다. 실제 git과 디스크 없이 명령 순서, 사건, 상태 전이를
// 결정적으로 검증한다.
namespace cyella::test {
    using text_list = std::vector<std::string>;

    // 가짜 git 한 번의 응답이다.
    struct scripted_response
    {
        std::int32_t exit_code { 0 };
        // raw stdout이다. 조각 경계를 흔들려고 반으로 나눠 두 번 `on_chunk`로 보낸다.
        std::string standard_output {};
        std::vector<std::string> standard_error_lines {};
        // `\r`로 끝난 진행 줄처럼 `progress`가 켜진 stderr 레코드다.
        std::vector<std::string> progress_lines {};
        process_completion completion { process_completion::exited };
        // 응답하기 전에 부른다. 다른 job과 겹치는 순간을 만들 때 막아 둔다.
        std::function<void()> before_reply {};
    };

    // 공통 앞 인자(`-c ...`, `--no-pager`, `--no-optional-locks`)를 뗀 git 하위 명령 인자다.
    struct recorded_call
    {
        text_list arguments {};
        std::string working_directory {};
        std::string standard_input {};
        bool no_optional_locks { false };
        // 자식에게 건넨 환경 변수 override다. 값이 없는 항목은 부모 환경에서 지우라는 뜻이다.
        std::vector<process_environment_override> environment {};

        // 설정한 변수의 값이다. 설정하지 않았거나 지우도록 했으면 값이 없다.
        [[nodiscard]] std::optional<std::string> variable(const std::u8string_view name) const
        {
            for (const process_environment_override& entry : environment)
                if (entry.name == name)
                    return entry.value.has_value() ? std::optional<std::string> { std::string { entry.value->begin(), entry.value->end() } } : std::nullopt;
            return std::nullopt;
        }
    };

    // 명령의 앞 토큰으로 응답을 고르는 가짜 runner다. 규칙은 등록 순서로 찾고, `once`는 한 번
    // 쓰면 지운다. 맞는 규칙이 없으면 종료 코드 0, 빈 출력이다.
    class fake_git_runner final : public process_runner
    {
    public:
        void when(text_list prefix, scripted_response response)
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            rules_.push_back(rule { std::move(prefix), std::move(response), false });
        }

        void when_once(text_list prefix, scripted_response response)
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            rules_.push_back(rule { std::move(prefix), std::move(response), true });
        }

        [[nodiscard]] std::vector<recorded_call> calls() const
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            return calls_;
        }

        // 하위 명령 이름만 순서대로 모은다(`status`, `switch` ...). `-c core.editor=true`처럼 하위
        // 명령 앞의 인자는 건너뛴다.
        [[nodiscard]] text_list command_names() const
        {
            text_list names {};
            for (const recorded_call& call : calls())
            {
                std::size_t index { 0 };
                while (index < call.arguments.size() && (call.arguments[index] == "-c" || call.arguments[index] == "--literal-pathspecs"))
                    index += call.arguments[index] == "-c" ? 2 : 1;
                names.push_back(index < call.arguments.size() ? call.arguments[index] : std::string {});
            }
            return names;
        }

        [[nodiscard]] process_result run(const process_request& request, process_output_sink* const sink, const process_cancellation_token& token) noexcept override
        {
            recorded_call call {};
            call.working_directory = to_text(request.working_directory);
            call.standard_input = request.standard_input;
            call.environment = request.environment_overrides;
            const std::vector<std::u8string> common { git_common_arguments() };
            std::size_t index { 0 };
            while (index < common.size() && index < request.arguments.size() && request.arguments[index] == common[index])
                ++index;
            if (index < request.arguments.size() && request.arguments[index] == u8"--no-optional-locks")
            {
                call.no_optional_locks = true;
                ++index;
            }
            for (; index < request.arguments.size(); ++index)
                call.arguments.push_back(to_text(request.arguments[index]));

            scripted_response response {};
            {
                const std::lock_guard<std::mutex> lock { mutex_ };
                calls_.push_back(call);
                for (auto rule { rules_.begin() }; rule != rules_.end(); ++rule)
                {
                    if (starts_with(call.arguments, rule->prefix) == false)
                        continue;
                    response = rule->response;
                    if (rule->once)
                        rules_.erase(rule);
                    break;
                }
            }
            if (response.before_reply != nullptr)
                response.before_reply();

            process_result result {};
            std::u8string line { u8"git" };
            for (const std::string& argument : call.arguments)
                line.append(u8" ").append(to_u8(argument));
            result.masked_command_line = line;
            if (token.cancelled())
            {
                result.completion = process_completion::cancelled;
                return result;
            }
            result.completion = response.completion;
            if (response.completion == process_completion::exited)
                result.exit_code = response.exit_code;

            if (sink != nullptr)
            {
                std::uint64_t sequence { 0 };
                const std::string_view bytes { response.standard_output };
                const std::size_t half { bytes.size() / 2 };
                for (const std::string_view piece : { bytes.substr(0, half), bytes.substr(half) })
                {
                    if (piece.empty())
                        continue;
                    process_output_chunk chunk {};
                    chunk.sequence = sequence++;
                    chunk.stream = process_stream::standard_output;
                    chunk.bytes = piece;
                    sink->on_chunk(chunk);
                }
                for (const std::string& text : response.progress_lines)
                {
                    process_output_record record {};
                    record.sequence = sequence++;
                    record.stream = process_stream::standard_error;
                    record.text = to_u8(text);
                    record.progress = true;
                    sink->on_record(record);
                }
                for (const std::string& text : response.standard_error_lines)
                {
                    process_output_record record {};
                    record.sequence = sequence++;
                    record.stream = process_stream::standard_error;
                    record.text = to_u8(text);
                    sink->on_record(record);
                }
            }
            return result;
        }

    private:
        struct rule
        {
            text_list prefix {};
            scripted_response response {};
            bool once { false };
        };

        static bool starts_with(const text_list& arguments, const text_list& prefix)
        {
            if (prefix.size() > arguments.size())
                return false;
            for (std::size_t index { 0 }; index < prefix.size(); ++index)
                if (arguments[index] != prefix[index])
                    return false;
            return true;
        }

        static std::string to_text(const std::u8string_view text)
        {
            return std::string { text.begin(), text.end() };
        }

        static std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        mutable std::mutex mutex_ {};
        std::vector<rule> rules_ {};
        std::vector<recorded_call> calls_ {};
    };

    // 메모리 안의 파일이다. 경로는 대소문자·구분자를 무시하고 비교한다. 폴더는 그 아래 파일이
    // 있거나 `add_directory`로 넣은 경로다.
    class fake_workspace_files final : public workspace_files
    {
    public:
        void put(const std::u8string_view path, std::string content)
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            files_[normalize_path_key(path)] = std::move(content);
        }

        void add_directory(const std::u8string_view path)
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            directories_.push_back(normalize_path_key(path));
        }

        [[nodiscard]] std::optional<std::string> content(const std::u8string_view path) const
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            const auto found { files_.find(normalize_path_key(path)) };
            if (found == files_.end())
                return std::nullopt;
            return found->second;
        }

        [[nodiscard]] path_kind probe(const std::u8string_view absolute_path) const noexcept override
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            const std::u8string key { normalize_path_key(absolute_path) };
            if (files_.contains(key))
                return path_kind::file;
            for (const std::u8string& directory : directories_)
                if (directory == key)
                    return path_kind::directory;
            const std::u8string prefix { key + u8"/" };
            for (const auto& [path, content] : files_)
                if (path.starts_with(prefix))
                    return path_kind::directory;
            return path_kind::missing;
        }

        [[nodiscard]] std::optional<std::string> read_text(const std::u8string_view absolute_path, const std::size_t maximum_bytes) const noexcept override
        {
            const std::optional<std::string> found { content(absolute_path) };
            if (found.has_value() == false || found->size() > maximum_bytes)
                return std::nullopt;
            return found;
        }

        [[nodiscard]] bool append_text(const std::u8string_view absolute_path, const std::string_view text) noexcept override
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            files_[normalize_path_key(absolute_path)].append(text);
            return true;
        }

        [[nodiscard]] bool remove_file(const std::u8string_view absolute_path) noexcept override
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            return files_.erase(normalize_path_key(absolute_path)) != 0;
        }

    private:
        mutable std::mutex mutex_ {};
        std::map<std::u8string, std::string> files_ {};
        std::vector<std::u8string> directories_ {};
    };

    // worker 사건을 모았다가 test 스레드에서 한꺼번에 넘긴다. relay와 같은 역할이다.
    class event_queue
    {
    public:
        [[nodiscard]] operation_event_sink sink()
        {
            return [this](operation_event event) {
                const std::lock_guard<std::mutex> lock { mutex_ };
                events_.push_back(std::move(event));
            };
        }

        [[nodiscard]] std::vector<operation_event> take()
        {
            const std::lock_guard<std::mutex> lock { mutex_ };
            std::vector<operation_event> taken { std::move(events_) };
            events_ = {};
            return taken;
        }

    private:
        std::mutex mutex_ {};
        std::vector<operation_event> events_ {};
    };

    // 접수한 job을 test가 원할 때 하나씩 실행하는 dispatcher다. 실행 순서를 test가 정한다.
    class manual_dispatcher
    {
    public:
        [[nodiscard]] std::function<bool(operation_request)> dispatcher()
        {
            return [this](operation_request request) {
                pending_.push_back(std::move(request));
                return true;
            };
        }

        [[nodiscard]] std::size_t pending() const noexcept
        {
            return pending_.size();
        }

        [[nodiscard]] const operation_request& peek(const std::size_t index) const
        {
            return pending_.at(index);
        }

        // 맨 앞(또는 index번째) job을 꺼내 실행한다.
        bool run(const std::size_t index = 0)
        {
            if (index >= pending_.size())
                return false;
            operation_request request { std::move(pending_[index]) };
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(index));
            const operation_context context { request.id, request.repository_key, shutdown_.token() };
            request.job(context);
            return true;
        }

    private:
        std::deque<operation_request> pending_ {};
        process_cancellation_source shutdown_ {};
    };

    // ------------------------------------------------------------------ 가짜 git 출력

    inline std::string log_record(const std::string& sha, const text_list& parents, const std::string& subject = "subject", const bool with_body = false)
    {
        std::string record { sha };
        record.push_back('\x1f');
        for (std::size_t index { 0 }; index < parents.size(); ++index)
        {
            if (index != 0)
                record.push_back(' ');
            record.append(parents[index]);
        }
        record.append("\x1f"
                      "Ann Author\x1f"
                      "ann@example.com\x1f"
                      "1775000000\x1f"
                      "1775000000\x1f");
        record.append(subject);
        // `show_commit`처럼 본문을 요청한 형식은 빈 본문 필드가 하나 더 있다.
        if (with_body)
            record.push_back('\x1f');
        record.push_back('\0');
        return record;
    }

    inline std::string status_header(const std::string& head_sha, const std::string& branch, const std::string& upstream = {})
    {
        std::string bytes { "# branch.oid " + head_sha };
        bytes.push_back('\0');
        bytes.append("# branch.head " + branch);
        bytes.push_back('\0');
        if (upstream.empty() == false)
        {
            bytes.append("# branch.upstream " + upstream);
            bytes.push_back('\0');
            bytes.append("# branch.ab +0 -0");
            bytes.push_back('\0');
        }
        return bytes;
    }

    inline std::string status_modified(const std::string& path)
    {
        std::string record { "1 .M N... 100644 100644 100644 1111111111111111111111111111111111111111 1111111111111111111111111111111111111111 " + path };
        record.push_back('\0');
        return record;
    }

    inline std::string status_conflict(const std::string& path)
    {
        std::string record { "u UU N... 100644 100644 100644 100644 " };
        record.append("1111111111111111111111111111111111111111 2222222222222222222222222222222222222222 3333333333333333333333333333333333333333 ");
        record.append(path);
        record.push_back('\0');
        return record;
    }

    inline std::string sha_of(const int value)
    {
        std::string text { std::to_string(value) };
        return std::string(40 - text.size(), 'a') + text;
    }
} // namespace cyella::test
