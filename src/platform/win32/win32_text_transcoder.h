#pragma once

#include "application/text_transcoder.h"

#include <memory>

namespace cyella::win32 {
    // 프로세스 활성 code page(`CP_ACP`)를 UTF-8로 바꾸는 transcoder를 만든다.
    // 상태를 갖지 않으므로 여러 스레드에서 같은 instance를 사용할 수 있다.
    [[nodiscard]] std::unique_ptr<text_transcoder> make_active_code_page_transcoder();
} // namespace cyella::win32
