#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cyella {
    // Win32 API 없이 어휘적으로만 절대 경로를 판정한다. `C:\`와 `C:/`, `\\server\share`,
    // `\\?\C:\`, `\\?\UNC\server\share`를 절대 경로로 보고 `C:relative`, `\rooted`, 빈 값은 거부한다.
    // 공유 이름이 없는 `\\server`, 장치 이름 공간 `\\.\`, 그 밖의 `\\?\` 이름 공간(`GLOBALROOT` 등)도 거부한다.
    //
    // 프로세스 요청 검증과 git 실행 파일 경로 검증이 같은 규칙을 써야 하므로 도메인에
    // 둔다. filesystem을 조회하지 않으므로 존재 여부는 알 수 없다.
    [[nodiscard]] bool is_absolute_windows_path(std::u8string_view path) noexcept;

    // 같은 저장소인지 비교하는 열쇠다. 구분자를 `/`로, ASCII 대문자를 소문자로 바꾸고 끝의
    // 구분자를 뗀다. Windows 경로는 대소문자를 구분하지 않으므로 탭 중복 판정과 실행 중 git
    // 수 세기가 같은 규칙을 써야 한다. 심볼릭 링크는 풀지 않는다.
    [[nodiscard]] std::u8string normalize_path_key(std::u8string_view path);

    // 저장소 루트(`root`, 절대 경로)와 git이 준 상대 경로(`/` 구분, UTF-8)를 합친 절대 경로다. 구분자는 `\`로 바뀐다.
    // 상대 경로가 비었거나 절대 경로·`..`·`.` 구성 요소·NUL·빈 구성 요소를 가졌거나 `.git` 안을 가리키면 값이 없다.
    // 끝이 점이나 공백인 구성 요소(`.git.`, `...`)와 `.git`의 8.3 이름(`GIT~1`)도 NTFS가 다른 이름으로 풀므로 거부한다.
    // 휴지통처럼 되돌리기 어려운 동작이 저장소 밖을 건드리지 못하게 하는 어휘 검사이며 filesystem을 조회하지 않는다.
    [[nodiscard]] std::optional<std::u8string> resolve_workspace_child(std::u8string_view root, std::string_view relative_path);

    // 저장소 경로를 `.gitignore`에서 그 파일 하나만 맞는 글로 바꾼다(앞의 `/`는 호출자가 붙인다). `*`·`?`·`[`·`\`는 패턴 글자라
    // `\`를 앞에 붙이고, 끝의 공백은 git이 지우므로 하나씩 `\`로 지킨다. 그러지 않으면 `a[1].txt`가 `a1.txt`와 맞아 그 파일은 계속
    // 변경 목록에 보인다. 맨 앞의 `#`·`!`는 앞에 `/`가 붙어 특별하지 않다.
    [[nodiscard]] std::string gitignore_literal(std::string_view path);
} // namespace cyella
