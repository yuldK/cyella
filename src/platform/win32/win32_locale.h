#pragma once

#include "domain/text_catalog.h"

#include <string_view>

namespace cyella::win32 {
    // Windows UI 언어다. 한국어가 아니면 영어로 본다.
    [[nodiscard]] ui_language system_ui_language() noexcept;

    // 설정 파일에서 언어 선택만 읽는다. 파일이 없거나 읽지 못하면 `system`이다. 카탈로그를 정하기 전에 부르므로
    // 깨진 파일을 옮기는 등의 일은 하지 않는다 (그것은 앱이 설정을 읽을 때 한다).
    [[nodiscard]] language_preference read_language_preference(std::u8string_view settings_path);

    // 실행 파일에 실린 언어 하나의 문구 파일(JSON) 바이트다. 없으면 비어 있다.
    [[nodiscard]] std::string_view locale_resource(ui_language language) noexcept;

    // 실행 파일(또는 test 실행 파일)에 실린 문구 파일로 전역 카탈로그를 정한다. 한국어 원문을 늘 바탕으로 깔고,
    // 영어면 그 위에 얹는다. 작업 thread가 생기기 전에 부른다.
    void install_ui_language(ui_language language);
} // namespace cyella::win32
