#pragma once

#include "application/process_runner.h"

#include <memory>

namespace cyella::win32 {
    // Win32 프로세스 실행 구현을 만든다. 반환한 instance는 여러 스레드에서 동시에
    // `run`을 호출할 수 있으며 실행별 상태를 공유하지 않는다.
    [[nodiscard]] std::unique_ptr<process_runner> make_process_runner();
} // namespace cyella::win32
