#include "infrastructure/git_extension_operations.h"
#include "infrastructure/git_lfs_commands.h"
#include "infrastructure/git_lfs_parser.h"

#include <algorithm>
#include <optional>
#include <set>
#include <utility>

namespace cyella {
    namespace {
        // 포인터 파일 하나는 1 KiB보다 작다. 이보다 조금 넉넉히 읽어 포인터인지 가린다.
        constexpr std::size_t pointer_probe_bytes { 2048 };
        constexpr std::size_t attributes_limit { 1024u * 1024u };

        std::string trimmed_line(std::string text)
        {
            while (text.empty() == false && (text.back() == '\n' || text.back() == '\r'))
                text.pop_back();
            return text;
        }

        // git-lfs가 진행률을 stdout으로 내려면 터미널이 아니어도 내라고 알려야 한다.
        std::vector<process_environment_override> lfs_progress_environment()
        {
            return { process_environment_override { u8"GIT_LFS_FORCE_PROGRESS", u8"1" } };
        }

        // git-lfs를 실행할 수 있는가. 아니면 실패 끝맺음을 `stop`에 넣는다.
        bool require_lfs(git_operation_context& context, std::optional<operation_outcome>& stop)
        {
            git_command_spec spec {};
            spec.arguments = make_git_lfs_version_args();
            spec.command_class = git_command_class::tool_probe;
            spec.accepted_exit_codes = { 0, 1 };
            const git_command_outcome outcome { context.run(std::move(spec)) };
            if (outcome.process.completion != process_completion::exited)
            {
                stop = context.failure_from(outcome);
                return false;
            }
            if (outcome.exit_code() != 0 || parse_git_lfs_version(outcome.standard_output).has_value() == false)
            {
                stop = operation_outcome::failed(failure_kind::not_found, u8"Git LFS(git-lfs)가 설치되어 있지 않습니다.");
                return false;
            }
            return true;
        }

        // `git lfs install --local`이 되어 있는가. 설정이 없으면 pull은 "Skipping object checkout"만 쓰고 성공으로 끝난다.
        bool lfs_filter_installed(git_operation_context& context)
        {
            git_command_spec spec {};
            spec.arguments = make_git_lfs_filter_probe_args();
            spec.accepted_exit_codes = { 0, 1 };
            const git_command_outcome outcome { context.run(std::move(spec)) };
            return outcome.accepted && outcome.exit_code() == 0;
        }

        // LFS 객체 저장소를 가진 git 디렉터리다. 연결된 worktree도 주 저장소의 것을 같이 쓴다.
        std::u8string lfs_storage_directory(git_operation_context& context)
        {
            git_command_spec spec {};
            spec.arguments = { u8"rev-parse", u8"--path-format=absolute", u8"--git-common-dir" };
            const git_command_outcome outcome { context.run(std::move(spec)) };
            if (outcome.accepted)
            {
                const std::string path { trimmed_line(outcome.standard_output) };
                if (path.empty() == false)
                    return to_utf8(path);
            }
            return to_utf8(context.read_git_dir());
        }

        bool object_exists(git_operation_context& context, const std::u8string& storage, const std::string& oid)
        {
            const std::string relative { lfs_object_relative_path(oid) };
            return storage.empty() == false && relative.empty() == false && context.owner().files().probe(join_path(storage, relative)) == path_kind::file;
        }

        // 작업 트리 파일이 포인터가 아니라 실제 내용인가. 파일이 없으면 거짓이다.
        bool worktree_has_content(git_operation_context& context, const std::string& path)
        {
            const std::u8string absolute { join_path(context.job().repository_path, path) };
            workspace_files& files { context.owner().files() };
            if (files.probe(absolute) != path_kind::file)
                return false;
            const std::optional<std::string> text { files.read_text(absolute, pointer_probe_bytes) };
            // 읽지 못했으면(너무 크다) 포인터가 아니다.
            return text.has_value() == false || parse_lfs_pointer(*text).has_value() == false;
        }

        // hunk 하나의 줄을 옛 쪽과 새 쪽 텍스트로 가른다.
        void split_hunk_sides(const diff_hunk& hunk, std::string& old_text, std::string& new_text)
        {
            for (const diff_line& line : hunk.lines)
            {
                if (line.kind == diff_line_kind::no_newline)
                    continue;
                if (line.kind != diff_line_kind::add)
                    old_text.append(line.content).push_back('\n');
                if (line.kind != diff_line_kind::del)
                    new_text.append(line.content).push_back('\n');
            }
        }
    } // namespace

    bool repository_uses_lfs(git_operation_context& context)
    {
        const std::optional<std::string> attributes { context.owner().files().read_text(join_path(context.job().repository_path, ".gitattributes"), attributes_limit) };
        return attributes.has_value() && gitattributes_uses_lfs(*attributes);
    }

