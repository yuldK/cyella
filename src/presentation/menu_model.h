#pragma once

#include "domain/file_change.h"
#include "domain/ref.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cyella {
    // 우클릭 메뉴와 끌어 놓기 메뉴의 항목을 정하는 순수 규칙이다. 어떤 대상에 어떤 항목이 보이고 무엇이 꺼지는지만
    // 말하고, 항목을 실행하는 일(operation 내기, 대화 상자 열기)은 controller가 맡는다. 키는 controller가 같은
    // 대상으로 이 함수를 다시 불러 항목을 되찾는 열쇠다.

    enum class menu_target_kind
    {
        commit,
        local_branch,
        remote_branch,
        tag,
        stash,
        file,
        // 사이드바 TAGS 머리다.
        tags_section,
        // 브랜치 라벨을 다른 브랜치나 커밋에 놓았다.
        drop,
    };

    // 끌어 놓기의 한쪽이다. 브랜치(`kind`가 local·remote)나 커밋이다.
    struct drop_party
    {
        ref_kind kind { ref_kind::local };
        // 브랜치 짧은 이름이다 (`main`, `origin/main`). 커밋이면 비어 있다.
        std::string name {};
        // 가리키는 커밋 SHA다. 커밋이면 그 커밋이다.
        std::string sha {};
        bool is_commit { false };

        [[nodiscard]] bool operator==(const drop_party&) const noexcept = default;
    };

    struct menu_target
    {
        menu_target_kind kind { menu_target_kind::commit };
        // 브랜치·태그 이름, 파일 경로다.
        std::string name {};
        // 커밋 SHA다. ref면 가리키는 커밋이다.
        std::string sha {};
        std::uint32_t index { 0 };
        // file
        change_side side { change_side::unstaged };
        bool untracked { false };
        bool conflicted { false };
        // drop
        drop_party source {};
        drop_party destination {};

        [[nodiscard]] bool operator==(const menu_target&) const noexcept = default;
    };

    // 두 브랜치의 관계다. 끌어 놓기 메뉴의 조건과 "커밋 N개"의 근거이고, 모르면 빈 값이다.
    struct drop_facts
    {
        // 놓은 쪽(destination)이 끈 쪽(source)의 조상이다 (같으면 참).
        std::optional<bool> destination_is_ancestor {};
        // 끈 쪽이 놓은 쪽의 조상이다 (같으면 참).
        std::optional<bool> source_is_ancestor {};
        // `rev-list --count destination..source`: 끈 쪽에만 있는 커밋 수다.
        std::optional<std::uint32_t> only_in_source {};
        // `rev-list --count source..destination`: 놓은 쪽에만 있는 커밋 수다.
        std::optional<std::uint32_t> only_in_destination {};

        [[nodiscard]] bool complete() const noexcept
        {
            return destination_is_ancestor.has_value() && source_is_ancestor.has_value() && only_in_source.has_value() && only_in_destination.has_value();
        }
    };

    // 항목을 정하는 데 필요한 저장소 상태다. 값 복사라 snapshot이 만들어 test가 직접 채울 수 있다.
    struct menu_context
    {
        std::string current_branch {};
        bool detached { false };
        std::string head_sha {};
        std::vector<ref> refs {};
        std::vector<std::string> remotes {};
        // 작업 트리나 index에 변경이 있다.
        bool worktree_dirty { false };
        // 우클릭한 커밋이 병합 커밋이다 (cherry-pick·revert는 부모를 골라야 한다).
        bool commit_is_merge { false };
        drop_facts facts {};
    };

    struct menu_entry
    {
        // 항목 키다. `menu_select_intent`로 돌아온다.
        std::u8string key {};
        std::u8string label {};
        char32_t icon { 0 };
        bool enabled { true };
        bool danger { false };
        bool separator_above { false };

        [[nodiscard]] bool operator==(const menu_entry&) const noexcept = default;
    };

    struct menu_model
    {
        // 메뉴 맨 위의 눌리지 않는 제목 줄이다. 비어 있으면 없다.
        std::u8string title {};
        std::vector<menu_entry> entries {};
    };

    [[nodiscard]] menu_model build_context_menu(const menu_target& target, const menu_context& context);

    // 놓을 수 있는 쌍인가. 같은 브랜치 위, 원격끼리, 원격이 끈 쪽이면서 원격 대상이 아니면 안 된다.
    [[nodiscard]] bool can_drop_on(const drop_party& source, const drop_party& destination) noexcept;

    // 끄는 동안 대상 위에서 보일 한 줄이다 ("feature를 main 위로 rebase · 커밋 3개"). 놓을 수 없으면 빈 값이다.
    [[nodiscard]] std::optional<std::u8string> describe_drop(const drop_party& source, const drop_party& destination, const std::string& current_branch, const drop_facts& facts);

    // 항목 키의 접두다. `push:<remote>`, `delete_remote:<remote>`처럼 뒤에 원격 이름이 붙는 키를 가른다.
    [[nodiscard]] std::optional<std::string> menu_key_argument(const std::u8string& key, std::u8string_view prefix);

    // 사이드바·그래프 라벨의 끌기 payload 키다 (`l:main`, `rb:origin/main`). 브랜치가 아니면 빈 값이다.
    [[nodiscard]] std::u8string branch_drag_key(ref_kind kind, const std::string& name);
    [[nodiscard]] std::optional<drop_party> parse_branch_drag_key(const std::u8string& key);
} // namespace cyella
