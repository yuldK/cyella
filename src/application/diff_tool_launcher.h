#pragma once

#include <string>
#include <vector>

namespace cyella {
    struct tool_launch_request
    {
        // 실행 파일이다. 절대 경로이거나 PATH에서 찾을 이름(`code`, `winmergeu`)이다.
        std::u8string executable {};
        std::vector<std::u8string> arguments {};
        std::u8string working_directory {};
    };

    struct tool_launch_result
    {
        bool launched { false };
        // 실패한 까닭이다. 로그와 알림에 쓴다.
        std::string error {};
    };

    // 외부 diff 도구를 띄우는 계약이다. 프로세스가 끝나기를 기다리지 않는다(분리 실행). 테스트는 가짜로
    // 바꿔 실행하지 않고 요청만 확인한다.
    class diff_tool_launcher
    {
    public:
        diff_tool_launcher() = default;
        diff_tool_launcher(const diff_tool_launcher&) = delete;
        diff_tool_launcher(diff_tool_launcher&&) = delete;
        diff_tool_launcher& operator=(const diff_tool_launcher&) = delete;
        diff_tool_launcher& operator=(diff_tool_launcher&&) = delete;
        virtual ~diff_tool_launcher() = default;

        [[nodiscard]] virtual tool_launch_result launch(const tool_launch_request& request) = 0;
    };
} // namespace cyella
