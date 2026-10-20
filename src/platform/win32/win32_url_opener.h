#pragma once

#include <string_view>

namespace cyella::win32 {
    // 주소를 사용자의 기본 브라우저로 연다 (`ShellExecuteW`의 `open`). http·https 주소만 연다 — 서버가 준 주소이므로 `file:`·실행 파일 경로·스크립트 주소를
    // 셸에 넘기지 않는다. 호출한 스레드에서 COM을 잠시 초기화하므로 UI·logic thread가 아닌 워커에서 부른다. 열었으면 true다.
    [[nodiscard]] bool open_url_in_browser(std::u8string_view url) noexcept;

    // 열 수 있는 주소인가 (http·https이고 제어 문자·공백이 없다).
    [[nodiscard]] bool is_openable_web_url(std::u8string_view url) noexcept;
} // namespace cyella::win32
