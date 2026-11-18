#include "application/external_diff_opener.h"

#include "domain/text_catalog.h"

#include <filesystem>
#include <optional>
#include <system_error>

namespace cyella {
    namespace {
        std::u8string to_u8(const std::string& text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        bool is_full_object_id(const std::string& sha)
        {
            if (sha.size() != 40 && sha.size() != 64)
                return false;
            for (const char letter : sha)
            {
                if ((letter >= '0' && letter <= '9') == false && (letter >= 'a' && letter <= 'f') == false && (letter >= 'A' && letter <= 'F') == false)
                    return false;
            }
            return true;
        }

        // 커밋에서 추가된 파일의 왼쪽이다. 루트 커밋은 `sha^`를 풀 수 없어 difftool이 창 없이 실패하므로, 부모가
        // 있든 없든 같은 결과인 빈 트리와 비교한다. 빈 트리 해시는 저장소의 객체 형식에 따르므로 저장소에 물은 값을
        // 먼저 쓴다. 묻지 못했으면 전체 해시의 길이로 형식을 짐작하고, 줄인 해시면 예전처럼 `sha^`를 둔다.
        std::string added_file_base(const std::string& sha, const std::optional<std::string>& empty_tree)
        {
            if (empty_tree.has_value())
                return *empty_tree;
            if (is_full_object_id(sha) == false)
                return sha + "^";
            if (sha.size() == 64)
                return "6ef19b41225c5369f1c104d45d8d85efa9b057b53b14b4b9b939dd74decc5321";
            return "4b825dc642cb6eb9a060e54bf8d69288fbee4904";
        }

        // 커밋에서 추가된 파일인가다. 추가 판단은 resolve_diff_sides와 같다.
        bool added_in_commit(const diff_side_plan& plan) noexcept
        {
            return plan.view == diff_view::commit && (plan.status == 'A' || plan.old_path.empty());
        }

        // `git difftool`이 보기에 맞는 비교를 하게 하는 인자다. git은 index와 작업 트리, HEAD와 index, 두 커밋을
        // 이 인자로 가른다.
        std::vector<std::u8string> difftool_arguments(const diff_side_plan& plan, const std::optional<std::string>& empty_tree)
        {
            std::vector<std::u8string> arguments { u8"--literal-pathspecs", u8"difftool", u8"--no-prompt" };
            switch (plan.view)
            {
            case diff_view::unstaged:
                break;
            case diff_view::staged:
                arguments.emplace_back(u8"--cached");
                break;
            case diff_view::commit:
                // 추가 판단은 resolve_diff_sides와 같다.
                arguments.push_back(to_u8(added_in_commit(plan) ? added_file_base(plan.sha, empty_tree) : plan.sha + "^"));
                arguments.push_back(to_u8(plan.sha));
                break;
            case diff_view::range:
                arguments.push_back(to_u8(plan.base));
                arguments.push_back(to_u8(plan.sha));
                break;
            }
            arguments.emplace_back(u8"--");
            arguments.push_back(to_u8(plan.display_path()));
            return arguments;
        }
    } // namespace

