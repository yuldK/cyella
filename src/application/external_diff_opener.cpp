#include "application/external_diff_opener.h"

#include <filesystem>
#include <system_error>

namespace cyella {
    namespace {
        std::u8string to_u8(const std::string& text)
        {
            return std::u8string { text.begin(), text.end() };
        }

        // `git difftool`이 보기에 맞는 비교를 하게 하는 인자다. git은 index와 작업 트리, HEAD와 index, 두 커밋을
        // 이 인자로 가른다.
        std::vector<std::u8string> difftool_arguments(const diff_side_plan& plan)
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
                arguments.push_back(to_u8(plan.sha + "^"));
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
                return { external_open_status::invalid_template, app.name, "인자 템플릿의 따옴표가 닫히지 않았습니다." };
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
                    return { external_open_status::extraction_failed, app.name, "작업 경로를 찾을 수 없습니다. 저장소 상태를 새로 읽어 주세요." };
                values.file = file.u8string();
            }
            else if (needs_file)
            {
                const extracted_sides sides { gateway_.extract_cancellable(plan, true, request.cancellation) };
                if (sides.ok == false)
                    return { external_open_status::extraction_failed, app.name, sides.error };
                const extracted_side& side { sides.right.present ? sides.right : sides.left };
                if (side.present == false || side.file.empty())
                    return { external_open_status::extraction_failed, app.name, "열 수 있는 파일 버전이 없습니다." };
                values.file = side.file;
                // 이력·index의 사본을 편집해도 저장소에는 반영되지 않는다. 원본과 혼동하지 않게 읽기 전용으로 둔다.
                std::error_code error {};
                std::filesystem::permissions(std::filesystem::path { values.file }, std::filesystem::perms::owner_read, std::filesystem::perm_options::replace, error);
            }
            if (request.cancellation.cancelled())
                return { external_open_status::launch_failed, app.name, "취소되었습니다." };
            auto arguments { expand_external_diff_arguments(app.arguments, values) };
            if (arguments.has_value() == false)
                return { external_open_status::invalid_template, app.name, "인자 템플릿의 따옴표가 닫히지 않았습니다." };
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
                return { external_open_status::invalid_template, tool->executable, "인자 템플릿의 따옴표가 닫히지 않았습니다." };

            const tool_launch_result launched { launcher_.launch({ tool->executable, std::move(*arguments), plan.repository_path }) };
            if (launched.launched == false)
                return { external_open_status::launch_failed, tool->executable, launched.error };
            return { external_open_status::launched_tool, tool->executable, {} };
        }

        // 추적하지 않는 파일은 git difftool이 비교하지 못한다.
        if (plan.untracked == false && gateway_.configured_diff_tool(plan.repository_path).has_value() && request.git_executable.empty() == false)
        {
            const tool_launch_result launched { launcher_.launch({ request.git_executable, difftool_arguments(plan), plan.repository_path }) };
            if (launched.launched == false)
                return { external_open_status::launch_failed, request.git_executable, launched.error };
            return { external_open_status::launched_difftool, request.git_executable, {} };
        }

        return { external_open_status::no_tool, {}, "이 확장자에 지정된 외부 도구가 없고 git의 diff.tool도 설정돼 있지 않습니다." };
    }
} // namespace cyella
