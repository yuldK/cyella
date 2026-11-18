#include "infrastructure/git_diff_gateway.h"

#include "domain/text_catalog.h"
#include "infrastructure/git_command_builder.h"
#include "infrastructure/git_execution_policy.h"
#include "infrastructure/git_lfs_parser.h"

#include <charconv>
#include <cstdint>
#include <exception>
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

        // Windows가 폴더와 상관없이 장치로 여는 이름인지 본다. `CON.txt`·`nul .png`처럼 확장자나 공백이 붙어도 장치다.
        bool is_reserved_device_name(const std::string_view name)
        {
            std::string stem { name.substr(0, name.find('.')) };
            while (stem.empty() == false && stem.back() == ' ')
                stem.pop_back();
            for (char& letter : stem)
            {
                if (letter >= 'a' && letter <= 'z')
                    letter = static_cast<char>(letter - 'a' + 'A');
            }
            if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" || stem == "CONIN$" || stem == "CONOUT$")
                return true;
            if (stem.starts_with("COM") == false && stem.starts_with("LPT") == false)
                return false;
            // 숫자 하나와 위 첨자 ¹²³(UTF-8)이 붙은 것이 장치다.
            const std::string_view number { std::string_view { stem }.substr(3) };
            return (number.size() == 1 && number[0] >= '0' && number[0] <= '9')
                || number == "\xC2\xB9"
                || number == "\xC2\xB2"
                || number == "\xC2\xB3";
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

    std::string safe_temp_file_name(const std::string_view path)
    {
        // git 경로는 `/`로 나뉘지만 `\`는 Windows에서 폴더 구분이라 `..\..\x`가 위로 올라간다. 둘 다로 자른다.
        const std::size_t separator { path.find_last_of("/\\") };
        std::string name { separator == std::string_view::npos ? path : path.substr(separator + 1) };
        // `:`가 남으면 `C:x`가 드라이브 경로, `a:b`가 대체 데이터 스트림이 된다. 나머지는 Windows가 이름에 받지 않는 글자다.
        for (char& letter : name)
        {
            const unsigned char byte { static_cast<unsigned char>(letter) };
            if (byte < 0x20 || byte == 0x7F || std::string_view { ":*?\"<>|" }.find(letter) != std::string_view::npos)
                letter = '_';
        }
        // Windows는 끝의 점과 공백을 버린다. 남겨 두면 `..`이 위 폴더를, `a.png.`이 다른 이름을 가리킨다.
        while (name.empty() == false && (name.back() == '.' || name.back() == ' '))
            name.pop_back();
        if (name.empty())
            return "file";
        if (is_reserved_device_name(name))
            return "_" + name;
        return name;
    }

    extracted_sides git_diff_gateway::extract(const diff_side_plan& plan, const bool write_files)
    {
        return extract_cancellable(plan, write_files, {});
    }

    extracted_sides git_diff_gateway::extract_cancellable(const diff_side_plan& plan, const bool write_files, const process_cancellation_token& cancellation)
    {
        extracted_sides result {};
        const diff_side_pair pair { resolve_diff_sides(plan) };

        std::filesystem::path directory {};
        if (write_files)
        {
            directory = temp_.create_operation_directory();
            if (directory.empty())
            {
                result.error = tr_narrow(u8"git.diff.temp_folder_failed");
                return result;
            }
        }

        const std::filesystem::path repository { std::filesystem::path { plan.repository_path } };

        // LFS 포인터의 diff면 blob이 포인터 텍스트이므로 로컬 LFS 객체를 대신 꺼낸다. 객체 저장소는 연결된 worktree도
        // 주 저장소의 것을 같이 쓰므로 `--git-common-dir`이다. 처음 필요할 때 한 번만 묻는다.
        std::optional<std::filesystem::path> lfs_storage {};
        const auto storage_of = [&]() -> const std::filesystem::path& {
            if (lfs_storage.has_value() == false)
            {
                stdout_sink sink { nullptr };
                const process_result run { runner_.run(make_request(git_executable_, plan.repository_path, { u8"rev-parse", u8"--path-format=absolute", u8"--git-common-dir" }), &sink, cancellation) };
                std::string text { sink.text() };
                while (text.empty() == false && (text.back() == '\n' || text.back() == '\r'))
                    text.pop_back();
                lfs_storage = run.succeeded() && text.empty() == false ? std::filesystem::path { to_u8(text) } : std::filesystem::path {};
            }
            return *lfs_storage;
        };

        const auto extract_side = [&](const diff_side_spec& spec, const char* folder) -> side_outcome {
            side_outcome outcome {};
            std::filesystem::path target {};
            if (write_files)
            {
                const std::string name { safe_temp_file_name(spec.path.empty() ? plan.display_path() : spec.path) };
                std::error_code error {};
                std::filesystem::create_directories(directory / folder, error);
                target = directory / folder / std::filesystem::path { to_u8(name) };
                if (error)
                {
                    outcome.error = tr_narrow(u8"git.diff.temp_folder_failed");
                    return outcome;
                }
                // 이름을 다듬었어도 한 번 더 본다. 쓰기 전에 열어 자르므로, 작업 폴더 밖을 가리키면 아무것도 열지 않는다.
                if (target.lexically_normal().parent_path() != (directory / folder).lexically_normal())
                {
                    outcome.error = tr_narrow(u8"git.diff.temp_file_failed");
                    return outcome;
                }
            }

            if (spec.kind == diff_side_kind::none)
            {
                if (write_files && write_empty_file(target) == false)
                    outcome.error = tr_narrow(u8"git.diff.temp_file_failed");
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
                    outcome.error = tr_narrow(u8"git.diff.worktree_file_unreadable", { { u8"path", spec.path } });
                    return outcome;
                }
                outcome.side.size = size;
                if (write_files)
                {
                    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, error);
                    if (error)
                    {
                        outcome.error = tr_narrow(u8"git.diff.worktree_file_copy_failed", { { u8"path", spec.path } });
                        return outcome;
                    }
                    outcome.side.file = target.u8string();
                }
                return outcome;
            }

            const std::string object { object_spec(spec) };
            if (plan.lfs)
            {
                // 포인터면 로컬 객체가 이 쪽의 실제 내용이다. 포인터가 아니면(LFS로 옮기기 전의 일반 내용) 아래의 blob 경로다.
                stdout_sink pointer_sink { nullptr };
                const process_result pointer_run { runner_.run(make_request(git_executable_, plan.repository_path, { u8"cat-file", u8"blob", to_u8(object) }), &pointer_sink, cancellation) };
                if (pointer_run.succeeded())
                    if (const std::optional<lfs_pointer> pointer { parse_lfs_pointer(pointer_sink.text()) }; pointer.has_value())
                    {
                        const std::string relative { lfs_object_relative_path(pointer->oid) };
                        const std::filesystem::path& storage { storage_of() };
                        const std::filesystem::path source { storage / std::filesystem::path { to_u8(relative) } };
                        std::error_code error {};
                        const std::uintmax_t size { relative.empty() || storage.empty() ? 0 : std::filesystem::file_size(source, error) };
                        if (relative.empty() || storage.empty() || error)
                        {
                            outcome.error = tr_narrow(u8"git.diff.lfs_object_missing", { { u8"path", spec.path } });
                            return outcome;
                        }
                        outcome.side.size = size;
                        if (write_files)
                        {
                            std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, error);
                            if (error)
                            {
                                outcome.error = tr_narrow(u8"git.diff.lfs_object_copy_failed", { { u8"path", spec.path } });
                                return outcome;
                            }
                            outcome.side.file = target.u8string();
                        }
                        return outcome;
                    }
            }
            if (write_files)
            {
                std::ofstream file { target, std::ios::binary | std::ios::trunc };
                if (file.is_open() == false)
                {
                    outcome.error = tr_narrow(u8"git.diff.temp_file_failed");
                    return outcome;
                }
                stdout_sink sink { &file };
                const process_result run { runner_.run(make_request(git_executable_, plan.repository_path, { u8"cat-file", u8"blob", to_u8(object) }), &sink, cancellation) };
                file.close();
                if (run.succeeded() == false)
                {
                    outcome.error = tr_narrow(u8"git.diff.cat_file_failed", { { u8"object", object } });
                    return outcome;
                }
                outcome.side.size = sink.bytes();
                outcome.side.file = target.u8string();
                return outcome;
            }

            stdout_sink sink { nullptr };
            const process_result run { runner_.run(make_request(git_executable_, plan.repository_path, { u8"cat-file", u8"-s", to_u8(object) }), &sink, cancellation) };
            if (run.succeeded() == false)
            {
                outcome.error = tr_narrow(u8"git.diff.cat_file_failed", { { u8"object", object } });
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

        // UTF-8이 아닌 경로 바이트(다른 OS에서 만든 커밋의 Latin-1 이름 등)는 `std::filesystem::path`로 바꿀 때 던진다.
        // worker 밖으로 새면 앱이 끝나므로 그 쪽은 임시 파일을 만들지 못한 것으로 돌려준다.
        const auto guarded_side = [&](const diff_side_spec& spec, const char* folder) -> side_outcome {
            try
            {
                return extract_side(spec, folder);
            }
            catch (const std::exception&)
            {
                side_outcome failed {};
                failed.error = tr_narrow(u8"git.diff.temp_file_failed");
                return failed;
            }
        };
        side_outcome left { guarded_side(pair.left, "left") };
        side_outcome right { guarded_side(pair.right, "right") };
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

    std::optional<std::string> git_diff_gateway::empty_tree(const std::u8string_view repository_path)
    {
        // 표준 입력을 주지 않으면 NUL 장치라 빈 입력이다. 객체는 쓰지 않는다(`-w`가 없다).
        const process_request request { make_request(git_executable_, repository_path, make_git_empty_tree_args()) };
        stdout_sink sink { nullptr };
        const process_result run { runner_.run(request, &sink, {}) };
        if (run.succeeded() == false)
            return std::nullopt;
        std::string value { sink.text() };
        while (value.empty() == false && (value.back() == '\n' || value.back() == '\r'))
            value.pop_back();
        if (value.size() != 40 && value.size() != 64)
            return std::nullopt;
        return value;
    }

    void git_diff_gateway::release(const extracted_sides& sides) noexcept
    {
        // 꺼낸 파일은 `<작업 폴더>/left|right/<이름>`이다. 그 작업 폴더를 지운다. 경로를 다루다 던지면(할당 실패) 앱이
        // 끝날 때의 정리에 맡긴다.
        try
        {
            for (const extracted_side* const side : { &sides.left, &sides.right })
            {
                if (side->file.empty())
                    continue;
                temp_.remove_operation_directory(std::filesystem::path { side->file }.parent_path().parent_path());
            }
        }
        catch (...)
        {
            // 지우지 못한 폴더는 앱이 끝날 때 `diff_temp_directory`가 지운다.
        }
    }
} // namespace cyella
