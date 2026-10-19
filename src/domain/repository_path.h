#pragma once

#include <string>
#include <string_view>

namespace cyella {
    // 저장소 경로를 저장하는 모양으로 맞춘다. `/`를 `\`로 바꾸고 끝 구분자를 지운다.
    // 대소문자는 건드리지 않는다. 사용자가 고른 표기를 목록에 그대로 보여 주기 위해서다.
    [[nodiscard]] std::u8string normalize_repository_path(std::u8string_view path);

    // 같은 저장소인지 판정하는 열쇠다. `normalize_repository_path` 결과의 ASCII 대문자를
    // 소문자로 내린다. Windows 파일 이름이 대소문자를 구분하지 않기 때문이다. ASCII 밖의
    // 글자는 그대로 둔다. 한글처럼 대소문자가 없는 글자가 대부분이고, 잘못 접어서 다른
    // 저장소를 같다고 보는 쪽이 더 나쁘다.
    [[nodiscard]] std::u8string repository_path_key(std::u8string_view path);

    // 경로의 마지막 구성 요소다. 뿌리(`C:\`)처럼 구성 요소가 없으면 경로 전체를 쓴다.
    [[nodiscard]] std::u8string repository_display_name(std::u8string_view path);

    // 마지막 구성 요소를 뺀 폴더다. `C:\work\repo`는 `C:\work`, `C:\repo`는 `C:\`이고
    // 구분자가 없거나 뿌리이면 빈 값이다.
    [[nodiscard]] std::u8string repository_parent_folder(std::u8string_view path);
} // namespace cyella
