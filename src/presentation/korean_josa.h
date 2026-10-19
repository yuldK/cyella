#pragma once

#include <string>
#include <string_view>

namespace cyella {
    // 영어 이름 뒤에 붙는 조사를 고른다. 이름의 마지막 글자가 발음에서 받침으로 끝나는지를 거칠게 짐작한다
    // (main=받침 있음, graph=없음).
    [[nodiscard]] bool ends_with_batchim(std::u8string_view text) noexcept;
    [[nodiscard]] bool ends_with_rieul(std::u8string_view text) noexcept;

    // 이름 + 조사다. 이름이 비어 있으면 조사만 붙지 않게 이름을 그대로 돌려준다.
    [[nodiscard]] std::u8string with_eul_reul(std::u8string_view name);
    [[nodiscard]] std::u8string with_eun_neun(std::u8string_view name);
    [[nodiscard]] std::u8string with_euro_ro(std::u8string_view name);
} // namespace cyella
