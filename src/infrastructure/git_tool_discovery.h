#pragma once

#include "application/file_probe.h"
#include "application/process_cancellation.h"
#include "application/process_runner.h"
#include "domain/git_tool.h"

#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 탐색에 필요한 호스트 정보다. 값으로 받아 두면 탐색 규칙 자체를 프로세스나
    // 환경 변수 없이 test할 수 있다. 실제 값은 Win32 adapter가 채운다.
    struct git_tool_environment
    {
        std::u8string path_environment {};
        // `ProgramFiles`와 `ProgramFiles(x86)`처럼 알려진 기본 설치 위치의 부모다.
        std::vector<std::u8string> program_files_directories {};
    };

    struct git_tool_candidate
    {
        std::u8string executable {};
        bool manually_configured { false };
    };

    inline constexpr std::u8string_view git_executable_name { u8"git.exe" };

    // `PATH`를 분해한다. 따옴표를 벗기고, 빈 항목과 절대 경로가 아닌 항목을 버리며,
    // ASCII 대소문자를 무시하고 중복을 제거한다. 상대 경로 항목을 버리는 이유는
    // 호출자의 현재 디렉터리에 따라 다른 실행 파일이 잡힐 수 있기 때문이다.
    [[nodiscard]] std::vector<std::u8string> split_search_path(std::u8string_view path_environment);

    // 지정 경로가 있으면 그 하나만, 없으면 `PATH`와 기본 설치 위치 순서의 후보를
    // 만든다. filesystem을 조회하지 않는 순수 함수다.
    [[nodiscard]] std::vector<git_tool_candidate> git_tool_candidates(std::u8string_view configured_executable, const git_tool_environment& environment);

    [[nodiscard]] std::u8string executable_directory(std::u8string_view executable);

    // 후보를 확인하고 `--version`을 실행해 도구 정보를 만든다. 예외를 던지지 않는다.
    // 지원하는 최소 버전은 2.43이다.
    [[nodiscard]] git_tool_info resolve_git_tool(
        std::u8string_view configured_executable, const git_tool_environment& environment, process_runner& runner, const file_probe& probe, const process_cancellation_token& token) noexcept;
} // namespace cyella
