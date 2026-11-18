#pragma once

#include <string>
#include <string_view>

namespace cyella {
    // 여러 줄 본문 칸(커밋 본문, rebase 메시지 본문)에 넣는 글이다. 붙여넣은 CRLF·CR은 LF로 바꾼다. 칸은 LF로만 줄을 나눠
    // CR이 줄 끝에 남으면 caret이 그 뒤에 서고 커밋 메시지에도 실린다. 탭은 그대로 둔다. 두 칸이 같은 규칙이어야 해서 한곳에 둔다.
    [[nodiscard]] std::u8string multiline_text(std::u8string_view text);
} // namespace cyella
