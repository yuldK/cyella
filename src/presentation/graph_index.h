#pragma once

#include "domain/commit_info.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cyella {
    // 읽어 둔 이력(`repository_session::graph_commits`)의 부모 관계를 담은 값이다. 끌어 놓기가 "B가 A의
    // 조상인가", "커밋 N개"를 git을 다시 부르지 않고 답하는 근거다. 값 타입이라 게시된 tree의 element가
    // `shared_ptr<const graph_index>`로 들고 그리는 thread에서 읽어도 된다.
    //
    // 이력은 topo 순서(자식이 부모보다 앞)로 이어 읽는다. 그래서 읽은 부분은 이력의 **앞쪽 접두**이고, 읽지
    // 않은 커밋은 모두 읽은 커밋보다 뒤에 있다. 두 커밋이 모두 읽혀 있으면 둘 사이의 관계는 읽은 부분만으로
    // 정확히 정해진다 — 부모 쪽으로 가는 길은 색인이 늘기만 하기 때문이다. 하나라도 읽히지 않았으면 모른다
    // (빈 값). 그때는 `is_ancestor`·`count_commits` 질의가 대신한다.
    class graph_index
    {
    public:
        graph_index() = default;
        explicit graph_index(const std::vector<commit_info>& commits);

        [[nodiscard]] std::size_t size() const noexcept
        {
            return shas_.size();
        }

        // 항목 키(SHA 앞 `key_length`자)나 전체 SHA의 앞부분으로 색인을 찾는다. 읽지 않았으면 빈 값이다.
        [[nodiscard]] std::optional<std::size_t> find(std::string_view sha) const;
        [[nodiscard]] const std::string& sha_at(std::size_t index) const noexcept
        {
            return shas_[index];
        }

        // `ancestor`가 `descendant`와 같거나 그 조상인가. 둘 다 읽혀 있을 때만 값이 있다.
        [[nodiscard]] std::optional<bool> is_ancestor(std::string_view ancestor, std::string_view descendant) const;
        // `git rev-list --count from..to`다. `to`에서 닿고 `from`에서 닿지 않는 커밋 수이며, 둘 다 읽혀 있을 때만
        // 값이 있다.
        [[nodiscard]] std::optional<std::uint32_t> count(std::string_view from, std::string_view to) const;

        // 항목 키의 길이다 (`graph_item_key`).
        static constexpr std::size_t key_length { 15 };

    private:
        std::vector<std::string> shas_ {};
        // 색인 → 부모 색인 목록의 시작 위치다 (끝에 한 칸 더 있다). 읽지 않은 부모는 담지 않는다.
        std::vector<std::uint32_t> parents_begin_ {};
        std::vector<std::uint32_t> parent_indices_ {};
        // 항목 키 → 색인이다.
        std::unordered_map<std::string, std::uint32_t> by_key_ {};
    };
} // namespace cyella