    lfs_repository_state read_lfs_state(git_operation_context& context)
    {
        lfs_repository_state state {};
        state.in_use = repository_uses_lfs(context);
        if (state.in_use == false)
            return state;

        git_command_spec version_spec {};
        version_spec.arguments = make_git_lfs_version_args();
        version_spec.command_class = git_command_class::tool_probe;
        version_spec.accepted_exit_codes = { 0, 1 };
        const git_command_outcome version { context.run(std::move(version_spec)) };
        if (version.accepted == false || version.exit_code() != 0)
            return state;
        const std::optional<std::string> text { parse_git_lfs_version(version.standard_output) };
        if (text.has_value() == false)
            return state;
        state.available = true;
        state.version = *text;
        state.installed = lfs_filter_installed(context);

        git_command_spec json_spec {};
        json_spec.arguments = make_git_lfs_ls_files_json_args();
        const git_command_outcome json { context.run(std::move(json_spec)) };
        if (json.accepted)
        {
            if (std::optional<std::vector<lfs_file>> files { parse_git_lfs_files_json(json.standard_output) }; files.has_value())
            {
                state.files = std::move(*files);
                return state;
            }
        }
        // 첫 커밋 전이거나 오래된 git-lfs는 json을 모른다. 긴 형식으로 한 번 더 시도한다.
        git_command_spec long_spec {};
        long_spec.arguments = make_git_lfs_ls_files_long_args();
        const git_command_outcome longer { context.run(std::move(long_spec)) };
        if (longer.accepted)
            state.files = parse_git_lfs_files_long(longer.standard_output);
        return state;
    }

    void mark_lfs_changes(git_operation_context& context, std::vector<file_change>& changes)
    {
        if (changes.empty())
            return;
        std::set<std::string> paths {};
        for (const file_change& change : changes)
            paths.insert(change.path);
        git_command_spec spec {};
        spec.arguments = make_git_check_attr_filter_args();
        for (const std::string& path : paths)
        {
            spec.standard_input.append(path);
            spec.standard_input.push_back('\0');
        }
        const git_command_outcome outcome { context.run(std::move(spec)) };
        if (outcome.accepted == false)
            return;
        const std::set<std::string> tracked { parse_git_check_attr_lfs_paths(outcome.standard_output) };
        for (file_change& change : changes)
            change.is_lfs = tracked.contains(change.path);
    }

    void annotate_lfs_diff(git_operation_context& context, const diff_file_request& request, std::vector<diff_file>& files)
    {
        std::optional<std::u8string> storage {};
        for (diff_file& file : files)
        {
            if (file.is_binary || file.hunks.size() != 1)
                continue;
            std::string old_text {};
            std::string new_text {};
            split_hunk_sides(file.hunks.front(), old_text, new_text);
            lfs_diff_info info {};
            info.old_pointer = parse_lfs_pointer(old_text);
            info.new_pointer = parse_lfs_pointer(new_text);
            if (info.old_pointer.has_value() == false && info.new_pointer.has_value() == false)
                continue;

            if (storage.has_value() == false)
                storage = lfs_storage_directory(context);
            // 포인터가 아닌 쪽(LFS로 옮기기 전의 일반 내용)과 없는 쪽(추가·삭제)은 로컬에 있는 것으로 본다.
            info.old_local = info.old_pointer.has_value() == false || object_exists(context, *storage, info.old_pointer->oid);
            if (info.new_pointer.has_value() == false)
                info.new_local = true;
            else if (request.view == diff_view::unstaged)
                info.new_local = worktree_has_content(context, file.new_path.empty() ? file.old_path : file.new_path);
            else
                info.new_local = object_exists(context, *storage, info.new_pointer->oid);
            file.lfs = std::move(info);
        }
    }

    operation_outcome run_lfs_pull(git_operation_context& context)
    {
        std::optional<operation_outcome> stop {};
        if (require_lfs(context, stop) == false)
            return *stop;

        // 설정이 없으면 `lfs pull`은 작업 트리를 채우지 않고 성공으로 끝난다. 이 저장소에만 설정을 더한다.
        if (lfs_filter_installed(context) == false)
        {
            git_command_spec install {};
            install.arguments = make_git_lfs_install_local_args();
            install.command_class = git_command_class::local_write;
            git_command_outcome installed {};
            if (context.run_required(std::move(install), installed, stop) == false)
                return *stop;
        }

        git_command_spec spec {};
        spec.arguments = make_git_lfs_pull_args();
        spec.command_class = git_command_class::network;
        spec.environment = lfs_progress_environment();
        spec.progress_on_standard_output = true;
        git_command_outcome outcome {};
        if (context.run_required(std::move(spec), outcome, stop) == false)
            return *stop;
        return operation_outcome::succeeded(u8"LFS 객체를 받아 작업 트리에 채웠습니다.");
    }

    operation_outcome run_lfs_fetch(git_operation_context& context, const lfs_fetch_request& request)
    {
        std::optional<operation_outcome> stop {};
        if (require_lfs(context, stop) == false)
            return *stop;
        git_command_spec spec {};
        spec.arguments = make_git_lfs_fetch_args(request.all);
        spec.command_class = git_command_class::network;
        spec.environment = lfs_progress_environment();
        spec.progress_on_standard_output = true;
        git_command_outcome outcome {};
        if (context.run_required(std::move(spec), outcome, stop) == false)
            return *stop;
        return operation_outcome::succeeded(u8"LFS 객체를 받았습니다.");
    }

    operation_outcome run_lfs_track(git_operation_context& context, const lfs_track_request& request)
    {
        std::optional<operation_outcome> stop {};
        if (require_lfs(context, stop) == false)
            return *stop;
        git_command_spec spec {};
        spec.arguments = make_git_lfs_track_args(request.pattern);
        spec.command_class = git_command_class::local_write;
        git_command_outcome outcome {};
        if (context.run_required(std::move(spec), outcome, stop) == false)
            return *stop;
        return operation_outcome::succeeded(to_utf8(request.pattern) + u8"을(를) LFS로 추적합니다. .gitattributes가 바뀌었습니다.");
    }
} // namespace cyella
