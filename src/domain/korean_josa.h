#pragma once

#include <string>
#include <string_view>

namespace cyella {
    // 이름 뒤에 붙는 조사를 고른다 (문구 틀의 `{name|을/를}`, `text_catalog`). 이름의 마지막 글자가 발음에서 받침으로
    // 끝나는지를 거칠게 짐작한다 (main=받침 있음, graph=없음).
    [[nodiscard]] bool ends_with_batchim(std::u8string_view text) noexcept;
    [[nodiscard]] bool ends_with_rieul(std::u8string_view text) noexcept;
} // namespace cyella
