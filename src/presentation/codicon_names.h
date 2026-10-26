#pragma once

#include <string_view>

namespace cyella {
    // Codicons 이름(`github`, `link-external`, 대소문자 무시)의 글리프다. 모르는 이름이면 0이다. 설정에 적은 아이콘 이름을 그릴 때 쓴다.
    [[nodiscard]] char32_t find_codicon(std::string_view name) noexcept;
} // namespace cyella
