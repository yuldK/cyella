#include "infrastructure/git_command_runner.h"

#include <utility>

namespace cyella {
    namespace {
        std::u8string join_lines(const std::vector<std::u8string>& lines)
        {
            std::u8string result {};
            for (const std::u8string& line : lines)
            {
                if (result.empty() == false)
                    result.push_back(u8'\n');
                result.append(line);
            }
            return result;
        }
    } // namespace

    bool git_command_result::succeeded() const noexcept
    {
        return process.completion == process_completion::exited && process.exit_code.has_value() && *process.exit_code == 0;
    }

    std::u8string git_command_result::standard_output_text() const
    {
        return join_lines(standard_output_lines);
    }

    std::u8string git_command_result::standard_error_text() const
    {
        return join_lines(standard_error_lines);
    }

    std::u8string git_command_result::first_output_line() const
    {
        return standard_output_lines.empty() ? std::u8string {} : standard_output_lines.front();
    }

    git_output_collector::git_output_collector(process_output_sink* const forward) noexcept
        : forward_ { forward }
    {}

    void git_output_collector::on_record(const process_output_record& record)
    {
        if (record.stream == process_stream::standard_error)
            standard_error_.push_back(record.text);
        else
            standard_output_.push_back(record.text);

        if (forward_ != nullptr)
            forward_->on_record(record);
    }

    void git_output_collector::on_chunk(const process_output_chunk& chunk)
    {
        // 조각은 모으지 않고 넘기기만 한다. 줄 단위 수집은 `lines` 스트림의 몫이다.
        if (forward_ != nullptr)
            forward_->on_chunk(chunk);
    }

    std::vector<std::u8string> git_output_collector::take_standard_output() noexcept
    {
        return std::move(standard_output_);
    }

    std::vector<std::u8string> git_output_collector::take_standard_error() noexcept
    {
        return std::move(standard_error_);
    }

    git_command_result run_git_command(process_runner& runner, const process_request& request, const process_cancellation_token& token, process_output_sink* const forward) noexcept
    {
        try
        {
            git_output_collector collector { forward };
            git_command_result result {};
            result.process = runner.run(request, &collector, token);
            result.standard_output_lines = collector.take_standard_output();
            result.standard_error_lines = collector.take_standard_error();
            return result;
        }
        catch (...)
        {
            // 출력 수집에서 메모리 확보에 실패한 경우다. 실행 결과를 신뢰할 수 없으므로
            // 상위 계층이 실패로 다루도록 `internal_error`로 보고한다.
            git_command_result result {};
            result.process.completion = process_completion::internal_error;
            return result;
        }
    }
} // namespace cyella
