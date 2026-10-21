#pragma once

#include "domain/text_catalog.h"

#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    struct text_catalog_parse_result
    {
        text_catalog catalog {};
        // 읽지 못한 항목이다 (깨진 JSON, 문자열이 아닌 값, 같은 키 두 번). 화면 문구라 사용자에게 보이지 않고 키 검사
        // test가 읽는다. 문제가 있어도 읽을 수 있는 항목은 남긴다.
        std::vector<std::u8string> problems {};
    };

    // 언어 하나의 문구 파일(`assets/locales/<언어>.json`)을 읽는다. 최상위가 키 → 문구 문자열인 평평한 object다.
    [[nodiscard]] text_catalog_parse_result parse_text_catalog_json(std::string_view source, ui_language language);
} // namespace cyella
