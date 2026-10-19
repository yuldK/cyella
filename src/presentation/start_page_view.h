#pragma once

#include "domain/app_settings.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // 시작 페이지의 저장소 한 행이다.
    struct start_page_item
    {
        std::u8string display_name {};
        // 저장소가 있는 폴더다. 이름 뒤에 흐리게 붙는다.
        std::u8string parent_folder {};
        // 클릭했을 때 여는 저장소의 절대 경로다.
        std::u8string path {};
        std::u8string opened_at {};
        bool pinned { false };
        // 경로가 지금 디스크에 있는가. 거짓이면 화면이 "사용할 수 없음"으로 흐리게 그리고 열기를
        // 막는다. 항목을 지우는 것은 사용자의 몫이다 (이동식 드라이브가 잠시 빠졌을 수 있다).
        bool exists { true };

        [[nodiscard]] bool operator==(const start_page_item&) const noexcept = default;
    };

    struct start_page_view
    {
        // 고정한 항목이 먼저, 그다음 최근에 연 순서다.
        std::vector<start_page_item> items {};
        // 검색어를 걸기 전의 전체 항목 수다. 결과가 비었을 때 "없음"과 "일치 없음"을 가른다.
        std::size_t total_count { 0 };

        [[nodiscard]] bool operator==(const start_page_view&) const noexcept = default;
    };

    // 경로가 있는지 알려 주는 함수다. 주입해서 뷰 모델을 디스크 없이 test한다.
    using repository_exists_function = std::function<bool(std::u8string_view path)>;

    // 검색어와 맞는지 본다. 앞뒤 공백을 뗀 검색어를 부분 문자열로 찾고, ASCII 대소문자를
    // 무시한다. 한글은 대소문자가 없고 UTF-8이 자기 동기화 형식이라 바이트 비교가 글자 경계에
    // 맞으므로 그대로 찾는다. 검색어의 `/`는 `\`로 본다. 빈 검색어는 모두와 맞는다.
    [[nodiscard]] bool matches_search_text(std::u8string_view text, std::u8string_view search);

    // 설정에서 시작 페이지 목록을 만든다.
    // - 고정한 항목이 먼저이고 설정 목록의 순서를 따른다.
    // - 나머지는 `opened_at` 내림차순이다. 시각이 같거나 없으면 설정 목록의 순서가 이긴다.
    // - `search`가 이름이나 경로와 맞는 항목만 남긴다.
    // - `exists`는 남은 항목에 대해서만 부른다 (디스크 조회를 보이는 만큼만 하려는 것이다).
    [[nodiscard]] start_page_view build_start_page_view(const app_settings& settings, const repository_exists_function& exists, std::u8string_view search = {});
} // namespace cyella
