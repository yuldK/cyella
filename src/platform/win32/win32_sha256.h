#pragma once

#include <string>
#include <string_view>

namespace cyella::win32 {
    // 바이트열의 SHA-256을 소문자 16진수 64자로 돌려준다 (Windows CNG). 실패하면 빈 값이다.
    [[nodiscard]] std::string sha256_hex(std::string_view bytes);
} // namespace cyella::win32
