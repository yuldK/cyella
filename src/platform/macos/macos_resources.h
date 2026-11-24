#pragma once

#include <string_view>

namespace cyella::macos {
    // 실행 파일에 바이트 배열로 실린 자원이다 (Windows의 RCDATA 자리). src/CMakeLists.txt의 embedded 목록이 만든다.
    enum class embedded_resource
    {
        // 화면 문구 파일이다 (assets/locales/*.json, ADR-008).
        locale_korean,
        locale_english,
        // macOS의 낱말(Finder, 실행 파일 이름, 설치 위치)로 바꾸는 덮어쓰기 문구다 (assets/locales/macos/*.json).
        locale_korean_macos,
        locale_english_macos,
        // 앱 심볼과 타이틀 캘리그라피 SVG다 (assets/cyella*.svg).
        app_symbol,
        app_title,
        app_title_sea,
    };

    // 실린 바이트다. 프로세스가 끝날 때까지 살아 있다.
    [[nodiscard]] std::string_view embedded_bytes(embedded_resource resource) noexcept;
} // namespace cyella::macos