    external_open_result external_diff_opener::open(const external_open_request& request)
    {
        const diff_side_plan& plan { request.plan };
        const std::u8string path { to_u8(plan.display_path()) };

        if (request.file_app.has_value() && request.folder.empty() == false)
        {
            const external_file_app& app { *request.file_app };
            external_diff_values values {};
            values.file = request.folder;
            values.repository = request.folder;
            auto arguments { expand_external_diff_arguments(app.arguments, values) };
            if (arguments.has_value() == false)
                return { external_open_status::invalid_template, app.name, tr_narrow(u8"external_diff.template_unclosed_quote") };
            const tool_launch_result launched { launcher_.launch({ app.executable, std::move(*arguments), request.folder }) };
            if (launched.launched == false)
                return { external_open_status::launch_failed, app.name, launched.error };
            return { external_open_status::launched_tool, app.name, {} };
        }

        if (request.file_app.has_value())
        {
            const external_file_app& app { *request.file_app };
            external_diff_values values {};
            values.path = path;
            values.repository = plan.repository_path;
            const diff_side_pair specs { resolve_diff_sides(plan) };
            const bool needs_file { app.arguments.find(u8"{file}") != std::u8string::npos };
            if (needs_file && specs.right.kind == diff_side_kind::worktree)
            {
                const std::filesystem::path file { std::filesystem::path { plan.repository_path } / std::filesystem::path { to_u8(specs.right.path) } };
                std::error_code error {};
                if (std::filesystem::exists(file, error) == false)
                    return { external_open_status::extraction_failed, app.name, tr_narrow(u8"external_diff.worktree_path_missing") };
                values.file = file.u8string();
            }
            else if (needs_file)
            {
                const extracted_sides sides { gateway_.extract_cancellable(plan, true, request.cancellation) };
                if (sides.ok == false)
                    return { external_open_status::extraction_failed, app.name, sides.error };
                const extracted_side& side { sides.right.present ? sides.right : sides.left };
                if (side.present == false || side.file.empty())
                    return { external_open_status::extraction_failed, app.name, tr_narrow(u8"external_diff.no_file_version") };
                values.file = side.file;
                // 이력·index의 사본을 편집해도 저장소에는 반영되지 않는다. 원본과 혼동하지 않게 읽기 전용으로 둔다.
                std::error_code error {};
                std::filesystem::permissions(std::filesystem::path { values.file }, std::filesystem::perms::owner_read, std::filesystem::perm_options::replace, error);
            }
            if (request.cancellation.cancelled())
                return { external_open_status::launch_failed, app.name, tr_narrow(u8"diff_action.cancelled") };
            auto arguments { expand_external_diff_arguments(app.arguments, values) };
            if (arguments.has_value() == false)
                return { external_open_status::invalid_template, app.name, tr_narrow(u8"external_diff.template_unclosed_quote") };
            const tool_launch_result launched { launcher_.launch({ app.executable, std::move(*arguments), plan.repository_path }) };
            if (launched.launched == false)
                return { external_open_status::launch_failed, app.name, launched.error };
            return { external_open_status::launched_tool, app.name, {} };
        }

        if (const external_diff_tool* const tool { select_external_diff_tool(request.tools, path) }; tool != nullptr)
        {
            const extracted_sides sides { gateway_.extract_cancellable(plan, true, request.cancellation) };
            if (sides.ok == false)
                return { external_open_status::extraction_failed, tool->executable, sides.error };

            external_diff_values values {};
            values.left = sides.left.file;
            values.right = sides.right.file;
            values.path = path;
            std::optional<std::vector<std::u8string>> arguments { expand_external_diff_arguments(tool->arguments, values) };
            if (arguments.has_value() == false)
                return { external_open_status::invalid_template, tool->executable, tr_narrow(u8"external_diff.template_unclosed_quote") };

            const tool_launch_result launched { launcher_.launch({ tool->executable, std::move(*arguments), plan.repository_path }) };
            if (launched.launched == false)
                return { external_open_status::launch_failed, tool->executable, launched.error };
            return { external_open_status::launched_tool, tool->executable, {} };
        }

        // 추적하지 않는 파일은 git difftool이 비교하지 못한다.
        if (plan.untracked == false && gateway_.configured_diff_tool(plan.repository_path).has_value() && request.git_executable.empty() == false)
        {
            // 빈 트리는 저장소에 물어야 줄인 해시로 온 루트 커밋도 맞게 연다. 추가된 파일일 때만 git을 한 번 더 부른다.
            const std::optional<std::string> empty_tree { added_in_commit(plan) ? gateway_.empty_tree(plan.repository_path) : std::optional<std::string> {} };
            const tool_launch_result launched { launcher_.launch({ request.git_executable, difftool_arguments(plan, empty_tree), plan.repository_path }) };
            if (launched.launched == false)
                return { external_open_status::launch_failed, request.git_executable, launched.error };
            return { external_open_status::launched_difftool, request.git_executable, {} };
        }

        return { external_open_status::no_tool, {}, tr_narrow(u8"external_diff.no_tool_configured") };
    }
} // namespace cyella
