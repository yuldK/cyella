#pragma once

#include "presentation/graph_index.h"
#include "presentation/menu_model.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace cyella {
    // 끄는 동안 대상 위에서 보일 문구를 그리는 쪽(UI thread)이 읽는 값이다. 게시된 tree의 element가
    // `shared_ptr<const drop_hint_data>`로 들고 있으므로 세션 내부를 가리키는 포인터는 두지 않는다 — 읽어 둔 이력의 부모
    // 관계(`graph_index`)와 브랜치 표만 값으로 담는다.
    struct drop_hint_data
    {
        std::shared_ptr<const graph_index> index {};
        std::string current_branch {};
        // 끌기 payload 키(`l:main`, `rb:origin/main`) → 브랜치다. 사이드바 행과 그래프 라벨이 같은 키를 쓴다.
        std::unordered_map<std::u8string, drop_party> branches {};

        // 끈 쪽 키와 놓을 자리의 element owner로 문구를 정한다. 모르는 owner거나 놓을 수 없으면 빈 값이다.
        //  - 사이드바 행 owner는 `row-<키>`, 그래프 라벨은 `label-<키>`, 그래프 행(커밋)은 `commit-<SHA 앞 15자>`다.
        [[nodiscard]] std::optional<std::u8string> describe(const std::u8string& source_key, const std::u8string& target_owner) const;

        // 이 쌍의 관계다. 읽어 둔 이력에서 모르면 빈 값이 들어 있다.
        [[nodiscard]] drop_facts facts_for(const drop_party& source, const drop_party& destination) const;
    };

    // 요소 owner 접두다. 이름을 한 곳에서 정해 element와 힌트가 어긋나지 않게 한다.
    inline constexpr std::u8string_view sidebar_row_owner_prefix { u8"row-" };
    inline constexpr std::u8string_view ref_label_owner_prefix { u8"label-" };
    inline constexpr std::u8string_view graph_row_owner_prefix { u8"commit-" };
} // namespace cyella
