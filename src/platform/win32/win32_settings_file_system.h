#pragma once

#include "application/settings_file_system.h"

#include <memory>

namespace cyella::win32 {
    // 임시 파일 + `MoveFileExW`로 원자적으로 쓰는 설정 파일 시스템을 만든다. 상태가 없으므로
    // 여러 스레드에서 같은 instance를 써도 안전하다.
    [[nodiscard]] std::unique_ptr<settings_file_system> make_settings_file_system();
} // namespace cyella::win32
