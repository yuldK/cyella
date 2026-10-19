#include "application/external_diff_opener.h"

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
