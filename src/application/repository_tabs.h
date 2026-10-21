#pragma once

#include "domain/app_settings.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cyella {
    // UI의 key로 쓰는 탭 id다. 단조 증가하며 탭이 닫혀도 다시 쓰지 않는다. 인덱스는 닫기·
    // 순서 바꾸기로 바뀌지만 id는 그대로여서, 탭마다 둔 화면 상태가 엉뚱한 탭에 붙지 않는다.
    using repository_tab_id = std::uint64_t;

    enum class repository_tab_kind
    {
        // 시작 페이지(최근·고정 목록과 열기·Clone·Init)다.
        start,
        repository,
    };

    struct repository_tab
    {
        repository_tab_id id { 0 };
        repository_tab_kind kind { repository_tab_kind::start };
        // `repository`일 때만 값이 있다.
        std::u8string path {};
        std::u8string display_name {};

        [[nodiscard]] bool operator==(const repository_tab&) const noexcept = default;
    };

    struct open_repository_result
    {
        // 저장소가 열려 있는 탭의 id다. 새로 만들었든 기존 탭이든 같다.
        repository_tab_id id { 0 };
        // 새 탭이 만들어졌거나 시작 탭이 바뀌었다. 거짓이면 이미 열려 있던 탭을 골랐다.
        bool created { false };
        // 선택돼 있던 시작 탭을 그 자리에서 저장소 탭으로 바꿨다.
        bool replaced_start_tab { false };
    };

    // 탭 줄의 순수 상태 모델이다. 화면도 파일도 모른다.
    //
    // 불변: 탭은 항상 하나 이상이고, 선택된 탭이 정확히 하나 있다.
    class repository_tabs
    {
    public:
        // 선택된 시작 탭 하나로 시작한다.
        repository_tabs();

        [[nodiscard]] const std::vector<repository_tab>& tabs() const noexcept;
        [[nodiscard]] std::size_t selected_index() const noexcept;
        [[nodiscard]] const repository_tab& selected() const noexcept;
        [[nodiscard]] std::optional<std::size_t> index_of(repository_tab_id id) const noexcept;
        // 같은 저장소(`repository_path_key`가 같은)가 열린 탭의 인덱스다.
        [[nodiscard]] std::optional<std::size_t> find_repository(std::u8string_view path) const;

        // 저장소를 연다.
        // - 이미 열려 있으면 그 탭을 선택하고 새로 만들지 않는다.
        // - 아니고 선택된 탭이 시작 탭이면 그 자리에서 바꾼다 (새 id를 받는다).
        // - 아니면 선택된 탭 바로 오른쪽에 끼우고 선택한다.
        // 빈 경로는 아무것도 하지 않고 선택된 탭을 돌려준다.
        open_repository_result open_repository(std::u8string_view path);
        // "+" 버튼이다. 탭 줄 맨 뒤("+" 단추 바로 앞)에 시작 탭을 더하고 선택한다.
        repository_tab_id new_tab();

        // 바뀐 것이 있으면 true다. 범위 밖이거나 이미 그 탭이면 false다.
        bool select(std::size_t index);
        // 탭을 닫는다. 브라우저처럼 선택된 탭을 닫으면 오른쪽 이웃을, 없으면 왼쪽 이웃을 고른다.
        // 선택되지 않은 탭을 닫으면 선택은 같은 탭에 머문다. 마지막 탭을 닫으면 시작 탭 하나가
        // 남는다. 이미 시작 탭 하나뿐이면 바꿀 것이 없어 false다.
        bool close(std::size_t index);
        // `from`의 탭을 빼서 `to` 인덱스에 놓는다. 선택은 탭을 따라간다.
        bool reorder(std::size_t from, std::size_t to);

        // 세션 복원이다. 지금 탭을 모두 버리고 `paths`로 다시 채운다. 같은 저장소의 중복과
        // 빈 경로는 거른다. 남는 것이 없으면 시작 탭 하나다. `active_path`가 가리키는 탭을
        // 선택하고, 비었거나 없으면 첫 탭이다.
        void restore(const std::vector<std::u8string>& paths, std::u8string_view active_path);
        // 세션에 남길 저장소 경로다. 시작 탭은 빠진다.
        [[nodiscard]] std::vector<std::u8string> repository_paths() const;
        // 선택된 탭이 저장소 탭이면 그 경로, 시작 탭이면 빈 값이다.
        [[nodiscard]] std::u8string active_repository_path() const;

    private:
        repository_tab make_start_tab();
        repository_tab make_repository_tab(std::u8string_view path);

        std::vector<repository_tab> tabs_ {};
        std::size_t selected_ { 0 };
        repository_tab_id next_id_ { 1 };
    };

    // 탭 상태를 설정의 `open_tabs`·`active_tab`에 옮긴다.
    void capture_open_tabs(const repository_tabs& tabs, app_settings& settings);
    // 설정에서 탭 상태를 만든다. 설정이 비어 있으면 시작 탭 하나다.
    [[nodiscard]] repository_tabs restore_tabs(const app_settings& settings);
} // namespace cyella
