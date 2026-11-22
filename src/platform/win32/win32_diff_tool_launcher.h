#pragma once

#include "application/diff_tool_launcher.h"
#include "application/file_probe.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella::win32 {
    // 외부 diff 도구를 CreateProcess로 띄우는 구현이다. 프로세스가 끝나기를 기다리지 않고 핸들을 바로 닫는다.
    // 실행 파일 이름에 경로가 없으면 앱 폴더, System32, Windows 폴더, PATH의 절대 항목에서 찾는다(사용자가 `cmd`나
    // `winmergeu`처럼 적을 수 있다). CreateProcessW에 이름만 넘기면 부모의 현재 폴더를 먼저 찾아, 앱을 띄운 저장소에
    // 심어 둔 같은 이름의 실행 파일이 돌기 때문이다.
    [[nodiscard]] std::unique_ptr<diff_tool_launcher> make_diff_tool_launcher();

    // 이름만 적힌 도구를 찾을 폴더를 순서대로 만든다. 앱 폴더, System32, Windows 폴더, PATH의 절대 항목 순서다.
    // 빈 값과 절대 경로가 아닌 폴더는 버린다. Win32 호출이 실패해 빈 폴더가 오거나 PATH에 `.`이 있으면 현재 폴더나
    // 현재 드라이브를 기준으로 찾게 되기 때문이다.
    [[nodiscard]] std::vector<std::u8string> tool_search_directories(
        std::u8string_view application_directory, std::u8string_view system_directory, std::u8string_view windows_directory, std::u8string_view path_environment);

    // 실행 파일 이름에 `\`, `/`, `:`가 하나라도 있으면 그대로 돌려준다(절대·상대 경로는 지금처럼 CreateProcessW가 푼다).
    // 이름뿐이면 `directories`에서 차례로 찾아 처음 있는 파일의 절대 경로를, 없으면 `std::nullopt`를 돌려준다.
    // 확장자가 없으면 CreateProcessW처럼 `.exe`를 붙인다. PATHEXT는 보지 않는다(CreateProcessW도 보지 않는다).
    [[nodiscard]] std::optional<std::u8string> resolve_tool_executable(std::u8string_view executable, const std::vector<std::u8string>& directories, const file_probe& probe);

    // 커밋 메시지 제안 명령의 실행 파일을 찾는다 (ADR-009). 먼저 `resolve_tool_executable`의 규칙으로 찾고, 못 찾은 이름이 `claude`면
    // 알려진 설치 자리를 더 본다: 네이티브 설치의 `<user_profile>\.local\bin\claude.exe`, 그리고 npm 설치가 PATH에 놓는 `claude.cmd`
    // 옆의 `node_modules\@anthropic-ai\claude-code\bin\claude.exe`다. npm으로 깔면 PATH에는 `.cmd` shim뿐이고, shim은 cmd.exe의
    // 인용 규칙을 거치므로 띄우지 않고 그것이 가리키는 실행 파일을 직접 쓴다.
    [[nodiscard]] std::optional<std::u8string> resolve_suggestion_executable(
        std::u8string_view executable, const std::vector<std::u8string>& directories, std::u8string_view user_profile, const file_probe& probe);
    // 지금 프로세스의 폴더·PATH·`USERPROFILE`로 찾는다. 파일을 조회하므로 worker에서 부른다.
    [[nodiscard]] std::optional<std::u8string> resolve_suggestion_executable(std::u8string_view executable);
} // namespace cyella::win32
