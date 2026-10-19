#include "infrastructure/git_diff_gateway.h"

#include "infrastructure/git_execution_policy.h"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <ios>
#include <system_error>
#include <utility>

namespace cyella {
    namespace {
        std::u8string to_u8(const std::string_view text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        // 저장소 상대 경로(`/` 구분, UTF-8)의 마지막 이름이다.
        std::string base_name(const std::string_view path)
        {
            const std::size_t slash { path.find_last_of('/') };
            return std::string { slash == std::string_view::npos ? path : path.substr(slash + 1) };
        }

        std::string object_spec(const diff_side_spec& side)
        {
            return side.kind == diff_side_kind::revision ? side.revision + ":" + side.path : ":" + side.path;
        }

        // stdout을 가공 없이 받아 파일로 흘리거나 짧은 출력으로 모은다.
        class stdout_sink final : public process_output_sink
        {
        public:
            explicit stdout_sink(std::ofstream* file) noexcept
                : file_ { file }
            {}

            void on_chunk(const process_output_chunk& chunk) override
            {
                if (chunk.stream != process_stream::standard_output)
                    return;
                bytes_ += chunk.bytes.size();
                if (file_ != nullptr)
                    file_->write(chunk.bytes.data(), static_cast<std::streamsize>(chunk.bytes.size()));
                else
                    text_.append(chunk.bytes);
            }

            [[nodiscard]] std::uint64_t bytes() const noexcept
            {
                return bytes_;
            }
            [[nodiscard]] const std::string& text() const noexcept
            {
                return text_;
            }

        private:
            std::ofstream* file_ { nullptr };
            std::uint64_t bytes_ { 0 };
            std::string text_ {};
        };

        process_request make_request(const std::u8string& git_executable, const std::u8string_view repository, std::vector<std::u8string> arguments)
        {
            git_request_options options {};
            options.raw_standard_output = true;
            options.maximum_captured_bytes_per_stream = unlimited_process_capture_bytes;
            return make_git_process_request(git_executable, repository, std::move(arguments), git_command_class::local_query, options);
        }

        struct side_outcome
        {
            extracted_side side {};
            std::string error {};
        };

        // 파일이 없는 쪽에도 빈 파일을 둔다. 도구가 두 인자를 모두 실제 파일로 기대하는 경우가 많다.
        bool write_empty_file(const std::filesystem::path& target)
        {
            std::ofstream file { target, std::ios::binary | std::ios::trunc };
            return file.is_open();
        }
    } // namespace

    extracted_sides git_diff_gateway::extract(const diff_side_plan& plan, const bool write_files)
    {
        extracted_sides result {};
        const diff_side_pair pair { resolve_diff_sides(plan) };

        std::filesystem::path directory {};
        if (write_files)
        {
            directory = temp_.create_operation_directory();
            if (directory.empty())
            {
                result.error = "임시 폴더를 만들지 못했습니다.";
                return result;
            }
        }

        const std::filesystem::path repository { std::filesystem::path { plan.repository_path } };

        const auto extract_side = [&](const diff_side_spec& spec, const char* folder) -> side_outcome {
            side_outcome outcome {};
            std::filesystem::path target {};
            if (write_files)
            {
                const std::string name { base_name(spec.path.empty() ? plan.display_path() : spec.path) };
                std::error_code error {};
                std::filesystem::create_directories(directory / folder, error);
                target = directory / folder / std::filesystem::path { to_u8(name) };
                if (error)
                {
                    outcome.error = "임시 폴더를 만들지 못했습니다.";
                    return outcome;
                }
            }

            if (spec.kind == diff_side_kind::none)
            {
                if (write_files && write_empty_file(target) == false)
                    outcome.error = "임시 파일을 쓰지 못했습니다.";
                outcome.side.file = target.u8string();
                return outcome;
            }

            outcome.side.present = true;
            if (spec.kind == diff_side_kind::worktree)
            {
                const std::filesystem::path source { repository / std::filesystem::path { to_u8(spec.path) } };
                std::error_code error {};
                const std::uintmax_t size { std::filesystem::file_size(source, error) };
                if (error)
                {
                    outcome.error = "작업 트리 파일을 읽지 못했습니다: " + spec.path;
                    return outcome;
                }
                outcome.side.size = size;
                if (write_files)
                {
                    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, error);
                    if (error)
                    {
                        outcome.error = "작업 트리 파일을 복사하지 못했습니다: " + spec.path;
                        return outcome;
                    }
                    outcome.side.file = target.u8string();
                }
                return outcome;
            }

            const std::string object { object_spec(spec) };
            if (write_files)
            {
                std::ofstream file { target, std::ios::binary | std::ios::trunc };
                if (file.is_open() == false)
                {
                    outcome.error = "임시 파일을 쓰지 못했습니다.";
                    return outcome;
                }
                stdout_sink sink { &file };
                const process_result run { runner_.run(make_request(git_executable_, plan.repository_path, { u8"cat-file", u8"blob", to_u8(object) }), &sink, {}) };
                file.close();
                if (run.succeeded() == false)
                {
                    outcome.error = "git cat-file 실패: " + object;
                    return outcome;
                }
                outcome.side.size = sink.bytes();
                outcome.side.file = target.u8string();
                return outcome;
            }

            stdout_sink sink { nullptr };
            const process_result run { runner_.run(make_request(git_executable_, plan.repository_path, { u8"cat-file", u8"-s", to_u8(object) }), &sink, {}) };
            if (run.succeeded() == false)
            {
                outcome.error = "git cat-file 실패: " + object;
                return outcome;
            }
            std::uint64_t size { 0 };
            std::string_view digits { sink.text() };
            while (digits.empty() == false && (digits.back() == '\n' || digits.back() == '\r'))
                digits.remove_suffix(1);
            if (std::from_chars(digits.data(), digits.data() + digits.size(), size).ec == std::errc {})
                outcome.side.size = size;
            return outcome;
        };

        side_outcome left { extract_side(pair.left, "left") };
        side_outcome right { extract_side(pair.right, "right") };
        result.left = std::move(left.side);
        result.right = std::move(right.side);
        if (left.error.empty() == false)
            result.error = std::move(left.error);
        else if (right.error.empty() == false)
            result.error = std::move(right.error);
        result.ok = result.error.empty();
        return result;
    }

    std::optional<std::string> git_diff_gateway::configured_diff_tool(const std::u8string_view repository_path)
    {
        const process_request request { make_request(git_executable_, repository_path, { u8"config", u8"--get", u8"diff.tool" }) };
        stdout_sink sink { nullptr };
        const process_result run { runner_.run(request, &sink, {}) };
        if (run.succeeded() == false)
            return std::nullopt;
        std::string value { sink.text() };
        while (value.empty() == false && (value.back() == '\n' || value.back() == '\r' || value.back() == ' '))
            value.pop_back();
        if (value.empty())
            return std::nullopt;
        return value;
    }
} // namespace cyella
