#pragma once

#include <string>
#include <string_view>

namespace cyella::win32 {
    // diff 웹뷰를 세우는 데 필요한 OS 사실들이다. 웹뷰 자체(생성·합성)는 luil이 하고, 여기는 "세울 수 있는가"와 "어디에
    // 두는가"만 답한다.

    // WebView2 Evergreen Runtime이 설치돼 있는가. 로더에 물어 설치된 판 번호를 얻는다 (프로세스를 띄우지 않는다).
    // luil은 환경을 만들지 못해도 앱에 사건을 보내지 않으므로(placeholder만 남는다) 앱이 미리 알아야 대체 보기로 물러선다.
    [[nodiscard]] bool webview_runtime_installed() noexcept;

    // 실행 파일 옆 `diff-viewer\index.html`의 절대 경로다 (빌드 폴더와 설치 폴더가 같은 배치다). 파일이 없으면 값은 있어도
    // `exists`가 거짓이다.
    struct viewer_location
    {
        std::u8string index_path {};
        bool exists { false };
    };
    [[nodiscard]] viewer_location locate_diff_viewer() noexcept;

    // 웹뷰의 프로필(쿠키·캐시) 폴더다. `%LOCALAPPDATA%\cyella\webview`. 폴더는 WebView2가 만든다. 위치를 모르면 비어 있다.
    [[nodiscard]] std::u8string webview_user_data_directory() noexcept;
    // `%LOCALAPPDATA%\cyella\<name>`이다 (지워도 되는 보관 자료: 웹뷰 프로필, 작성자 사진). 폴더는 쓰는 쪽이 만든다. 위치를 모르면 비어 있다.
    [[nodiscard]] std::u8string local_data_directory(std::wstring_view name) noexcept;

    // OS의 "앱 모드"가 밝은가 (`AppsUseLightTheme`). 읽지 못하면 밝은 쪽으로 본다 (luil의 기본과 같다).
    [[nodiscard]] bool system_prefers_light_theme() noexcept;

    // 고대비 모드가 켜져 있는가.
    [[nodiscard]] bool high_contrast_enabled() noexcept;
} // namespace cyella::win32
