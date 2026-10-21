#include "infrastructure/git_interactive_rebase.h"

#include "domain/ref_name.h"
#include "domain/text_catalog.h"
#include "infrastructure/git_log_parser.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace cyella {
    namespace {
        // git_command_builder의 `log_format_with_body`와 같은 형식이다. git_log_parser(has_body)가 읽는다.
        constexpr std::string_view range_log_format { "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%ct%x1f%s%x1f%b" };
        // git `run-command.c`의 SHELL_META다. 하나라도 있으면 git이 편집기를 `sh -c`로 실행한다.
        constexpr std::u8string_view shell_meta { u8"|&;<>()$`\\\"' \t\n*?[#~=%" };
        constexpr std::string_view state_folder { "cyella-rebase" };
        // 비정상 종료로 남은 준비 폴더를 이만큼 지나면 다음 rebase가 지운다.
        constexpr std::chrono::hours stale_state_age { 24 };

        void push_text(git_arguments& arguments, const std::string_view value)
        {
            arguments.emplace_back(value.begin(), value.end());
        }

        std::string require_revision(const std::string_view value, const std::string_view what)
        {
            if (const auto problem { find_revision_problem(value) })
            {
                std::string message { what };
                message.append(": ").append(ref_name_problem_text(*problem));
                throw std::invalid_argument { message };
            }
            return std::string { value };
        }

        std::string head_of(const std::string_view branch)
        {
            return branch.empty() ? std::string { "HEAD" } : require_revision(branch, "branch");
        }

        std::vector<std::string> split_lines(const std::string_view text)
        {
            std::vector<std::string> lines {};
            std::string_view rest { text };
            while (rest.empty() == false)
            {
                const std::size_t end { rest.find('\n') };
                std::string_view line { rest.substr(0, end) };
                if (line.empty() == false && line.back() == '\r')
                    line.remove_suffix(1);
                if (line.empty() == false)
                    lines.emplace_back(line);
                if (end == std::string_view::npos)
                    break;
                rest.remove_prefix(end + 1);
            }
            return lines;
        }

        std::string trimmed_body(const std::optional<std::string>& body)
        {
            if (body.has_value() == false)
                return {};
            std::string_view text { *body };
            while (text.empty() == false && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
                text.remove_suffix(1);
            while (text.empty() == false && (text.front() == '\n' || text.front() == '\r'))
                text.remove_prefix(1);
            return std::string { text };
        }

        std::filesystem::path path_of(const std::u8string_view text)
        {
            return std::filesystem::path { std::u8string { text } };
        }

        bool write_file(const std::filesystem::path& path, const std::string_view text)
        {
            std::ofstream stream { path, std::ios::binary | std::ios::trunc };
            if (stream.is_open() == false)
                return false;
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            stream.close();
            return stream.fail() == false;
        }

        // 편집기 도우미가 읽을 준비 폴더다. 소멸할 때 지운다. git이 rebase를 시작하면 도우미가 메시지를
        // `rebase-merge/cyella`로 옮겨 두므로 이 폴더는 첫 명령이 끝나면 필요 없다.
        class rebase_state_directory
        {
        public:
            rebase_state_directory(const std::u8string_view root, const operation_id id)
            {
                std::error_code error {};
                std::filesystem::path base { root.empty() ? std::filesystem::temp_directory_path(error) : path_of(root) };
                if (error)
                    return;
                base /= state_folder;
                sweep_stale(base);
                static std::atomic<std::uint64_t> counter { 0 };
                const auto ticks { std::chrono::steady_clock::now().time_since_epoch().count() };
                const std::string name { std::to_string(id) + "-" + std::to_string(ticks) + "-" + std::to_string(counter.fetch_add(1)) };
                std::filesystem::path candidate { base / name };
                std::filesystem::create_directories(candidate / "messages", error);
                if (error)
                    return;
                path_ = std::move(candidate);
            }

            rebase_state_directory(const rebase_state_directory&) = delete;
            rebase_state_directory& operator=(const rebase_state_directory&) = delete;

            ~rebase_state_directory()
            {
                if (path_.empty())
                    return;
                std::error_code error {};
                std::filesystem::remove_all(path_, error);
            }

            [[nodiscard]] const std::filesystem::path& path() const noexcept
            {
                return path_;
            }

        private:
            static void sweep_stale(const std::filesystem::path& base) noexcept
            {
                std::error_code error {};
                if (std::filesystem::is_directory(base, error) == false)
                    return;
                const auto now { std::filesystem::file_time_type::clock::now() };
                for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator { base, error })
                {
                    std::error_code entry_error {};
                    const auto written { entry.last_write_time(entry_error) };
                    if (entry_error || now - written < stale_state_age)
                        continue;
                    std::filesystem::remove_all(entry.path(), entry_error);
                }
            }

            std::filesystem::path path_ {};
        };

        struct loaded_range
        {
            std::vector<commit_info> commits {};
            std::uint32_t merge_count { 0 };
        };

        // 범위의 커밋과 병합 커밋 수를 읽는다. 실패하면 `stop`에 끝맺음을 넣고 false다.
        bool load_range(git_operation_context& context, const std::string_view onto, const std::string_view branch, loaded_range& range, std::optional<operation_outcome>& stop)
        {
            git_command_outcome outcome {};
            git_command_spec log {};
            log.arguments = make_git_rebase_range_args(onto, branch);
            if (context.run_required(std::move(log), outcome, stop) == false)
                return false;
            range.commits = parse_git_log(outcome.standard_output, true);

            git_command_spec merges {};
            merges.arguments = make_git_rebase_merge_count_args(onto, branch);
            if (context.run_required(std::move(merges), outcome, stop) == false)
                return false;
            const std::vector<std::string> lines { split_lines(outcome.standard_output) };
            range.merge_count = lines.empty() ? 0u : static_cast<std::uint32_t>(std::strtoul(lines.front().c_str(), nullptr, 10));
            return true;
        }

        std::string resolve_commit(git_operation_context& context, const std::string_view revision)
        {
            git_command_spec spec {};
            spec.arguments = make_git_resolve_commit_args(revision);
            spec.accepted_exit_codes = { 0, 1 };
            const git_command_outcome outcome { context.run(std::move(spec)) };
            if (outcome.accepted == false || outcome.exit_code() != 0)
                return {};
            const std::vector<std::string> lines { split_lines(outcome.standard_output) };
            return lines.empty() ? std::string {} : lines.front();
        }
    } // namespace

    std::u8string make_editor_command(const std::u8string_view helper_path)
    {
        std::u8string path { helper_path };
        std::replace(path.begin(), path.end(), u8'\\', u8'/');
        if (path.find_first_of(shell_meta) == std::u8string::npos)
            return path;
        std::u8string quoted { u8"'" };
        for (const char8_t value : path)
            if (value == u8'\'')
                quoted.append(u8"'\\''");
            else
                quoted.push_back(value);
        quoted.push_back(u8'\'');
        return quoted;
    }

    git_arguments make_git_rebase_range_args(const std::string_view onto, const std::string_view branch)
    {
        git_arguments arguments {};
        for (const std::string_view value : { "log", "--reverse", "--topo-order", "--max-parents=1", "--right-only", "--cherry-pick", "-z" })
            push_text(arguments, value);
        push_text(arguments, range_log_format);
        push_text(arguments, require_revision(onto, "onto") + "..." + head_of(branch));
        push_text(arguments, "--");
        return arguments;
    }

    git_arguments make_git_rebase_merge_count_args(const std::string_view onto, const std::string_view branch)
    {
        git_arguments arguments {};
        for (const std::string_view value : { "rev-list", "--count", "--merges" })
            push_text(arguments, value);
        push_text(arguments, require_revision(onto, "onto") + ".." + head_of(branch));
        push_text(arguments, "--");
        return arguments;
    }

    git_arguments make_git_rebase_unpushed_args(const std::string_view onto, const std::string_view branch)
    {
        git_arguments arguments {};
        push_text(arguments, "rev-list");
        push_text(arguments, require_revision(onto, "onto") + ".." + head_of(branch));
        for (const std::string_view value : { "--not", "--remotes", "--" })
            push_text(arguments, value);
        return arguments;
    }

    git_arguments make_git_resolve_commit_args(const std::string_view revision)
    {
        git_arguments arguments {};
        for (const std::string_view value : { "rev-parse", "--verify", "--quiet", "--end-of-options" })
            push_text(arguments, value);
        push_text(arguments, require_revision(revision, "revision") + "^{commit}");
        return arguments;
    }

    git_arguments make_git_interactive_rebase_args(const std::string_view onto, const std::string_view branch, const bool autostash)
    {
        git_arguments arguments {};
        for (const std::string_view value : { "rebase", "-i", "--empty=drop", "--no-autosquash", "--no-update-refs" })
            push_text(arguments, value);
        if (autostash)
            push_text(arguments, "--autostash");
        push_text(arguments, "--");
        push_text(arguments, require_revision(onto, "onto"));
        if (branch.empty() == false)
            push_text(arguments, require_revision(branch, "branch"));
        return arguments;
    }

    operation_outcome run_load_rebase_plan(git_operation_context& context, const load_rebase_plan_request& request)
    {
        loaded_range range {};
        std::optional<operation_outcome> stop {};
        if (load_range(context, request.onto, request.branch, range, stop) == false)
            return *stop;

        rebase_plan plan {};
        plan.onto = request.onto;
        plan.branch = request.branch;
        plan.onto_sha = resolve_commit(context, request.onto);
        plan.head_sha = resolve_commit(context, request.branch.empty() ? std::string_view { "HEAD" } : std::string_view { request.branch });
        plan.merge_count = range.merge_count;

        // 원격 브랜치가 하나도 없으면 `--not --remotes`가 아무것도 빼지 않아 모두 "올라가지 않음"이다.
        git_command_outcome outcome {};
        git_command_spec unpushed {};
        unpushed.arguments = make_git_rebase_unpushed_args(request.onto, request.branch);
        if (context.run_required(std::move(unpushed), outcome, stop) == false)
            return *stop;
        const std::vector<std::string> local_only { split_lines(outcome.standard_output) };
        const std::set<std::string> local { local_only.begin(), local_only.end() };

        for (commit_info& commit : range.commits)
        {
            rebase_commit entry {};
            entry.sha = std::move(commit.sha);
            entry.subject = std::move(commit.subject);
            entry.body = trimmed_body(commit.body);
            entry.author_name = std::move(commit.author_name);
            entry.author_time = commit.author_time;
            entry.pushed = local.contains(entry.sha) == false;
            if (entry.pushed)
                plan.has_remote_branches = true;
            plan.commits.push_back(std::move(entry));
        }
        context.emit(rebase_plan_loaded { std::move(plan) });
        return operation_outcome::succeeded();
    }

    operation_outcome run_interactive_rebase(git_operation_context& context, const interactive_rebase_request& request)
    {
        const std::u8string& helper { context.owner().options().rebase_editor_helper };
        if (helper.empty())
            return operation_outcome::failed(failure_kind::other, tr(u8"git.rebase.editor_helper_missing"));

        const std::string git_dir { context.read_git_dir() };
        if (git_dir.empty())
            return operation_outcome::failed(failure_kind::not_found, tr(u8"git.not_a_repository"));
        if (context.read_in_progress(git_dir).kind != repository_operation_state::none)
            return operation_outcome::failed(failure_kind::refused, tr(u8"git.rebase.operation_in_progress"));

        loaded_range range {};
        std::optional<operation_outcome> stop {};
        if (load_range(context, request.onto, request.branch, range, stop) == false)
            return *stop;
        if (range.merge_count != 0 && request.allow_flatten_merges == false)
            return operation_outcome::failed(failure_kind::refused, tr(u8"git.rebase.merge_commits_in_range", { { u8"count", range.merge_count } }));

        std::vector<std::string> range_shas {};
        range_shas.reserve(range.commits.size());
        for (const commit_info& commit : range.commits)
            range_shas.push_back(commit.sha);
        if (const auto issue { find_rebase_todo_problem(request.todo, range_shas) })
            return operation_outcome::failed(failure_kind::invalid_request, rebase_todo_problem_text(*issue));

        const rebase_state_directory state { context.owner().options().rebase_state_root, context.job().id };
        if (state.path().empty())
            return operation_outcome::failed(failure_kind::other, tr(u8"git.rebase.temp_folder_failed"));
        std::string expected {};
        for (const std::string& sha : range_shas)
            expected.append(sha).push_back('\n');
        bool written { write_file(state.path() / "todo", serialize_rebase_todo(request.todo)) && write_file(state.path() / "expected", expected) };
        for (const rebase_message_file& file : rebase_message_files(request.todo))
            written = written && is_hex_object_id(file.sha) && write_file(state.path() / "messages" / file.sha, file.message);
        if (written == false)
            return operation_outcome::failed(failure_kind::other, tr(u8"git.rebase.prepare_files_failed"));

        const std::u8string editor { make_editor_command(helper) };
        git_command_spec spec {};
        spec.arguments = make_git_interactive_rebase_args(request.onto, request.branch, request.autostash);
        spec.command_class = git_command_class::local_write;
        spec.environment = {
            process_environment_override { u8"GIT_SEQUENCE_EDITOR", editor },
            process_environment_override { u8"GIT_EDITOR", editor },
            process_environment_override { u8"CYELLA_REBASE_STATE", state.path().u8string() },
        };
        const git_command_outcome outcome { context.run(std::move(spec)) };

        std::error_code error {};
        if (std::filesystem::exists(state.path() / "mismatch", error))
            return operation_outcome::failed(failure_kind::refused, tr(u8"git.rebase.commits_changed"));

        const rebase_preview preview { preview_rebase(request.todo, {}) };
        std::u8string summary { tr(u8"git.rebase.finished", { { u8"before", request.todo.size() }, { u8"after", preview.commits.size() } }) };
        return context.finish_sequence(outcome, std::move(summary));
    }

    std::vector<process_environment_override> interactive_rebase_continue_environment(git_operation_context& context)
    {
        const std::u8string& helper { context.owner().options().rebase_editor_helper };
        if (helper.empty())
            return {};
        const std::string git_dir { context.read_git_dir() };
        if (git_dir.empty())
            return {};
        if (context.owner().files().probe(join_path(to_utf8(git_dir), "rebase-merge/cyella/active")) == path_kind::missing)
            return {};
        return { process_environment_override { u8"GIT_EDITOR", make_editor_command(helper) } };
    }
} // namespace cyella
