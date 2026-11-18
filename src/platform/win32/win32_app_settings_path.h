#pragma once

#include <string>
#include <string_view>

namespace cyella::win32 {
    // `<로밍 AppData>\cyella\settings.json`을 조립한다. 폴더를 만들지 않는 순수 문자열 처리다.
    [[nodiscard]] std::u8string app_settings_file_path_under(std::u8string_view roaming_app_data);

    // `SHGetKnownFolderPath(FOLDERID_RoamingAppData)`의 UTF-8 경로다. 폴더를 만들지 않는 조회라 시험이 실제 프로필에
    // 흔적을 남기지 않고 같은 위치를 확인할 수 있다. 알 수 없으면 빈 값이다.
    [[nodiscard]] std::u8string roaming_app_data_folder();

    // 앱 단위 설정 파일의 절대 경로다. `SHGetKnownFolderPath(FOLDERID_RoamingAppData)` 아래
    // `cyella` 폴더를 만들어 두고 돌려준다. 환경 변수가 아니라 known folder를 쓰는 이유는
    // `%APPDATA%`가 리디렉션되거나 비어 있어도 셸이 아는 위치를 따르기 위해서다. 위치를 알 수
    // 없거나 폴더를 만들 수 없으면 빈 값이다 (이때 앱은 저장 없이 메모리 설정으로 돈다).
    [[nodiscard]] std::u8string app_settings_file_path();

    // `app_settings_file_path`가 known folder 대신 받은 폴더 아래에서 하는 일이다. 경로를 조립하고 `cyella` 폴더를 만들어 둔다.
    // 시험이 실제 프로필 대신 임시 폴더로 폴더 만들기를 확인할 수 있게 따로 둔다. 만들 수 없으면 빈 값이다.
    [[nodiscard]] std::u8string prepared_app_settings_file_path_under(std::u8string_view roaming_app_data);
} // namespace cyella::win32
