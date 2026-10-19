#pragma once

#include "application/diff_sides.h"
#include "application/diff_tool_launcher.h"
#include "domain/external_diff_tool.h"

#include <string>
#include <vector>

namespace cyella {
    struct external_open_request
    {
        diff_side_plan plan {};
        // 대체 경로(`git difftool`)를 띄울 때 쓰는 git이다.
        std::u8string git_executable {};
        // 환경 설정의 확장자별 도구 표다.
        std::vector<external_diff_tool> tools {};
    };

    enum class external_open_status
    {
        // 설정 표의 도구를 띄웠다.
        launched_tool,
        // 표에 없어서 `diff.tool` 설정을 따르는 `git difftool`을 띄웠다.
        launched_difftool,
        // 표에도 `diff.tool`에도 도구가 없다. 환경 설정에서 지정하라고 안내한다.
        no_tool,
        // 두 쪽 파일을 꺼내지 못했다.
        extraction_failed,
        // 인자 템플릿이 잘못됐다(닫히지 않은 따옴표).
        invalid_template,
        launch_failed,
    };

    struct external_open_result
    {
        external_open_status status { external_open_status::no_tool };
        // 띄운 실행 파일이다.
        std::u8string tool {};
        std::string detail {};

        [[nodiscard]] bool launched() const noexcept
        {
            return status == external_open_status::launched_tool || status == external_open_status::launched_difftool;
        }
    };

    // "외부 도구로 열기"다 (ADR-002).
    //  1. 확장자 표에서 도구를 고르면 두 쪽을 임시 파일로 꺼내 인자 템플릿을 풀어 띄운다.
    //  2. 표에 없고 git에 `diff.tool`이 설정돼 있으면 `git difftool --no-prompt`에 넘긴다.
    //  3. 둘 다 없으면 도구가 없다고 알린다.
    // 프로세스를 띄우고 파일을 쓰므로 worker thread에서 부른다.
    class external_diff_opener
    {
    public:
        external_diff_opener(diff_git_gateway& gateway, diff_tool_launcher& launcher) noexcept
            : gateway_ { gateway }
            , launcher_ { launcher }
        {}

        [[nodiscard]] external_open_result open(const external_open_request& request);

    private:
        diff_git_gateway& gateway_;
        diff_tool_launcher& launcher_;
    };
} // namespace cyella
