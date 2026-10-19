#pragma once

#include "application/workspace_files.h"

#include <memory>

namespace cyella::win32 {
    // 실제 디스크의 `workspace_files`를 만든다. 상태가 없어 여러 스레드가 함께 써도 된다.
    [[nodiscard]] std::unique_ptr<workspace_files> make_workspace_files();
} // namespace cyella::win32
