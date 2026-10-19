#pragma once

#include <string>
#include <string_view>

namespace cyella {
    // Win32 API 없이 어휘적으로만 절대 경로를 판정한다. `C:\`와 `C:/`, `\\server\share`,
    // `\\?\C:\`를 절대 경로로 보고 `C:relative`, `\rooted`, 빈 값은 거부한다.
    //
    // 프로세스 요청 검증과 git 실행 파일 경로 검증이 같은 규칙을 써야 하므로 도메인에
    // 둔다. filesystem을 조회하지 않으므로 존재 여부는 알 수 없다.
    [[nodiscard]] bool is_absolute_windows_path(std::u8string_view path) noexcept;

    // 같은 저장소인지 비교하는 열쇠다. 구분자를 `/`로, ASCII 대문자를 소문자로 바꾸고 끝의
    // 구분자를 뗀다. Windows 경로는 대소문자를 구분하지 않으므로 탭 중복 판정과 실행 중 git
    // 수 세기가 같은 규칙을 써야 한다. 심볼릭 링크는 풀지 않는다.
    [[nodiscard]] std::u8string normalize_path_key(std::u8string_view path);
} // namespace cyella
