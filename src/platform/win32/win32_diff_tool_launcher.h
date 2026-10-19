#pragma once

#include "application/diff_tool_launcher.h"

#include <memory>

namespace cyella::win32 {
    // 외부 diff 도구를 CreateProcess로 띄우는 구현이다. 프로세스가 끝나기를 기다리지 않고 핸들을 바로 닫는다.
    // 실행 파일 이름에 경로가 없으면 PATH에서 찾는다(사용자가 `code`나 `winmergeu`처럼 적을 수 있다).
    [[nodiscard]] std::unique_ptr<diff_tool_launcher> make_diff_tool_launcher();
} // namespace cyella::win32
