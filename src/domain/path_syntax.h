#pragma once

#include <cstddef>
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

    // ---- 이 OS의 경로 규칙
    // 앱이 돌고 있는 OS의 규칙이다. Windows에서는 위의 Windows 규칙과 같고, macOS(POSIX)에서는 `/`가 유일한 구분자이며 `/`로
    // 시작하는 경로가 절대 경로다 (`\`는 파일 이름에 쓸 수 있는 글자다). 경로를 받고 짓는 계층은 이 이름들만 쓰고 OS를 직접
    // 묻지 않는다.

#if defined(_WIN32)
    // 경로를 지을 때 넣는 구분자다.
    inline constexpr char8_t native_path_separator { u8'\\' };
    // `PATH` 환경 변수의 항목 구분자다.
    inline constexpr char8_t path_list_separator { u8';' };
    // 실행 파일 이름의 기본 확장자다. 이름만 적힌 도구를 찾을 때 붙인다.
    inline constexpr std::u8string_view executable_suffix { u8".exe" };
#else
    inline constexpr char8_t native_path_separator { u8'/' };
    inline constexpr char8_t path_list_separator { u8':' };
    inline constexpr std::u8string_view executable_suffix {};
#endif

    // 경로 구분자인가. Windows는 `\`와 `/` 둘 다, POSIX는 `/`만이다.
    [[nodiscard]] constexpr bool is_native_path_separator(const char8_t value) noexcept
    {
#if defined(_WIN32)
        return value == u8'\\' || value == u8'/';
#else
        return value == u8'/';
#endif
    }

    // 구분자 목록 문자열이다 (`find_last_of`에 넘긴다).
#if defined(_WIN32)
    inline constexpr std::u8string_view native_path_separators { u8"\\/" };
#else
    inline constexpr std::u8string_view native_path_separators { u8"/" };
#endif

    // 이 OS의 절대 경로인가. Windows는 `is_absolute_windows_path`이고 POSIX는 `/`로 시작하는 경로다. filesystem을 조회하지 않는다.
    [[nodiscard]] bool is_absolute_path(std::u8string_view path) noexcept;

    // 뿌리의 길이다. 끝 구분자를 지울 때 이보다 짧게 줄이지 않는다. Windows는 `C:\`의 3, POSIX는 `/`의 1이다.
#if defined(_WIN32)
    inline constexpr std::size_t native_root_length { 3 };
#else
    inline constexpr std::size_t native_root_length { 1 };
#endif

    // 폴더와 이름을 이 OS의 구분자로 잇는다. 폴더가 이미 구분자로 끝나면 하나만 둔다. 폴더가 비면 이름만이다.
    [[nodiscard]] std::u8string join_native_path(std::u8string_view directory, std::u8string_view name);

    // 마지막 구성 요소를 뗀 폴더다. 뿌리 바로 아래(`C:\repo`, `/repo`)는 뿌리(`C:\`, `/`)이고, 구분자가 없으면 빈 값이다.
    [[nodiscard]] std::u8string native_parent_path(std::u8string_view path);

    // 같은 저장소인지 비교하는 열쇠다. 구분자를 `/`로, ASCII 대문자를 소문자로 바꾸고 끝의
    // 구분자를 뗀다. Windows 경로는 대소문자를 구분하지 않으므로 탭 중복 판정과 실행 중 git
    // 수 세기가 같은 규칙을 써야 한다. 심볼릭 링크는 풀지 않는다. POSIX에서는 `\`를 구분자로 보지 않는다.
    [[nodiscard]] std::u8string normalize_path_key(std::u8string_view path);

    // 저장소 루트(`root`, 절대 경로)와 git이 준 상대 경로(`/` 구분, UTF-8)를 합친 절대 경로다. 구분자는 이 OS의 것이다
    // (Windows `\`, POSIX `/`).
    // 상대 경로가 비었거나 절대 경로·`..`·`.` 구성 요소·NUL·빈 구성 요소를 가졌거나 `.git` 안을 가리키면 값이 없다.
    // 끝이 점이나 공백인 구성 요소(`.git.`, `...`)와 `.git`의 8.3 이름(`GIT~1`)도 NTFS가 다른 이름으로 풀므로 거부한다.
    // 휴지통처럼 되돌리기 어려운 동작이 저장소 밖을 건드리지 못하게 하는 어휘 검사이며 filesystem을 조회하지 않는다.
    [[nodiscard]] std::optional<std::u8string> resolve_workspace_child(std::u8string_view root, std::string_view relative_path);

    // 저장소 경로를 `.gitignore`에서 그 파일 하나만 맞는 글로 바꾼다(앞의 `/`는 호출자가 붙인다). `*`·`?`·`[`·`\`는 패턴 글자라
    // `\`를 앞에 붙이고, 끝의 공백은 git이 지우므로 하나씩 `\`로 지킨다. 그러지 않으면 `a[1].txt`가 `a1.txt`와 맞아 그 파일은 계속
    // 변경 목록에 보인다. 맨 앞의 `#`·`!`는 앞에 `/`가 붙어 특별하지 않다.
    [[nodiscard]] std::string gitignore_literal(std::string_view path);
} // namespace cyella
