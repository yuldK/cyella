#pragma once

#include <string>
#include <string_view>

namespace cyella::win32 {
    // `<로밍 AppData>\cyella\settings.json`을 조립한다. 폴더를 만들지 않는 순수 문자열 처리다.
    [[nodiscard]] std::u8string app_settings_file_path_under(std::u8string_view roaming_app_data);

    // 앱 단위 설정 파일의 절대 경로다. `SHGetKnownFolderPath(FOLDERID_RoamingAppData)` 아래
    // `cyella` 폴더를 만들어 두고 돌려준다. 환경 변수가 아니라 known folder를 쓰는 이유는
    // `%APPDATA%`가 리디렉션되거나 비어 있어도 셸이 아는 위치를 따르기 위해서다. 위치를 알 수
    // 없거나 폴더를 만들 수 없으면 빈 값이다 (이때 앱은 저장 없이 메모리 설정으로 돈다).
    [[nodiscard]] std::u8string app_settings_file_path();
} // namespace cyella::win32
