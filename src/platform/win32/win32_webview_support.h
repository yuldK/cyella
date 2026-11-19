#pragma once

#include <string>
#include <string_view>

namespace cyella::win32 {
    // diff 웹뷰를 세우는 데 필요한 OS 사실들이다. 웹뷰 자체(생성·합성)는 luil이 하고, 여기는 "세울 수 있는가"와 "어디에
    // 두는가"만 답한다.

    // WebView2 Evergreen Runtime이 설치돼 있는가. 로더에 물어 설치된 판 번호를 얻는다 (프로세스를 띄우지 않는다).
    // 웹뷰를 세우지 못하면 luil이 `creation_failed`로 알리므로 이것 없이도 대체 보기로 물러서지만, Runtime이 없는 PC에서
    // 처음 diff를 열 때 placeholder가 잠깐 비치지 않도록 시작할 때 먼저 거르는 빠른 길이다. 판이 오래되었는지는 보지 않는다(사건이 맡는다).
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
} // namespace cyella::win32
