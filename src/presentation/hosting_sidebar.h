#pragma once

#include "presentation/build_view_snapshot.h"
#include "presentation/hosting_view.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace cyella {
    // 사이드바의 PULL REQUESTS 구역이다. `build_sidebar_rows`가 만든 행 뒤에 이어 붙인다. snapshot만 읽는 순수 함수라 UI 없이 test한다.
    //
    // 행 키(`sidebar_row_view::key`)와 동작(`path`, `hosting_row_*`):
    //  - `pr:n:<번호>`: 번호 그대로가 path다. 열면 PR 상세가 선다.
    //  - `pr:login`·`pr:refresh`·`pr:closed`·`pr:more`·`pr:create`: 동작 행이다.
    //  - `pr:info`·`pr:loading`·`pr:error`: 안내 행이라 눌러도 아무 일이 없다.
    void append_hosting_sidebar_rows(sidebar_rows& result, const hosting_repository_view& view, std::u8string_view filter, std::uint8_t collapsed_sections);

    // 행에 붙는 꼬리 글에 쓰는 CI 표시 글자다 ("✓", "✗", "…", "!").
    [[nodiscard]] std::u8string ci_glyph_text(ci_state state);
} // namespace cyella
